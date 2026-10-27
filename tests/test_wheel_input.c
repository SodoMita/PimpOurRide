#define _DEFAULT_SOURCE
#include "wheel_input.h"
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, msg) \
 do { \
  if(!(cond)){ \
   fprintf(stderr,"FAIL: %s (%s:%d)\n",msg,__FILE__,__LINE__); \
   failures++; \
  } \
 } while(0)

static void test_steering_math(void){
 CHECK(wheel_client_joy_value(0.0f)==0,"centered wheel maps to 0");
 CHECK(wheel_client_joy_value(-135.0f)==32767,"full right lock (-135 deg) maps to +32767");
 CHECK(wheel_client_joy_value(135.0f)==-32767,"full left lock (+135 deg) maps to -32767");
 CHECK(wheel_client_joy_value(-67.5f)==16384,"half right lock (-67.5 deg) maps to +16384");
 CHECK(wheel_client_joy_value(67.5f)==-16384,"half left lock (+67.5 deg) maps to -16384");
 CHECK(wheel_client_joy_value(-500.0f)==32767,"over-rotated right clamps to +32767");
 CHECK(wheel_client_joy_value(500.0f)==-32767,"over-rotated left clamps to -32767");
 CHECK(wheel_client_joy_value(NAN)==0,"NaN angle safely maps to 0");

 float angle=0.0f;
 angle=wheel_client_angle_step(angle,100.0f);
 CHECK(fabsf(angle - (-65.0f))<1e-5f,"dragging right rotates wheel clockwise (-65 deg)");
 CHECK(wheel_client_steer_from_angle(angle)>0.48f,"clockwise wheel produces positive right steering");
 angle=wheel_client_angle_step(angle,-100.0f);
 CHECK(fabsf(angle)<1e-5f,"equal opposite drag returns wheel to exact center");
 CHECK(wheel_client_joy_value(angle)==0,"returned center maps to joystick 0");

 /* Monotonicity across full lock-to-lock sweep from left (+135) to right (-135) */
 int prev=wheel_client_joy_value(135.0f);
 for(int deg=134;deg>=-135;deg--){
  int cur=wheel_client_joy_value((float)deg);
  CHECK(cur>prev,"joystick axis increases strictly from full left to full right");
  prev=cur;
 }
}

static void test_uinput_and_sdl_joystick(void){
 WheelJoystick joy;
 if(!wheel_joystick_init(&joy)){
  fprintf(stderr,"SKIP: /dev/uinput or its evdev node is not accessible in this environment\n");
  return;
 }
 CHECK(joy.active && joy.uinput_fd>=0,"virtual joystick is active");
 CHECK(joy.dev_node[0]!=0 && access(joy.dev_node,R_OK)==0,"evdev device node exists and is readable");
 const char *env_node=getenv("CHROMEDRIVE_WHEEL_EVDEV");
 CHECK(env_node && strcmp(env_node,joy.dev_node)==0,"CHROMEDRIVE_WHEEL_EVDEV points to device node");

 int ev_fd=open(joy.dev_node,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
 CHECK(ev_fd>=0,"opened virtual joystick evdev node");
 if(ev_fd>=0){
  struct input_id id={0};
  char name[128]={0};
  struct input_absinfo abs_x={0},abs_wheel={0};
  CHECK(ioctl(ev_fd,EVIOCGID,&id)==0,"queried EVIOCGID");
  CHECK(id.vendor==CD_WHEEL_VENDOR_ID && id.product==CD_WHEEL_PRODUCT_ID,
        "USB vendor/product match standard GameController ID");
  CHECK(ioctl(ev_fd,EVIOCGNAME(sizeof(name)),name)>=0,"queried EVIOCGNAME");
  CHECK(strcmp(name,CD_WHEEL_DEVICE_NAME)==0,"device name matches ChromeDrive Virtual Steering Wheel");
  CHECK(ioctl(ev_fd,EVIOCGABS(ABS_X),&abs_x)==0,"queried EVIOCGABS(ABS_X)");
  CHECK(abs_x.minimum==CD_WHEEL_JOY_MIN && abs_x.maximum==CD_WHEEL_JOY_MAX,
        "ABS_X range is [-32767, +32767]");
  CHECK(ioctl(ev_fd,EVIOCGABS(ABS_WHEEL),&abs_wheel)==0,"queried EVIOCGABS(ABS_WHEEL)");
  CHECK(abs_wheel.minimum==CD_WHEEL_JOY_MIN && abs_wheel.maximum==CD_WHEEL_JOY_MAX,
        "ABS_WHEEL range is [-32767, +32767]");

  /* Drain any initial events before testing a live steering update */
  struct input_event ev;
  while(read(ev_fd,&ev,sizeof(ev))==(ssize_t)sizeof(ev)){}

  CHECK(wheel_joystick_update_angle(&joy,-67.5f),"emitted half-right steering angle (-67.5 deg)");
  bool got_abs_x=false,got_abs_wheel=false,got_syn=false,got_rel_scroll=false;
  while(read(ev_fd,&ev,sizeof(ev))==(ssize_t)sizeof(ev)){
   if(ev.type==EV_ABS && ev.code==ABS_X && ev.value==16384)got_abs_x=true;
   if(ev.type==EV_ABS && ev.code==ABS_WHEEL && ev.value==16384)got_abs_wheel=true;
   if(ev.type==EV_SYN && ev.code==SYN_REPORT)got_syn=true;
   if(ev.type==EV_REL)got_rel_scroll=true;
  }
  CHECK(got_abs_x,"evdev emitted EV_ABS ABS_X = 16384");
  CHECK(got_abs_wheel,"evdev emitted EV_ABS ABS_WHEEL = 16384");
  CHECK(got_syn,"evdev emitted EV_SYN SYN_REPORT");
  CHECK(!got_rel_scroll,"evdev never emits relative scroll events");
  close(ev_fd);
 }

 if(SDL_Init(SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER)==0){
  /* Several pads can exist on a real host (another ChromeDrive instance, a
   * physical Xbox pad): identify ours by its evdev node, never by index. */
  int idx=-1;
  int n=SDL_NumJoysticks();
  for(int i=0;i<n;i++){
#if SDL_VERSION_ATLEAST(2,0,22)
   const char *path=SDL_JoystickPathForIndex(i);
   if(path&&strcmp(path,joy.dev_node)==0){idx=i;break;}
#else
   idx=0; /* SDL < 2.0.22 has no device path to match on */
   break;
#endif
  }
  CHECK(idx>=0,"SDL2 detects the virtual steering-wheel joystick");
  if(idx>=0){
   CHECK(SDL_IsGameController(idx)==SDL_TRUE,"SDL2 recognizes the wheel as an SDL_GameController");
   SDL_GameController *gc=SDL_GameControllerOpen(idx);
   SDL_Joystick *js=SDL_JoystickOpen(idx);
   CHECK(gc!=NULL && js!=NULL,"opened SDL_GameController and SDL_Joystick");
   if(gc && js){
    wheel_joystick_update_angle(&joy,-135.0f);
    SDL_PumpEvents();
    SDL_GameControllerUpdate();
    CHECK(SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_LEFTX)>=32766,
          "SDL_GameController LEFTX reaches +32767 on full right turn");
    CHECK(SDL_JoystickGetAxis(js,0)>=32766,
          "SDL_Joystick axis 0 reaches +32767 on full right turn");

    wheel_joystick_update_angle(&joy,135.0f);
    SDL_PumpEvents();
    SDL_GameControllerUpdate();
    CHECK(SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_LEFTX)<=-32766,
          "SDL_GameController LEFTX reaches -32767 on full left turn");
    CHECK(SDL_JoystickGetAxis(js,0)<=-32766,
          "SDL_Joystick axis 0 reaches -32767 on full left turn");

    wheel_joystick_update_angle(&joy,0.0f);
    SDL_PumpEvents();
    SDL_GameControllerUpdate();
    CHECK(SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_LEFTX)==0,
          "SDL_GameController LEFTX returns to 0 on center");
    CHECK(SDL_JoystickGetAxis(js,0)==0,
          "SDL_Joystick axis 0 returns to 0 on center");
   }
   if(gc)SDL_GameControllerClose(gc);
   if(js)SDL_JoystickClose(js);
  }
  SDL_Quit();
 }

 wheel_joystick_finish(&joy);
 CHECK(!joy.active && joy.uinput_fd==-1,"virtual joystick cleanly destroyed");
}

int main(void){
 test_steering_math();
 test_uinput_and_sdl_joystick();
 if(failures)return 1;
 puts("PASS: virtual steering-wheel joystick math, /dev/uinput evdev ABS_X/ABS_WHEEL emission, and SDL2 GameController/Joystick integration");
 return 0;
}
