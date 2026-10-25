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
void backend_sdl_poll(Server *s){
 SDL_Event e;while(SDL_PollEvent(&e)){
  uint32_t now=SDL_GetTicks();
  if(e.type==SDL_QUIT)s->running=false;
  else if(e.type==SDL_WINDOWEVENT){if(e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED)SDL_GetWindowSize(s->window,&s->width,&s->height);if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST){app_release_input(s,now);s->middle_look=false;s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;s->frame_dragging=false;s->frame_drag_slot=-1;}}
  else if(e.type==SDL_KEYDOWN||e.type==SDL_KEYUP){
   if(e.key.repeat)continue; /* client-side xkb repeat handles visual repetition, same as a real compositor */
   uint32_t k=sdl_to_evdev(e.key.keysym.scancode);if(!k)continue;
   bool down=e.type==SDL_KEYDOWN;
   if(!app_global_key(s,k,down))server_key(s,k,down,now);
  }
  else if(e.type==SDL_MOUSEMOTION){s->mouse_x=e.motion.x;s->mouse_y=e.motion.y;
   if(s->wheel_drag){scene_wheel_motion(s,e.motion.x,e.motion.y,now);continue;}
   if(s->middle_look){s->target_yaw-=e.motion.xrel*.16f;s->target_pitch=clampf(s->target_pitch-e.motion.yrel*.13f,-35,32);s->focus_view=false;s->arrange_last_x=s->mouse_x;s->arrange_last_y=s->mouse_y;s->frame_last_x=s->mouse_x;s->frame_last_y=s->mouse_y;}
   else if(s->frame_dragging)app_frame_pointer_motion(s,s->mouse_x,s->mouse_y);
   else if(s->arrange_mode)app_arrange_pointer_motion(s,s->mouse_x,s->mouse_y);
   else {Hit hit=scene_pick(s,s->mouse_x,s->mouse_y,s->pointer_grab!=NULL);server_pointer_motion(s,hit,now);}
  }else if(e.type==SDL_MOUSEBUTTONDOWN||e.type==SDL_MOUSEBUTTONUP){
   bool down=e.type==SDL_MOUSEBUTTONDOWN;s->mouse_x=e.button.x;s->mouse_y=e.button.y;
   if(e.button.button==SDL_BUTTON_MIDDLE){s->middle_look=down;if(down)server_pointer_motion(s,(Hit){0},now);continue;}
   Hit hit=scene_pick(s,s->mouse_x,s->mouse_y,s->pointer_grab!=NULL);
   if(!s->arrange_mode&&!s->frame_dragging&&e.button.button==SDL_BUTTON_LEFT&&scene_wheel_button(s,hit,e.button.x,e.button.y,down,now))continue;
   uint32_t button=e.button.button==SDL_BUTTON_LEFT?BTN_LEFT:e.button.button==SDL_BUTTON_RIGHT?BTN_RIGHT:e.button.button==SDL_BUTTON_X1?BTN_SIDE:BTN_EXTRA;
   if(s->arrange_mode){app_arrange_pointer_button(s,hit,button,down);continue;}
   if(s->frame_dragging||hit.kind==HIT_FRAME||hit.kind==HIT_HANDLES_BUTTON){app_frame_pointer_button(s,hit,button,down);continue;}
   server_pointer_motion(s,hit,now);
   if(down&&e.button.button==SDL_BUTTON_LEFT){if(hit.kind==HIT_BUTTON){app_launch(s,hit.button,(SDL_GetModState()&KMOD_SHIFT)!=0);continue;}if(hit.kind==HIT_CLOSE){server_close_surface(hit.surface);continue;}if(hit.kind==HIT_COG){app_open_settings(s);continue;}}
   if(down&&e.button.button==SDL_BUTTON_RIGHT&&hit.kind==HIT_BUTTON){app_open_settings_slot(s,hit.button,true);continue;}
   if(hit.kind==HIT_APP||s->pointer_grab)server_pointer_button(s,button,down,now);
  }else if(e.type==SDL_MOUSEWHEEL){if(s->arrange_mode)app_arrange_pointer_wheel(s,e.wheel.preciseY);else{Hit hit=scene_pick(s,s->mouse_x,s->mouse_y,false);if(hit.kind==HIT_APP)server_pointer_axis(s,e.wheel.preciseX,e.wheel.preciseY,now);else s->target_fov=clampf(s->target_fov-e.wheel.preciseY*2,30,75);}}
 }
}
void backend_sdl_present(Server *s){SDL_GL_SwapWindow(s->window);}
void backend_sdl_shutdown(Server *s){if(s->gl)SDL_GL_DeleteContext(s->gl);if(s->window)SDL_DestroyWindow(s->window);}
