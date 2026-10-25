/* Shared surfaceless-EGL renderer for the headless and fbdev backends.
 *
 * Both backends draw through the same desktop-GL context and explicit
 * GL_RGBA8 + GL_DEPTH24_STENCIL8 FBO. They differ only in presentation:
 * headless hands the single GL_RGBA readback to RFB, fbdev converts it into
 * /dev/fb* memory. Keeping the EGL setup here preserves the invariants the
 * rest of the compositor depends on: a current EGL context before Wayland
 * buffer uploads/EGLImage imports, s->target_fbo as the scene destination,
 * one shared readback scratch, and GL objects destroyed before EGL is
 * terminated. */
#include "chrome.h"

#include <stdlib.h>

static PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;

static void egl_fail(Server *s,const char *tag,const char *what){
 fprintf(stderr,"[%s] %s\n",tag,what);
 backend_egl_shutdown(s);
}

bool backend_egl_init(Server *s,const char *tag){
 if(!tag||!*tag)tag="EGL";
 s->egl_display=EGL_NO_DISPLAY;s->egl_context=EGL_NO_CONTEXT;
 s->target_fbo=s->fbo_color_rb=s->fbo_depth_rb=0;
 s->readback_pixels=NULL;
 if(s->width<=0||s->height<=0){fprintf(stderr,"[%s] invalid render size %dx%d\n",tag,s->width,s->height);return false;}
 /* Select surfaceless EGL explicitly instead of relying on whatever window
  * system EGL_DEFAULT_DISPLAY might discover. */
 get_platform_display=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
 if(!get_platform_display){fprintf(stderr,"[%s] EGL_EXT_platform_base / surfaceless platform not available\n",tag);return false;}
 EGLDisplay display=get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,(void*)EGL_DEFAULT_DISPLAY,NULL);
 EGLint maj,min;
 if(display==EGL_NO_DISPLAY||!eglInitialize(display,&maj,&min)){fprintf(stderr,"[%s] eglInitialize failed: 0x%x\n",tag,eglGetError());return false;}
 s->egl_display=display;
 if(!eglBindAPI(EGL_OPENGL_API)){egl_fail(s,tag,"eglBindAPI(EGL_OPENGL_API) failed");return false;}
 EGLint cfgattr[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
 EGLConfig cfg;EGLint n=0;
 if(!eglChooseConfig(s->egl_display,cfgattr,&cfg,1,&n)||n<1){egl_fail(s,tag,"eglChooseConfig failed");return false;}
 s->egl_context=eglCreateContext(s->egl_display,cfg,EGL_NO_CONTEXT,NULL);
 if(s->egl_context==EGL_NO_CONTEXT){fprintf(stderr,"[%s] eglCreateContext failed: 0x%x\n",tag,eglGetError());backend_egl_shutdown(s);return false;}
 /* EGL_KHR_surfaceless_context permits NO_DRAW/READ_SURFACE here. There is
  * no eglSwapBuffers in either EGL backend: the explicit FBO is the frame
  * destination and glReadPixels is the presentation/readback step. */
 if(!eglMakeCurrent(s->egl_display,EGL_NO_SURFACE,EGL_NO_SURFACE,s->egl_context)){fprintf(stderr,"[%s] eglMakeCurrent failed: 0x%x\n",tag,eglGetError());backend_egl_shutdown(s);return false;}
 fprintf(stderr,"[backend] %s/EGL (surfaceless, no X, no window) — %s / %s\n",tag,glGetString(GL_RENDERER),glGetString(GL_VERSION));

 GLuint color_rb,depth_rb,fbo;
 glGenRenderbuffers(1,&color_rb);glBindRenderbuffer(GL_RENDERBUFFER,color_rb);glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,s->width,s->height);
 glGenRenderbuffers(1,&depth_rb);glBindRenderbuffer(GL_RENDERBUFFER,depth_rb);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,s->width,s->height);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,color_rb);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth_rb);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,depth_rb);
 s->fbo_color_rb=color_rb;s->fbo_depth_rb=depth_rb;s->target_fbo=fbo;
 GLenum fbs=glCheckFramebufferStatus(GL_FRAMEBUFFER);
 if(fbs!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"[%s] FBO incomplete: 0x%x\n",tag,fbs);backend_egl_shutdown(s);return false;}

 s->readback_pixels=malloc((size_t)s->width*(size_t)s->height*4);
 if(!s->readback_pixels){fprintf(stderr,"[%s] out of memory for readback buffer\n",tag);backend_egl_shutdown(s);return false;}
 return true;
}

void backend_egl_shutdown(Server *s){
 if(!s)return;
 free(s->readback_pixels);s->readback_pixels=NULL;
 bool current=s->egl_display!=EGL_NO_DISPLAY&&s->egl_context!=EGL_NO_CONTEXT&&eglGetCurrentContext()==s->egl_context;
 if(current){
  if(s->fbo_color_rb)glDeleteRenderbuffers(1,&s->fbo_color_rb);
  if(s->fbo_depth_rb)glDeleteRenderbuffers(1,&s->fbo_depth_rb);
  if(s->target_fbo)glDeleteFramebuffers(1,&s->target_fbo);
 }
 s->fbo_color_rb=s->fbo_depth_rb=s->target_fbo=0;
 if(s->egl_display!=EGL_NO_DISPLAY){
  eglMakeCurrent(s->egl_display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
  if(s->egl_context!=EGL_NO_CONTEXT)eglDestroyContext(s->egl_display,s->egl_context);
  eglTerminate(s->egl_display);
 }
 s->egl_context=EGL_NO_CONTEXT;s->egl_display=EGL_NO_DISPLAY;
}
