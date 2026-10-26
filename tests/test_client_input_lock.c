/* Regression test for the compositor-local Meta+ScrollLock ownership toggle.
 *
 * Including main.c keeps the production entry point and toggle static. The
 * target uses function/data sections + --gc-sections, so only the toggle and
 * its small arrange/notice call graph survive; external Wayland/scene hooks
 * below are deterministic test doubles. This deliberately tests the real
 * toggle rather than a second implementation of its camera policy. */
#define main chromedrive_program_main
#include "../src/main.c"
#undef main

#include <stdio.h>

static int failures,focus_calls;
#define CHECK(cond) do{if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;}}while(0)

const char *tr(const char *english){return english;}
Surface *surface_root(Surface *surface){return surface;}
void server_release_pointer_buttons(Server *s,uint32_t time){(void)s;(void)time;}
void server_touch_cancel(Server *s){(void)s;}
void scene_wheel_cancel(Server *s){(void)s;}
void scene_focus_monitor(Server *s,int slot,bool zoom){
 (void)slot;(void)zoom;focus_calls++;
 /* Match the production function's camera side effects: if the toggle ever
  * calls this again, both the explicit counter and snapshots detect it. */
 s->camera_mode=VEHICLE_CAMERA_DRIVER;s->target_yaw=0;s->target_pitch=-6;s->target_fov=65;s->focus_view=false;
}
void server_focus(Server *s,Surface *p){s->keyboard_focus=p;}
Hit scene_pick(Server *s,float x,float y,bool grab){
 (void)x;(void)y;(void)grab;
 Surface *root=s->client_input_lock;
 return (Hit){.kind=HIT_APP,.surface=root,.slot=root?root->slot:-1};
}
void server_pointer_motion(Server *s,Hit hit,uint32_t time){(void)s;(void)hit;(void)time;}

typedef struct {
 VehicleCameraMode mode;VehicleSeat seat;Vec3 eye;
 float yaw,pitch,target_yaw,target_pitch,fov,target_fov,base_fov;
 bool focus_view;
} CameraSnapshot;

static CameraSnapshot camera_snapshot(const Server *s){
 CameraSnapshot c={0};
 c.mode=s->camera_mode;c.seat=s->camera_seat;c.eye=s->eye;
 c.yaw=s->yaw;c.pitch=s->pitch;c.target_yaw=s->target_yaw;c.target_pitch=s->target_pitch;
 c.fov=s->fov;c.target_fov=s->target_fov;c.base_fov=s->base_fov;c.focus_view=s->focus_view;
 return c;
}

int main(void){
 Server s={0};Surface root={.server=&s,.mapped=true,.slot=2,.width=1000,.height=625};
 s.keyboard_focus=&root;s.camera_mode=VEHICLE_CAMERA_CHASE;s.camera_seat=VEHICLE_SEAT_PASSENGER;
 s.eye=v3(8.2f,2.3f,-10.4f);s.yaw=41;s.pitch=-13;s.target_yaw=47;s.target_pitch=-17;
 s.fov=23;s.target_fov=29;s.base_fov=61;s.focus_view=true;s.arrange_mode=true;
 CameraSnapshot before=camera_snapshot(&s),after;

 app_toggle_client_input_lock(&s);after=camera_snapshot(&s);
 CHECK(s.client_input_lock==&root);CHECK(focus_calls==0);
 CHECK(!memcmp(&before,&after,sizeof(before)));CHECK(!s.arrange_mode);
 app_toggle_client_input_lock(&s);after=camera_snapshot(&s);
 CHECK(!s.client_input_lock);CHECK(focus_calls==0);CHECK(!memcmp(&before,&after,sizeof(before)));

 /* A rejected activation must be camera-neutral too. */
 s.keyboard_focus=NULL;before=camera_snapshot(&s);app_toggle_client_input_lock(&s);after=camera_snapshot(&s);
 CHECK(!s.client_input_lock);CHECK(focus_calls==0);CHECK(!memcmp(&before,&after,sizeof(before)));

 if(failures)return 1;
 puts("PASS: Meta+ScrollLock toggles input ownership without changing camera state");
 return 0;
}
