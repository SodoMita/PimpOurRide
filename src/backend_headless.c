/* Default production backend: no X server anywhere.
 *  - GL context: surfaceless EGL (EGL_PLATFORM_SURFACELESS_MESA) rendering into
 *    our own FBO (color+depth-stencil renderbuffers), never a window.
 *  - Display output: an embedded libvncserver RFB server — glReadPixels() each
 *    frame straight into the VNC framebuffer, no X11/compositor-of-compositors
 *    involved. Browsers reach it via a websockify+noVNC bridge (tools/viewer).
 *  - Input: RFB pointer/keyboard callbacks drive the exact same server_* input
 *    API the SDL backend uses, so behavior (picking, focus, hotkeys) matches.
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
 get_platform_display=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
 if(!get_platform_display){fprintf(stderr,"[headless] EGL_EXT_platform_base / surfaceless platform not available\n");return false;}
 s->egl_display=get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,(void*)EGL_DEFAULT_DISPLAY,NULL);
 EGLint maj,min;if(s->egl_display==EGL_NO_DISPLAY||!eglInitialize(s->egl_display,&maj,&min)){fprintf(stderr,"[headless] eglInitialize failed: 0x%x\n",eglGetError());return false;}
 eglBindAPI(EGL_OPENGL_API);
 EGLint cfgattr[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
 EGLConfig cfg;EGLint n=0;
 if(!eglChooseConfig(s->egl_display,cfgattr,&cfg,1,&n)||n<1){fprintf(stderr,"[headless] eglChooseConfig failed\n");return false;}
 s->egl_context=eglCreateContext(s->egl_display,cfg,EGL_NO_CONTEXT,NULL);
 if(s->egl_context==EGL_NO_CONTEXT){fprintf(stderr,"[headless] eglCreateContext failed: 0x%x\n",eglGetError());return false;}
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
 if(x<0||y<0||x>=s->width||y>=s->height){s->vnc_prev_mask=mask;return;}
 bool middle=(mask&CD_VNC_MIDDLE)!=0,prev_middle=(s->vnc_prev_mask&CD_VNC_MIDDLE)!=0;
 s->mouse_x=x;s->mouse_y=y;
 if(middle){
  if(!prev_middle){s->middle_look=true;server_pointer_motion(s,(Hit){0},now);}
  else{s->target_yaw-=(x-s->vnc_prev_x)*.5f;s->target_pitch=clampf(s->target_pitch-(y-s->vnc_prev_y)*.4f,-35,32);s->focus_view=false;s->arrange_last_x=x;s->arrange_last_y=y;}
 }else{
  if(prev_middle)s->middle_look=false;
  Hit hit=scene_pick(s,x,y,s->pointer_grab!=NULL);
  bool left=(mask&CD_VNC_LEFT)!=0,prev_left=(s->vnc_prev_mask&CD_VNC_LEFT)!=0;
  bool right=(mask&CD_VNC_RIGHT)!=0,prev_right=(s->vnc_prev_mask&CD_VNC_RIGHT)!=0;
  bool wu=(mask&CD_VNC_WHEEL_UP)&&!(s->vnc_prev_mask&CD_VNC_WHEEL_UP);
  bool wd=(mask&CD_VNC_WHEEL_DOWN)&&!(s->vnc_prev_mask&CD_VNC_WHEEL_DOWN);
  if(s->arrange_mode){
   if(left!=prev_left)app_arrange_pointer_button(s,hit,BTN_LEFT,left);
   if(right!=prev_right)app_arrange_pointer_button(s,hit,BTN_RIGHT,right);
   if(left||right)app_arrange_pointer_motion(s,x,y);
   if(wu||wd)app_arrange_pointer_wheel(s,wu?1.f:-1.f);
  }else{
   server_pointer_motion(s,hit,now);
   if(left&&!prev_left&&hit.kind==HIT_BUTTON)app_launch(s,hit.button,false);
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
/* X11 keysym -> Linux evdev keycode. Covers typing, navigation, function keys
 * and modifiers — enough for terminals/file managers/clients typically used
 * here. libvncserver hands us rfbKeySym values (== X11 keysymdef values). */
uint32_t keysym_to_evdev(uint32_t key){
 /* Evdev codes follow the physical keyboard matrix, not alphabet order:
  * KEY_A+('g'-'a') is KEY_J, for example. Spell the letters out. */
 if((key>='a'&&key<='z')||(key>='A'&&key<='Z')){static const uint32_t letters[26]={KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};uint32_t lower=key>='A'&&key<='Z'?key+('a'-'A'):key;return letters[lower-'a'];}
 if(key>='0'&&key<='9'){static const uint32_t row[10]={KEY_0,KEY_1,KEY_2,KEY_3,KEY_4,KEY_5,KEY_6,KEY_7,KEY_8,KEY_9};return row[key-'0'];}
 if(key>=XK_KP_0&&key<=XK_KP_9){static const uint32_t kp[10]={KEY_KP0,KEY_KP1,KEY_KP2,KEY_KP3,KEY_KP4,KEY_KP5,KEY_KP6,KEY_KP7,KEY_KP8,KEY_KP9};return kp[key-XK_KP_0];}
 switch(key){
 case XK_space:case XK_KP_Space:return KEY_SPACE;case XK_Return:return KEY_ENTER;case XK_KP_Enter:return KEY_KPENTER;case XK_BackSpace:return KEY_BACKSPACE;case XK_Tab:return KEY_TAB;case XK_Escape:return KEY_ESC;case XK_Delete:return KEY_DELETE;
 case XK_minus:return KEY_MINUS;case XK_equal:return KEY_EQUAL;case XK_bracketleft:return KEY_LEFTBRACE;case XK_bracketright:return KEY_RIGHTBRACE;
 case XK_semicolon:return KEY_SEMICOLON;case XK_apostrophe:return KEY_APOSTROPHE;case XK_grave:return KEY_GRAVE;case XK_backslash:return KEY_BACKSLASH;
 case XK_comma:return KEY_COMMA;case XK_period:return KEY_DOT;case XK_slash:return KEY_SLASH;
 case XK_Up:return KEY_UP;case XK_Down:return KEY_DOWN;case XK_Left:return KEY_LEFT;case XK_Right:return KEY_RIGHT;
 case XK_Home:return KEY_HOME;case XK_End:return KEY_END;case XK_Page_Up:return KEY_PAGEUP;case XK_Page_Down:return KEY_PAGEDOWN;case XK_Insert:return KEY_INSERT;
 case XK_Shift_L:return KEY_LEFTSHIFT;case XK_Shift_R:return KEY_RIGHTSHIFT;case XK_Control_L:return KEY_LEFTCTRL;case XK_Control_R:return KEY_RIGHTCTRL;
 case XK_Alt_L:return KEY_LEFTALT;case XK_Alt_R:return KEY_RIGHTALT;case XK_Super_L:return KEY_LEFTMETA;case XK_Super_R:return KEY_RIGHTMETA;
 case XK_Caps_Lock:return KEY_CAPSLOCK;case XK_Num_Lock:return KEY_NUMLOCK;case XK_Menu:return KEY_MENU;
 case XK_F1:return KEY_F1;case XK_F2:return KEY_F2;case XK_F3:return KEY_F3;case XK_F4:return KEY_F4;case XK_F5:return KEY_F5;case XK_F6:return KEY_F6;
 case XK_F7:return KEY_F7;case XK_F8:return KEY_F8;case XK_F9:return KEY_F9;case XK_F10:return KEY_F10;case XK_F11:return KEY_F11;case XK_F12:return KEY_F12;
 default:return 0;
 }
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
