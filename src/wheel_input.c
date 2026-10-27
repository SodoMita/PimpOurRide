#define _DEFAULT_SOURCE
#include "wheel_input.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static float clamp_f(float v,float lo,float hi){
 if(!(v==v))return 0.0f;
 return v<lo?lo:(v>hi?hi:v);
}

float wheel_client_angle_step(float angle_deg,float drag_dx){
 if(!(angle_deg==angle_deg))angle_deg=0.0f;
 if(!(drag_dx==drag_dx))drag_dx=0.0f;
 return clamp_f(angle_deg-drag_dx*0.65f,-CD_WHEEL_MAX_ANGLE,CD_WHEEL_MAX_ANGLE);
}

float wheel_client_steer_from_angle(float angle_deg){
 if(!(angle_deg==angle_deg))return 0.0f;
 float steer=clamp_f(-angle_deg/CD_WHEEL_MAX_ANGLE,-1.0f,1.0f);
 return steer==0.0f?0.0f:steer;
}

int wheel_client_joy_value(float angle_deg){
 float steer=wheel_client_steer_from_angle(angle_deg);
 long v=lrintf(steer*(float)CD_WHEEL_JOY_MAX);
 if(v<CD_WHEEL_JOY_MIN)v=CD_WHEEL_JOY_MIN;
 if(v>CD_WHEEL_JOY_MAX)v=CD_WHEEL_JOY_MAX;
 return (int)v;
}

void wheel_client_axis(float drag_dx,float *horizontal,float *vertical){
 float h=0.0f;
 if(drag_dx!=0.0f&&drag_dx==drag_dx)h=-drag_dx/28.0f;
 if(horizontal)*horizontal=h==0.0f?0.0f:h;
 if(vertical)*vertical=0.0f;
}

int wheel_client_scroll_steps(double *accum,double delta){
 if(!accum)return 0;
 if(!(delta==delta)||delta==0.0)return 0;
 *accum+=delta/15.0;
 int steps=(int)trunc(*accum);
 if(steps!=0)*accum-=(double)steps;
 if(fabs(*accum)<1e-12)*accum=0.0;
 return steps;
}

static bool uinput_emit(int fd,uint16_t type,uint16_t code,int32_t value){
 if(fd<0)return false;
 struct input_event ev;
 memset(&ev,0,sizeof(ev));
 ev.type=type;
 ev.code=code;
 ev.value=value;
 return write(fd,&ev,sizeof(ev))==(ssize_t)sizeof(ev);
}

static int open_uinput_device(void){
 static const char *const paths[]={"/dev/uinput","/dev/input/uinput",NULL};
 int saved_errno=0;
 for(int i=0;paths[i];i++){
  int fd=open(paths[i],O_WRONLY|O_NONBLOCK|O_CLOEXEC);
  if(fd>=0)return fd;
  int e=errno;
  /* Preserve a concrete permission/other failure over an earlier missing
   * alternate path; only a genuinely missing pair earns the mknod attempt. */
  if(!saved_errno||saved_errno==ENOENT)saved_errno=e;
 }

 /* A missing node can be repaired directly on a minimal/root or suitably
  * capability-enabled boot. Never invoke sudo, an askpass helper, or any
  * other subprocess here: this runs on the compositor thread, where waiting
  * for authentication would also stop evdev dispatch and managed VT escapes.
  * Permission failure is deliberately soft; host device policy belongs to
  * setup/udev, not to an interactive prompt hidden behind the cockpit. */
 if(saved_errno==ENOENT){
  bool created=false;
  if(mknod("/dev/uinput",S_IFCHR|0600,makedev(10,223))==0)created=true;
  if(created||errno==EEXIST){
   int fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
   if(fd>=0)return fd;
   saved_errno=errno;
   if(created)unlink("/dev/uinput");
  }else saved_errno=errno;
 }
 errno=saved_errno;
 return -1;
}

static void setup_abs_axis(int fd,uint16_t code,int32_t min_v,int32_t max_v){
 ioctl(fd,UI_SET_ABSBIT,code);
 struct uinput_abs_setup abs;
 memset(&abs,0,sizeof(abs));
 abs.code=code;
 abs.absinfo.minimum=min_v;
 abs.absinfo.maximum=max_v;
 abs.absinfo.fuzz=0;
 abs.absinfo.flat=0;
 abs.absinfo.value=0;
 ioctl(fd,UI_ABS_SETUP,&abs);
}

static void ensure_event_node_and_udev(WheelJoystick *joy){
 if(!joy->sysname[0])return;
 char sysdir[160];
 snprintf(sysdir,sizeof(sysdir),"/sys/class/input/%s",joy->sysname);
 for(int wait_step=0;wait_step<25;wait_step++){
  DIR *d=opendir(sysdir);
  if(d){
   struct dirent *de;
   while((de=readdir(d))!=NULL){
    if(strncmp(de->d_name,"event",5)!=0)continue;
    char devfile[256];
    snprintf(devfile,sizeof(devfile),"%s/%.64s/dev",sysdir,de->d_name);
    FILE *f=fopen(devfile,"r");
    int maj=0,min=0;
    if(f&&fscanf(f,"%d:%d",&maj,&min)==2){
     fclose(f);
     joy->major_num=maj;
     joy->minor_num=min;
     snprintf(joy->dev_node,sizeof(joy->dev_node),"/dev/input/%.64s",de->d_name);
     break;
    }
    if(f)fclose(f);
   }
   closedir(d);
  }
  if(joy->dev_node[0])break;
  struct timespec ts={.tv_sec=0,.tv_nsec=2000000};
  nanosleep(&ts,NULL);
 }
 if(!joy->dev_node[0])return;

 /* Why we best-effort prepare /dev/input/eventN and
  * /run/udev/data/c<maj>:<min>: desktop udevd normally creates both after
  * UI_DEV_CREATE. A minimal boot without udevd may expose only sysfs. Direct
  * mknod/file creation preserves that container use case, but an unprivileged
  * failure is not a reason to run a privilege helper or to chmod/unlink a
  * pre-existing host node. In particular, no language/config reload or VT
  * signal can ever become stuck behind an authentication process here. */
 if(access(joy->dev_node,R_OK)!=0){
  struct stat st;
  if(lstat(joy->dev_node,&st)!=0&&errno==ENOENT){
   mkdir("/dev/input",0755);
   if(mknod(joy->dev_node,S_IFCHR|0600,
            makedev((unsigned)joy->major_num,(unsigned)joy->minor_num))==0)
    joy->created_node=true;
  }
  if(access(joy->dev_node,R_OK)!=0)
   fprintf(stderr,"[wheel] evdev node %s is not accessible (%s); leaving host permissions unchanged\n",
           joy->dev_node,strerror(errno));
 }

 snprintf(joy->udev_file,sizeof(joy->udev_file),"/run/udev/data/c%d:%d",joy->major_num,joy->minor_num);
 if(access(joy->udev_file,R_OK)!=0){
  struct stat st;
  if(lstat(joy->udev_file,&st)!=0&&errno==ENOENT){
   mkdir("/run/udev",0755);
   mkdir("/run/udev/data",0755);
   FILE *uf=fopen(joy->udev_file,"wx"); /* never truncate an existing host tag */
   if(uf){
    bool wrote=fputs("I:1\nE:ID_INPUT=1\nE:ID_INPUT_JOYSTICK=1\nG:input\n",uf)>=0;
    if(fclose(uf)==0&&wrote)joy->created_udev=true;
    else unlink(joy->udev_file);
   }
  }
  if(!joy->created_udev)
   fprintf(stderr,"[wheel] could not write udev joystick tag %s (%s); leaving host configuration unchanged\n",
           joy->udev_file,strerror(errno));
 }
}

bool wheel_joystick_init(WheelJoystick *joy){
 if(!joy)return false;
 memset(joy,0,sizeof(*joy));
 joy->uinput_fd=-1;

 int fd=open_uinput_device();
 if(fd<0){
  fprintf(stderr,"[wheel] /dev/uinput unavailable (%s); virtual joystick disabled without blocking compositor input\n",strerror(errno));
  return false;
 }

 ioctl(fd,UI_SET_EVBIT,EV_KEY);
 ioctl(fd,UI_SET_EVBIT,EV_ABS);
 ioctl(fd,UI_SET_EVBIT,EV_SYN);

 static const uint16_t buttons[]={
  BTN_SOUTH,BTN_EAST,BTN_NORTH,BTN_WEST,
  BTN_TL,BTN_TR,BTN_SELECT,BTN_START,BTN_MODE,
  BTN_THUMBL,BTN_THUMBR
 };
 for(size_t i=0;i<sizeof(buttons)/sizeof(buttons[0]);i++)
  ioctl(fd,UI_SET_KEYBIT,buttons[i]);

 /* Expose both standard gamepad axes (ABS_X = SDL_CONTROLLER_AXIS_LEFTX /
  * joystick axis 0) and the dedicated racing-wheel axis (ABS_WHEEL) so
  * both GameController-based and raw evdev driving games bind steering. */
 setup_abs_axis(fd,ABS_X,CD_WHEEL_JOY_MIN,CD_WHEEL_JOY_MAX);
 setup_abs_axis(fd,ABS_Y,CD_WHEEL_JOY_MIN,CD_WHEEL_JOY_MAX);
 setup_abs_axis(fd,ABS_Z,0,255);
 setup_abs_axis(fd,ABS_RX,CD_WHEEL_JOY_MIN,CD_WHEEL_JOY_MAX);
 setup_abs_axis(fd,ABS_RY,CD_WHEEL_JOY_MIN,CD_WHEEL_JOY_MAX);
 setup_abs_axis(fd,ABS_RZ,0,255);
 setup_abs_axis(fd,ABS_WHEEL,CD_WHEEL_JOY_MIN,CD_WHEEL_JOY_MAX);
 setup_abs_axis(fd,ABS_HAT0X,-1,1);
 setup_abs_axis(fd,ABS_HAT0Y,-1,1);

 struct uinput_setup usetup;
 memset(&usetup,0,sizeof(usetup));
 usetup.id.bustype=BUS_USB;
 usetup.id.vendor=CD_WHEEL_VENDOR_ID;
 usetup.id.product=CD_WHEEL_PRODUCT_ID;
 usetup.id.version=CD_WHEEL_VERSION_ID;
 snprintf(usetup.name,sizeof(usetup.name),"%s",CD_WHEEL_DEVICE_NAME);

 if(ioctl(fd,UI_DEV_SETUP,&usetup)<0||ioctl(fd,UI_DEV_CREATE)<0){
  fprintf(stderr,"[wheel] uinput device creation failed: %s\n",strerror(errno));
  close(fd);
  return false;
 }

 joy->uinput_fd=fd;
 joy->active=true;
 if(ioctl(fd,UI_GET_SYSNAME(sizeof(joy->sysname)),joy->sysname)<0)
  joy->sysname[0]=0;

 ensure_event_node_and_udev(joy);
 if(!joy->dev_node[0]||access(joy->dev_node,R_OK)!=0){
  fprintf(stderr,"[wheel] no accessible evdev node for the virtual joystick; disabling it without blocking compositor input\n");
  wheel_joystick_finish(joy);
  return false;
 }
 setenv("CHROMEDRIVE_WHEEL_EVDEV",joy->dev_node,1);

 uinput_emit(fd,EV_ABS,ABS_X,0);
 uinput_emit(fd,EV_ABS,ABS_WHEEL,0);
 uinput_emit(fd,EV_SYN,SYN_REPORT,0);
 joy->last_abs_x=0;

 fprintf(stderr,"[wheel] virtual joystick '%s' ready on %s\n",
         CD_WHEEL_DEVICE_NAME,joy->dev_node[0]?joy->dev_node:"uinput");
 return true;
}

bool wheel_joystick_update_angle(WheelJoystick *joy,float angle_deg){
 if(!joy||!joy->active||joy->uinput_fd<0)return false;
 int val=wheel_client_joy_value(angle_deg);
 if(val==joy->last_abs_x)return true;
 bool ok=uinput_emit(joy->uinput_fd,EV_ABS,ABS_X,val)&&
         uinput_emit(joy->uinput_fd,EV_ABS,ABS_WHEEL,val)&&
         uinput_emit(joy->uinput_fd,EV_SYN,SYN_REPORT,0);
 if(ok)joy->last_abs_x=val;
 return ok;
}

void wheel_joystick_finish(WheelJoystick *joy){
 if(!joy)return;
 if(joy->uinput_fd>=0){
  uinput_emit(joy->uinput_fd,EV_ABS,ABS_X,0);
  uinput_emit(joy->uinput_fd,EV_ABS,ABS_WHEEL,0);
  uinput_emit(joy->uinput_fd,EV_SYN,SYN_REPORT,0);
  ioctl(joy->uinput_fd,UI_DEV_DESTROY);
  close(joy->uinput_fd);
 }
 if(joy->created_node&&joy->dev_node[0]&&unlink(joy->dev_node)!=0)
  fprintf(stderr,"[wheel] could not remove temporary evdev node %s: %s\n",
          joy->dev_node,strerror(errno));
 if(joy->created_udev&&joy->udev_file[0]&&unlink(joy->udev_file)!=0)
  fprintf(stderr,"[wheel] could not remove temporary udev tag %s: %s\n",
          joy->udev_file,strerror(errno));
 unsetenv("CHROMEDRIVE_WHEEL_EVDEV");
 memset(joy,0,sizeof(*joy));
 joy->uinput_fd=-1;
}
