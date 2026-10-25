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
 for(int attempt=0;attempt<2;attempt++){
  bool need_priv=false;
  for(int i=0;paths[i];i++){
   int fd=open(paths[i],O_WRONLY|O_NONBLOCK|O_CLOEXEC);
   if(fd>=0)return fd;
   if(errno==EACCES||errno==EPERM||errno==ENOENT)need_priv=true;
  }
  if(attempt==0&&need_priv){
   /* In unprivileged containers or minimal boots without udev ACL rules,
    * /dev/uinput often defaults to 0600 root:root (and /dev/input/uinput
    * may overwrite errno with ENOENT). Ensure the node exists and is
    * accessible if passwordless sudo is available. */
   int r=system("sudo -n mknod -m 0666 /dev/uinput c 10 223 >/dev/null 2>&1; "
                "sudo -n chmod 0666 /dev/uinput /dev/input/uinput >/dev/null 2>&1");
   (void)r;
  }
 }
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

 /* Why we ensure /dev/input/eventN and /run/udev/data/c<maj>:<min> exist:
  * On desktop systems udevd creates /dev/input/eventN and populates the udev
  * database automatically upon UI_DEV_CREATE. In headless containers or raw
  * fbdev boots where udevd is not running, the kernel registers the device in
  * sysfs (/sys/class/input/inputX/eventN) without creating /dev/input/eventN
  * or tagging ID_INPUT_JOYSTICK=1. Creating both when absent ensures SDL2,
  * libudev, GLFW, and raw evdev driving games discover the wheel immediately. */
 if(access(joy->dev_node,R_OK)!=0){
  mkdir("/dev/input",0755);
  unlink(joy->dev_node);
  if(mknod(joy->dev_node,S_IFCHR|0666,makedev((unsigned)joy->major_num,(unsigned)joy->minor_num))==0){
   chmod(joy->dev_node,0666);
   joy->created_node=true;
  }else{
   char cmd[512];
   snprintf(cmd,sizeof(cmd),
            "sudo -n mkdir -p /dev/input && sudo -n rm -f '%s' && "
            "sudo -n mknod -m 0666 '%s' c %d %d && sudo -n chmod 0666 '%s' >/dev/null 2>&1",
            joy->dev_node,joy->dev_node,joy->major_num,joy->minor_num,joy->dev_node);
   if(system(cmd)==0&&access(joy->dev_node,R_OK)==0)joy->created_node=true;
  }
 }else{
  if(chmod(joy->dev_node,0666)!=0){
   char cmd[256];
   snprintf(cmd,sizeof(cmd),"sudo -n chmod 0666 '%s' >/dev/null 2>&1",joy->dev_node);
   int r=system(cmd);
   (void)r;
  }
 }

 snprintf(joy->udev_file,sizeof(joy->udev_file),"/run/udev/data/c%d:%d",joy->major_num,joy->minor_num);
 if(access(joy->udev_file,R_OK)!=0){
  mkdir("/run/udev",0755);
  mkdir("/run/udev/data",0755);
  FILE *uf=fopen(joy->udev_file,"w");
  if(uf){
   fputs("I:1\nE:ID_INPUT=1\nE:ID_INPUT_JOYSTICK=1\nG:input\n",uf);
   fclose(uf);
   joy->created_udev=true;
  }else{
   char cmd[512];
   snprintf(cmd,sizeof(cmd),
            "sudo -n mkdir -p /run/udev/data && "
            "printf 'I:1\\nE:ID_INPUT=1\\nE:ID_INPUT_JOYSTICK=1\\nG:input\\n' | "
            "sudo -n tee '%s' >/dev/null 2>&1",
            joy->udev_file);
   if(system(cmd)==0&&access(joy->udev_file,R_OK)==0)joy->created_udev=true;
  }
 }
}

bool wheel_joystick_init(WheelJoystick *joy){
 if(!joy)return false;
 memset(joy,0,sizeof(*joy));
 joy->uinput_fd=-1;

 int fd=open_uinput_device();
 if(fd<0){
  fprintf(stderr,"[wheel] /dev/uinput unavailable (%s); virtual joystick disabled\n",strerror(errno));
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
 if(joy->dev_node[0])setenv("CHROMEDRIVE_WHEEL_EVDEV",joy->dev_node,1);

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
 if(joy->created_node&&joy->dev_node[0]){
  if(unlink(joy->dev_node)!=0){
   char cmd[256];
   snprintf(cmd,sizeof(cmd),"sudo -n rm -f '%s' >/dev/null 2>&1",joy->dev_node);
   int r=system(cmd);
   (void)r;
  }
 }
 if(joy->created_udev&&joy->udev_file[0]){
  if(unlink(joy->udev_file)!=0){
   char cmd[256];
   snprintf(cmd,sizeof(cmd),"sudo -n rm -f '%s' >/dev/null 2>&1",joy->udev_file);
   int r=system(cmd);
   (void)r;
  }
 }
 unsetenv("CHROMEDRIVE_WHEEL_EVDEV");
 memset(joy,0,sizeof(*joy));
 joy->uinput_fd=-1;
}
