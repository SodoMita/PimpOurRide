/* --windowed dev/debug backend: a real SDL2 window + GLX context on an X
 * server. Kept for local debugging with eyes on a real display; the default
 * production path is backend_headless.c (no X, no window, VNC output).
 * NOTE: dma-buf/EGLImage import is NOT available here — EGLImage does not
 * interoperate with a GLX-created GL context, only with EGL-created ones.
 * s->target_fbo stays 0 (the window's own default framebuffer). */
#include "chrome.h"
#include <linux/input-event-codes.h>

bool backend_sdl_init(Server *s){
 if(SDL_InitSubSystem(SDL_INIT_VIDEO)<0){fprintf(stderr,"SDL video: %s\n",SDL_GetError());return false;}
 /* SDL may report no touch devices until the first finger event on some
  * drivers, so the windowed seat advertises touch unconditionally. Finger
  * events are handled explicitly below; SDL's synthetic touch-mouse events
  * are filtered out to avoid delivering one contact twice. */
 s->touch_available=true;s->sdl_touch_chrome_id=-1;
 fprintf(stderr,"[input] SDL touchscreen capability advertised (%d device(s) currently reported)\n",SDL_GetNumTouchDevices());
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,8);SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,1);SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,4);
 uint32_t flags=SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|(s->fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0);
 s->window=SDL_CreateWindow("ChromeDrive / native Wayland cockpit",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,s->width,s->height,flags);
 if(!s->window){SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS,0);SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES,0);s->window=SDL_CreateWindow("ChromeDrive / native Wayland cockpit",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,s->width,s->height,flags);}
 if(!s->window||!(s->gl=SDL_GL_CreateContext(s->window))){fprintf(stderr,"OpenGL: %s\n",SDL_GetError());return false;}
 SDL_GL_SetSwapInterval(0);SDL_GetWindowSize(s->window,&s->width,&s->height);
 fprintf(stderr,"[backend] windowed/SDL2+GLX — %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
 s->target_fbo=0; /* window's own default framebuffer */
 return true;
}
static int sdl_touch_coord(float value,int extent){
 if(extent<=1)return 0;
 return (int)(clampf(value,0,1)*(float)(extent-1)+.5f);
}
static SdlTouchFinger *sdl_touch_find(Server *s,SDL_TouchID device,SDL_FingerID finger,bool create){
 SdlTouchFinger *free_slot=NULL;
 for(int i=0;i<CD_MAX_TOUCH_POINTS;i++){
  SdlTouchFinger *f=&s->sdl_touch[i];
  if(f->active&&f->device_id==(int64_t)device&&f->finger_id==(int64_t)finger)return f;
  if(!f->active&&!free_slot)free_slot=f;
 }
 if(create&&free_slot){
  memset(free_slot,0,sizeof(*free_slot));
  free_slot->active=true;free_slot->device_id=(int64_t)device;free_slot->finger_id=(int64_t)finger;
  free_slot->id=(int32_t)(free_slot-s->sdl_touch);free_slot->route=CD_TOUCH_ROUTE_NONE;
  return free_slot;
 }
 return NULL;
}
static bool sdl_touch_app_active(Server *s){
 for(int i=0;i<CD_MAX_TOUCH_POINTS;i++)if(s->sdl_touch[i].active&&s->sdl_touch[i].route==CD_TOUCH_ROUTE_APP)return true;
 return false;
}
static void sdl_touch_clear(Server *s,SdlTouchFinger *f){
 int32_t id=f->id;memset(f,0,sizeof(*f));
 if(s->sdl_touch_chrome_id==id)s->sdl_touch_chrome_id=-1;
}
static void sdl_touch_forget_all(Server *s){
 memset(s->sdl_touch,0,sizeof(s->sdl_touch));s->sdl_touch_chrome_id=-1;
}
static void sdl_touch_chrome_down(Server *s,SdlTouchFinger *f,Hit hit,int x,int y,uint32_t now){
 f->route=CD_TOUCH_ROUTE_CHROME;s->sdl_touch_chrome_id=f->id;s->mouse_x=x;s->mouse_y=y;
 if(scene_wheel_button(s,hit,x,y,true,now)){f->route=CD_TOUCH_ROUTE_WHEEL;return;}
 if(s->arrange_mode){app_arrange_pointer_button(s,hit,BTN_LEFT,true);return;}
 if(hit.kind==HIT_FRAME||hit.kind==HIT_HANDLES_BUTTON){app_frame_pointer_button(s,hit,BTN_LEFT,true);return;}
 if(hit.kind==HIT_BUTTON){app_launch(s,hit.button,false);return;}
 if(hit.kind==HIT_CLOSE){server_close_surface(hit.surface);return;}
 if(hit.kind==HIT_COG){app_open_settings(s);return;}
 /* Empty cockpit: one-finger drag looks around, matching middle-drag. */
 f->route=CD_TOUCH_ROUTE_LOOK;server_pointer_motion(s,(Hit){0},now);
}
static void sdl_touch_down(Server *s,const SDL_TouchFingerEvent *ev,uint32_t now){
 if(ev->touchId==SDL_MOUSE_TOUCHID)return;
 int x=sdl_touch_coord(ev->x,s->width),y=sdl_touch_coord(ev->y,s->height);
 SdlTouchFinger *f=sdl_touch_find(s,ev->touchId,ev->fingerId,true);if(!f)return;
 f->last_x=x;f->last_y=y;
 Hit hit=scene_pick(s,x,y,false);
 if(sdl_touch_app_active(s)){
  if(!s->arrange_mode&&hit.kind==HIT_APP&&server_touch_down(s,hit,f->id,now))f->route=CD_TOUCH_ROUTE_APP;
  return; /* extra non-app fingers do not steal an active app gesture */
 }
 if(s->sdl_touch_chrome_id>=0)return; /* one primary cockpit/pointer finger */
 if(s->arrange_mode){
  if(!s->arrange_dragging)sdl_touch_chrome_down(s,f,hit,x,y,now);
  return;
 }
 if(hit.kind==HIT_APP){
  if(server_touch_down(s,hit,f->id,now)){f->route=CD_TOUCH_ROUTE_APP;return;}
  if(s->pointer_grab||s->held_buttons)return;
  f->route=CD_TOUCH_ROUTE_POINTER;s->sdl_touch_chrome_id=f->id;s->mouse_x=x;s->mouse_y=y;
  server_pointer_motion(s,hit,now);server_pointer_button(s,BTN_LEFT,true,now);
  return;
 }
 if(s->wheel_drag||s->frame_dragging)return;
 sdl_touch_chrome_down(s,f,hit,x,y,now);
}
static void sdl_touch_motion(Server *s,const SDL_TouchFingerEvent *ev,uint32_t now){
 if(ev->touchId==SDL_MOUSE_TOUCHID)return;
 SdlTouchFinger *f=sdl_touch_find(s,ev->touchId,ev->fingerId,false);if(!f)return;
 int x=sdl_touch_coord(ev->x,s->width),y=sdl_touch_coord(ev->y,s->height);
 int dx=x-f->last_x,dy=y-f->last_y;
 if(f->route==CD_TOUCH_ROUTE_APP)server_touch_motion(s,f->id,x,y,now);
 else if(f->route==CD_TOUCH_ROUTE_POINTER){
  s->mouse_x=x;s->mouse_y=y;
  Hit hit=scene_pick(s,x,y,s->pointer_grab!=NULL);server_pointer_motion(s,hit,now);
 }else if(f->route==CD_TOUCH_ROUTE_WHEEL){s->mouse_x=x;s->mouse_y=y;scene_wheel_motion(s,x,y,now);}
 else if(f->route==CD_TOUCH_ROUTE_LOOK){
  s->mouse_x=x;s->mouse_y=y;
  s->target_yaw-=dx*.16f;s->target_pitch=clampf(s->target_pitch-dy*.13f,-35,32);s->focus_view=false;
  s->arrange_last_x=x;s->arrange_last_y=y;s->frame_last_x=x;s->frame_last_y=y;
 }else if(f->route==CD_TOUCH_ROUTE_CHROME){
  s->mouse_x=x;s->mouse_y=y;
  if(s->frame_dragging)app_frame_pointer_motion(s,x,y);
  else if(s->arrange_mode)app_arrange_pointer_motion(s,x,y);
 }
 f->last_x=x;f->last_y=y;
}
static void sdl_touch_up(Server *s,const SDL_TouchFingerEvent *ev,uint32_t now){
 if(ev->touchId==SDL_MOUSE_TOUCHID)return;
 SdlTouchFinger *f=sdl_touch_find(s,ev->touchId,ev->fingerId,false);if(!f)return;
 int x=sdl_touch_coord(ev->x,s->width),y=sdl_touch_coord(ev->y,s->height);
 if(f->route==CD_TOUCH_ROUTE_APP)server_touch_up(s,f->id,now);
 else if(f->route==CD_TOUCH_ROUTE_POINTER){
  s->mouse_x=x;s->mouse_y=y;
  Hit hit=scene_pick(s,x,y,s->pointer_grab!=NULL);server_pointer_motion(s,hit,now);
  server_pointer_button(s,BTN_LEFT,false,now);
 }else if(f->route==CD_TOUCH_ROUTE_WHEEL){s->mouse_x=x;s->mouse_y=y;scene_wheel_button(s,(Hit){0},x,y,false,now);}
 else if(f->route==CD_TOUCH_ROUTE_CHROME){
  s->mouse_x=x;s->mouse_y=y;Hit hit=scene_pick(s,x,y,false);
  if(s->arrange_mode)app_arrange_pointer_button(s,hit,BTN_LEFT,false);
  else if(s->frame_dragging)app_frame_pointer_button(s,hit,BTN_LEFT,false);
 }
 sdl_touch_clear(s,f);
}
void backend_sdl_poll(Server *s){
 SDL_Event e;while(SDL_PollEvent(&e)){
  uint32_t now=SDL_GetTicks();
  if(e.type==SDL_QUIT)s->running=false;
  else if(e.type==SDL_WINDOWEVENT){if(e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED)SDL_GetWindowSize(s->window,&s->width,&s->height);if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST){app_release_input(s,now);server_release_pointer_buttons(s,now);sdl_touch_forget_all(s);s->middle_look=false;s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;s->frame_dragging=false;s->frame_drag_slot=-1;}}
  else if(e.type==SDL_KEYDOWN||e.type==SDL_KEYUP){
   if(e.key.repeat)continue; /* client-side xkb repeat handles visual repetition, same as a real compositor */
   uint32_t k=sdl_to_evdev(e.key.keysym.scancode);if(!k)continue;
   bool down=e.type==SDL_KEYDOWN;
   if(!app_global_key(s,k,down))server_key(s,k,down,now);
  }
  else if(e.type==SDL_MOUSEMOTION){if(e.motion.which==SDL_TOUCH_MOUSEID)continue;s->mouse_x=e.motion.x;s->mouse_y=e.motion.y;
   if(s->wheel_drag){scene_wheel_motion(s,e.motion.x,e.motion.y,now);continue;}
   if(s->middle_look){s->target_yaw-=e.motion.xrel*.16f;s->target_pitch=clampf(s->target_pitch-e.motion.yrel*.13f,-35,32);s->focus_view=false;s->arrange_last_x=s->mouse_x;s->arrange_last_y=s->mouse_y;s->frame_last_x=s->mouse_x;s->frame_last_y=s->mouse_y;}
   else if(s->frame_dragging)app_frame_pointer_motion(s,s->mouse_x,s->mouse_y);
   else if(s->arrange_mode)app_arrange_pointer_motion(s,s->mouse_x,s->mouse_y);
   else {Hit hit=scene_pick(s,s->mouse_x,s->mouse_y,s->pointer_grab!=NULL);server_pointer_motion(s,hit,now);}
  }else if(e.type==SDL_MOUSEBUTTONDOWN||e.type==SDL_MOUSEBUTTONUP){
   if(e.button.which==SDL_TOUCH_MOUSEID)continue;
   bool down=e.type==SDL_MOUSEBUTTONDOWN;s->mouse_x=e.button.x;s->mouse_y=e.button.y;
   if(e.button.button==SDL_BUTTON_MIDDLE){s->middle_look=down;if(down)server_pointer_motion(s,(Hit){0},now);continue;}
   Hit hit=scene_pick(s,s->mouse_x,s->mouse_y,s->pointer_grab!=NULL);
   if(e.button.button==SDL_BUTTON_LEFT&&scene_wheel_button(s,hit,e.button.x,e.button.y,down,now))continue;
   uint32_t button=e.button.button==SDL_BUTTON_LEFT?BTN_LEFT:e.button.button==SDL_BUTTON_RIGHT?BTN_RIGHT:e.button.button==SDL_BUTTON_X1?BTN_SIDE:BTN_EXTRA;
   if(s->arrange_mode){app_arrange_pointer_button(s,hit,button,down);continue;}
   if(s->frame_dragging||hit.kind==HIT_FRAME||hit.kind==HIT_HANDLES_BUTTON){app_frame_pointer_button(s,hit,button,down);continue;}
   server_pointer_motion(s,hit,now);
   if(down&&e.button.button==SDL_BUTTON_LEFT){if(hit.kind==HIT_BUTTON){app_launch(s,hit.button,(SDL_GetModState()&KMOD_SHIFT)!=0);continue;}if(hit.kind==HIT_CLOSE){server_close_surface(hit.surface);continue;}if(hit.kind==HIT_COG){app_open_settings(s);continue;}}
   if(down&&e.button.button==SDL_BUTTON_RIGHT&&hit.kind==HIT_BUTTON){app_open_settings_slot(s,hit.button,true);continue;}
   if(hit.kind==HIT_APP||s->pointer_grab)server_pointer_button(s,button,down,now);
  }else if(e.type==SDL_MOUSEWHEEL){if(e.wheel.which==SDL_TOUCH_MOUSEID)continue;if(s->arrange_mode)app_arrange_pointer_wheel(s,e.wheel.preciseY);else{Hit hit=scene_pick(s,s->mouse_x,s->mouse_y,false);if(hit.kind==HIT_APP)server_pointer_axis(s,e.wheel.preciseX,e.wheel.preciseY,now);else s->target_fov=clampf(s->target_fov-e.wheel.preciseY*2,CD_DRIVER_FOV_MIN,CD_DRIVER_SCROLL_FOV_MAX);}}
  else if(e.type==SDL_FINGERDOWN)sdl_touch_down(s,&e.tfinger,now);
  else if(e.type==SDL_FINGERMOTION)sdl_touch_motion(s,&e.tfinger,now);
  else if(e.type==SDL_FINGERUP)sdl_touch_up(s,&e.tfinger,now);
 }
}
void backend_sdl_present(Server *s){SDL_GL_SwapWindow(s->window);}
void backend_sdl_shutdown(Server *s){if(s->gl)SDL_GL_DeleteContext(s->gl);if(s->window)SDL_DestroyWindow(s->window);}
