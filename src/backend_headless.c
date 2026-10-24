/* Default production backend: no X server anywhere.
 *  - GL context: surfaceless EGL (EGL_PLATFORM_SURFACELESS_MESA) rendering into
 *    our own FBO (color+depth-stencil renderbuffers), never a window.
 *    Why: containers, CI runners, and headless servers have no host X11/Wayland
 *    display or DRM master. EGL_PLATFORM_SURFACELESS_MESA + EGL_NO_SURFACE
 *    (via EGL_KHR_surfaceless_context) activates a full desktop OpenGL context
 *    (bound via eglBindAPI(EGL_OPENGL_API) before eglCreateContext) without any
 *    window surface. Because default framebuffer 0 is incomplete on a
 *    surfaceless context, we allocate an explicit FBO with a GL_RGBA8 color
 *    renderbuffer and a GL_DEPTH24_STENCIL8 depth-stencil renderbuffer.
 *  - Display output: an embedded libvncserver RFB server — glReadPixels() each
 *    frame straight into the VNC framebuffer (flipping OpenGL's bottom-up rows
 *    into RFB's top-down layout), no X11/compositor-of-compositors involved.
 *    Browsers reach it via a websockify+noVNC bridge (tools/viewer).
 *  - Input: RFB pointer/keyboard callbacks drive the exact same server_* input
 *    API the SDL backend uses, so behavior (picking, focus, hotkeys) matches.
 *  - Single-thread GL & teardown invariants: all GL work stays on the main
 *    compositor thread, and FBO/renderbuffers are deleted while the context is
 *    still current before eglMakeCurrent(..., EGL_NO_CONTEXT) / eglTerminate.
 *  This is also the only backend that supports zwp_linux_dmabuf_v1, because
 *  EGLImage only interoperates with an EGL-created GL context (see dmabuf.c).
 */
#include "chrome.h"
#include <rfb/rfb.h>
#include <rfb/keysym.h>
#include <arpa/inet.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

#define CD_VNC_LEFT 1
#define CD_VNC_MIDDLE 2
#define CD_VNC_RIGHT 4
#define CD_VNC_WHEEL_UP 8
#define CD_VNC_WHEEL_DOWN 16

static PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;

bool backend_headless_init(Server *s){
 /* Surfaceless EGL is selected explicitly instead of relying on whatever
  * window system EGL_DEFAULT_DISPLAY might discover. This is what lets the
  * default backend run with no X11/Wayland host and still use the same desktop
  * OpenGL renderer as the SDL development path. */
 get_platform_display=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
 if(!get_platform_display){fprintf(stderr,"[headless] EGL_EXT_platform_base / surfaceless platform not available\n");return false;}
 s->egl_display=get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,(void*)EGL_DEFAULT_DISPLAY,NULL);
 EGLint maj,min;if(s->egl_display==EGL_NO_DISPLAY||!eglInitialize(s->egl_display,&maj,&min)){fprintf(stderr,"[headless] eglInitialize failed: 0x%x\n",eglGetError());return false;}
 /* Bind the API before creating the context; otherwise the config's
  * EGL_OPENGL_BIT and the context's actual client API can disagree. */
 eglBindAPI(EGL_OPENGL_API);
 EGLint cfgattr[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
 EGLConfig cfg;EGLint n=0;
 if(!eglChooseConfig(s->egl_display,cfgattr,&cfg,1,&n)||n<1){fprintf(stderr,"[headless] eglChooseConfig failed\n");return false;}
 s->egl_context=eglCreateContext(s->egl_display,cfg,EGL_NO_CONTEXT,NULL);
 if(s->egl_context==EGL_NO_CONTEXT){fprintf(stderr,"[headless] eglCreateContext failed: 0x%x\n",eglGetError());return false;}
 /* EGL_KHR_surfaceless_context permits NO_DRAW/READ_SURFACE here. There is no
  * eglSwapBuffers in this backend: our explicitly allocated color+depth FBO
  * is the frame destination and glReadPixels is the presentation/readback
  * step into RFB. main initializes this before accepting Wayland clients, so
  * wl_shm glTexImage2D uploads and EGLImage imports also run with a current
  * context on this same thread. All GL objects are destroyed before EGL is
  * terminated during shutdown for the same reason. */
 if(!eglMakeCurrent(s->egl_display,EGL_NO_SURFACE,EGL_NO_SURFACE,s->egl_context)){fprintf(stderr,"[headless] eglMakeCurrent failed: 0x%x\n",eglGetError());return false;}
 fprintf(stderr,"[backend] headless/EGL (surfaceless, no X, no window) — %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));

 GLuint color_rb,depth_rb,fbo;
 glGenRenderbuffers(1,&color_rb);glBindRenderbuffer(GL_RENDERBUFFER,color_rb);glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,s->width,s->height);
 glGenRenderbuffers(1,&depth_rb);glBindRenderbuffer(GL_RENDERBUFFER,depth_rb);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,s->width,s->height);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,color_rb);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth_rb);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,depth_rb);
 GLenum fbs=glCheckFramebufferStatus(GL_FRAMEBUFFER);
 if(fbs!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"[headless] FBO incomplete: 0x%x\n",fbs);return false;}
 s->fbo_color_rb=color_rb;s->fbo_depth_rb=depth_rb;s->target_fbo=fbo;

 s->vnc_pixels=malloc((size_t)s->width*s->height*4); /* glReadPixels scratch, re-flipped into the VNC framebuffer each frame */
 if(!s->vnc_pixels){fprintf(stderr,"[headless] out of memory for readback buffer\n");return false;}

 int fake_argc=0;
 rfbScreenInfoPtr screen=rfbGetScreen(&fake_argc,NULL,s->width,s->height,8,3,4);
 if(!screen){fprintf(stderr,"[headless] rfbGetScreen failed\n");return false;}
 screen->desktopName="ChromeDrive cockpit";
 screen->frameBuffer=(char*)malloc((size_t)s->width*s->height*4);
 if(!screen->frameBuffer){fprintf(stderr,"[headless] out of memory for VNC framebuffer\n");return false;}
 memset(screen->frameBuffer,0,(size_t)s->width*s->height*4);
 screen->alwaysShared=TRUE;
 screen->port=s->vnc_port;
 screen->ipv6port=-1; /* IPv4 only keeps this simple for the sandbox preview proxy */
 screen->listenInterface=htonl(INADDR_ANY);
 screen->screenData=s;
 extern void cd_vnc_ptr(int,int,int,rfbClientPtr);
 extern void cd_vnc_key(rfbBool,rfbKeySym,rfbClientPtr);
 screen->ptrAddEvent=cd_vnc_ptr;
 screen->kbdAddEvent=cd_vnc_key;
 rfbInitServer(screen);
 s->vnc_screen=screen;
 fprintf(stderr,"[headless] VNC server listening on 0.0.0.0:%d — bridge with websockify+noVNC for a browser preview (see tools/viewer)\n",s->vnc_port);
 return true;
}

void cd_vnc_ptr(int mask,int x,int y,rfbClientPtr cl){
 Server *s=cl->screen->screenData;uint32_t now=SDL_GetTicks();
 bool middle=(mask&CD_VNC_MIDDLE)!=0,prev_middle=(s->vnc_prev_mask&CD_VNC_MIDDLE)!=0;
 bool left=(mask&CD_VNC_LEFT)!=0,prev_left=(s->vnc_prev_mask&CD_VNC_LEFT)!=0;
 bool right=(mask&CD_VNC_RIGHT)!=0,prev_right=(s->vnc_prev_mask&CD_VNC_RIGHT)!=0;
 if(x<0||y<0||x>=s->width||y>=s->height){
  /* VNC has no separate release event: a button-up outside the framebuffer
   * must still end grabs/drags instead of being silently consumed. Never
   * synthesize presses here, only releases/cancels. */
  if(prev_middle&&!middle)s->middle_look=false;
  if(prev_left&&!left)server_pointer_button(s,BTN_LEFT,false,now);
  if(prev_right&&!right)server_pointer_button(s,BTN_RIGHT,false,now);
  if(s->arrange_dragging&&((s->arrange_drag_button==BTN_LEFT&&!left)||(s->arrange_drag_button==BTN_RIGHT&&!right)))
   app_arrange_pointer_button(s,(Hit){0},(uint32_t)s->arrange_drag_button,false);
  if(s->frame_dragging&&!left)app_frame_pointer_button(s,(Hit){0},BTN_LEFT,false);
  if(!left&&!right&&!middle){
   s->middle_look=false;s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;
   s->frame_dragging=false;s->frame_drag_slot=-1;server_release_pointer_buttons(s,now);
  }
  s->vnc_prev_mask=mask;return;
 }
 s->mouse_x=x;s->mouse_y=y;
 if(s->wheel_drag){
  if(left)scene_wheel_motion(s,x,y,now);else scene_wheel_button(s,(Hit){0},x,y,false,now);
  s->vnc_prev_mask=mask;s->vnc_prev_x=x;s->vnc_prev_y=y;return;
 }
 if(middle){
  if(!prev_middle){s->middle_look=true;server_pointer_motion(s,(Hit){0},now);}
  else{s->target_yaw-=(x-s->vnc_prev_x)*.5f;s->target_pitch=clampf(s->target_pitch-(y-s->vnc_prev_y)*.4f,-35,32);s->focus_view=false;s->arrange_last_x=x;s->arrange_last_y=y;s->frame_last_x=x;s->frame_last_y=y;}
 }else{
  if(prev_middle)s->middle_look=false;
  Hit hit=scene_pick(s,x,y,s->pointer_grab!=NULL);
  bool wu=(mask&CD_VNC_WHEEL_UP)&&!(s->vnc_prev_mask&CD_VNC_WHEEL_UP);
  bool wd=(mask&CD_VNC_WHEEL_DOWN)&&!(s->vnc_prev_mask&CD_VNC_WHEEL_DOWN);
  if(s->arrange_mode){
   if(left!=prev_left)app_arrange_pointer_button(s,hit,BTN_LEFT,left);
   if(right!=prev_right)app_arrange_pointer_button(s,hit,BTN_RIGHT,right);
   if(left||right)app_arrange_pointer_motion(s,x,y);
   if(wu||wd)app_arrange_pointer_wheel(s,wu?1.f:-1.f);
  }else if(s->frame_dragging||((hit.kind==HIT_FRAME||hit.kind==HIT_HANDLES_BUTTON)&&left!=prev_left)){
   if(left!=prev_left)app_frame_pointer_button(s,hit,BTN_LEFT,left);
   if(left&&s->frame_dragging)app_frame_pointer_motion(s,x,y);
  }else{
   server_pointer_motion(s,hit,now);
   if(left&&!prev_left&&hit.kind==HIT_WHEEL)scene_wheel_button(s,hit,x,y,true,now);
   else if(left&&!prev_left&&hit.kind==HIT_BUTTON)app_launch(s,hit.button,false);
   else if(left&&!prev_left&&hit.kind==HIT_COG)app_open_settings(s);
   else if(left&&!prev_left&&hit.kind==HIT_CLOSE)server_close_surface(hit.surface);
   else if(left!=prev_left&&(hit.kind==HIT_APP||s->pointer_grab))server_pointer_button(s,BTN_LEFT,left,now);
   if(right&&!prev_right&&hit.kind==HIT_BUTTON)app_open_settings_slot(s,hit.button,true);
   else if(right!=prev_right&&(hit.kind==HIT_APP||s->pointer_grab))server_pointer_button(s,BTN_RIGHT,right,now);
   if(wu||wd){if(hit.kind==HIT_APP)server_pointer_axis(s,0,wu?-3:3,now);else s->target_fov=clampf(s->target_fov-(wu?3:-3),30,75);}
  }
 }
 s->vnc_prev_mask=mask;s->vnc_prev_x=x;s->vnc_prev_y=y;
}
void cd_vnc_key(rfbBool down,rfbKeySym key,rfbClientPtr cl){
 Server *s=cl->screen->screenData;uint32_t evdev=keysym_to_evdev((uint32_t)key);if(!evdev)return;
 uint32_t now=SDL_GetTicks();
 if(!app_global_key(s,evdev,down))server_key(s,evdev,down,now);
}
void backend_headless_poll(Server *s){rfbProcessEvents((rfbScreenInfoPtr)s->vnc_screen,0);}
void backend_headless_present(Server *s){
 rfbScreenInfoPtr screen=(rfbScreenInfoPtr)s->vnc_screen;
 glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);
 glReadPixels(0,0,s->width,s->height,GL_RGBA,GL_UNSIGNED_BYTE,s->vnc_pixels);
 int stride=s->width*4;
 for(int row=0;row<s->height;row++)
  memcpy(screen->frameBuffer+(size_t)row*stride,s->vnc_pixels+(size_t)(s->height-1-row)*stride,stride); /* GL is bottom-up, VNC/raster is top-down */
 rfbMarkRectAsModified(screen,0,0,s->width,s->height);
}
void backend_headless_shutdown(Server *s){
 if(s->vnc_screen){rfbScreenInfoPtr screen=(rfbScreenInfoPtr)s->vnc_screen;rfbShutdownServer(screen,TRUE);free(screen->frameBuffer);rfbScreenCleanup(screen);s->vnc_screen=NULL;}
 free(s->vnc_pixels);s->vnc_pixels=NULL;
 if(s->fbo_color_rb)glDeleteRenderbuffers(1,&s->fbo_color_rb);if(s->fbo_depth_rb)glDeleteRenderbuffers(1,&s->fbo_depth_rb);if(s->target_fbo)glDeleteFramebuffers(1,&s->target_fbo);
 if(s->egl_display!=EGL_NO_DISPLAY){eglMakeCurrent(s->egl_display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(s->egl_context!=EGL_NO_CONTEXT)eglDestroyContext(s->egl_display,s->egl_context);eglTerminate(s->egl_display);}
}
