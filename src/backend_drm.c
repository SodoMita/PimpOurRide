/* Direct DRM/KMS presentation through a GBM surface and desktop EGL.
 *
 * scene_render() still targets s->target_fbo (zero here, the EGL default
 * framebuffer), so Wayland uploads, RTT/post-FX, HUD and screenshots share the
 * windowed backend's GL contract. This file owns only the display lifecycle:
 * connected output + CRTC selection, GBM/EGL setup, KMS framebuffer handles,
 * event-driven page flips, and restoration of the previous CRTC at exit.
 * There is no glReadPixels/CPU scanout blit on this path. */
#include "chrome.h"

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct {
 struct gbm_bo *bo;
 uint32_t fb_id;
} DrmFramebuffer;

typedef struct {
 int fd;
 bool master,mode_set,page_flip_pending,warned_event;
 char path[256],connector_name[64];
 uint32_t connector_id,crtc_id;
 drmModeModeInfo mode;
 drmModeCrtcPtr saved_crtc;
 uint32_t *saved_connectors;int saved_connector_count;
 struct gbm_device *gbm;
 struct gbm_surface *surface;
 EGLSurface egl_surface;
 DrmFramebuffer framebuffers[2]; /* current scanout + optional pending flip */
 int framebuffer_count;
 struct gbm_bo *current_bo,*pending_bo;
} DrmState;

static DrmState *drm_state(Server *s){return s?(DrmState*)s->drm_state:NULL;}

static void drm_release_bo(DrmState *st,struct gbm_bo *bo){
 if(!st||!bo)return;
 for(int i=0;i<st->framebuffer_count;i++){
  if(st->framebuffers[i].bo!=bo)continue;
  if(drmModeRmFB(st->fd,st->framebuffers[i].fb_id)<0)
   fprintf(stderr,"[drm] drmModeRmFB %u failed: %s\n",st->framebuffers[i].fb_id,strerror(errno));
  st->framebuffers[i]=st->framebuffers[--st->framebuffer_count];
  st->framebuffers[st->framebuffer_count]=(DrmFramebuffer){0};
  break;
 }
 if(st->surface)gbm_surface_release_buffer(st->surface,bo);
}

static uint32_t drm_bo_framebuffer(DrmState *st,struct gbm_bo *bo){
 for(int i=0;i<st->framebuffer_count;i++)if(st->framebuffers[i].bo==bo)return st->framebuffers[i].fb_id;
 if(st->framebuffer_count>=2){fprintf(stderr,"[drm] more than two locked GBM buffers\n");return 0;}
 uint32_t handle=gbm_bo_get_handle(bo).u32,stride=gbm_bo_get_stride(bo),fb_id=0;
 uint32_t width=gbm_bo_get_width(bo),height=gbm_bo_get_height(bo),offset=gbm_bo_get_offset(bo,0);
 uint64_t modifier=gbm_bo_get_modifier(bo),modifier_cap=0;
 bool has_modifier=modifier!=DRM_FORMAT_MOD_INVALID;
 drmGetCap(st->fd,DRM_CAP_ADDFB2_MODIFIERS,&modifier_cap); /* zero is a valid legacy-only result */
 uint32_t handles[4]={handle},pitches[4]={stride},offsets[4]={offset};
 int result=-1;
 /* Legacy drmModeAddFB cannot describe a tiled/compressed BO or a nonzero
  * plane offset. Use the real GBM layout whenever the driver exposes modifier
  * support; silently scanning a non-linear BO out as linear is not a fallback. */
 if(has_modifier&&modifier_cap){
  uint64_t modifiers[4]={modifier};
  result=drmModeAddFB2WithModifiers(st->fd,width,height,GBM_FORMAT_XRGB8888,
   handles,pitches,offsets,modifiers,&fb_id,DRM_MODE_FB_MODIFIERS);
  if(result<0&&modifier!=DRM_FORMAT_MOD_LINEAR){
   fprintf(stderr,"[drm] drmModeAddFB2WithModifiers %ux%u modifier=%llx failed: %s\n",
    width,height,(unsigned long long)modifier,strerror(errno));return 0;
  }
 }
 if(result<0&&(!has_modifier||modifier==DRM_FORMAT_MOD_LINEAR)){
  /* Linear scanout remains safe without modifier flags, including when a
   * driver advertises the cap but rejects the modifier-form request. */
  result=drmModeAddFB2(st->fd,width,height,GBM_FORMAT_XRGB8888,handles,pitches,offsets,&fb_id,0);
  /* Very old KMS drivers may expose AddFB but not AddFB2. This fallback is
   * valid only for a linear, zero-offset, single-plane XRGB8888 buffer. */
  if(result<0&&offset==0)result=drmModeAddFB(st->fd,width,height,24,32,stride,handle,&fb_id);
  if(result<0)fprintf(stderr,"[drm] framebuffer registration %ux%u stride=%u offset=%u failed: %s\n",
   width,height,stride,offset,strerror(errno));
 }else if(result<0)fprintf(stderr,"[drm] KMS cannot register non-linear GBM modifier %llx without ADDFB2 modifier support\n",
  (unsigned long long)modifier);
 if(result<0)return 0;
 st->framebuffers[st->framebuffer_count++]=(DrmFramebuffer){.bo=bo,.fb_id=fb_id};
 return fb_id;
}

static void drm_page_flipped(int fd,unsigned sequence,unsigned seconds,unsigned usec,void *user_data){
 DrmState *st=user_data;(void)fd;(void)sequence;(void)seconds;(void)usec;
 if(!st||!st->page_flip_pending)return;
 struct gbm_bo *old=st->current_bo;
 st->current_bo=st->pending_bo;st->pending_bo=NULL;st->page_flip_pending=false;
 /* The vblank event proves old is no longer scanned out. Only now may its
  * kernel FB be removed and its GBM buffer return to the EGL surface. */
 if(old)drm_release_bo(st,old);
}

static int64_t drm_now_ms(void){
 struct timespec now;
 clock_gettime(CLOCK_MONOTONIC,&now);
 return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

/* 1 = consumed a DRM event, 0 = timeout/no event, -1 = polling/driver error,
 * -2 = VT signal activity. The VT fd is polled alongside DRM so a release
 * request cannot be stuck behind a slow page-flip wait. */
static int drm_wait_event(Server *s,int timeout_ms){
 DrmState *st=drm_state(s);
 if(!st||st->fd<0)return -1;
 struct pollfd fds[2]={{.fd=st->fd,.events=POLLIN}};
 int count=1;
 int vt_fd=backend_vt_fd(s);
 if(vt_fd>=0){fds[1].fd=vt_fd;fds[1].events=POLLIN;count=2;}
 int ready;
 do ready=poll(fds,(nfds_t)count,timeout_ms);while(ready<0&&errno==EINTR);
 if(ready==0)return 0;
 if(ready<0||(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL)))return -1;
 if(count>1&&fds[1].revents)return -2;
 if(!(fds[0].revents&POLLIN))return 0;
 drmEventContext context={.version=DRM_EVENT_CONTEXT_VERSION,.page_flip_handler=drm_page_flipped};
 return drmHandleEvent(st->fd,&context)==0?1:-1;
}

static bool drm_wait_for_flip(Server *s,int timeout_ms){
 DrmState *st=drm_state(s);
 if(!st)return true;
 int64_t deadline=drm_now_ms()+timeout_ms;
 while(st->page_flip_pending){
  int64_t remaining=deadline-drm_now_ms();
  int result=drm_wait_event(s,remaining>0?(int)remaining:0);
  if(result>0)continue;
  if(result==-2){fprintf(stderr,"[drm] page-flip wait interrupted by VT activity\n");return false;}
  if(result==0)fprintf(stderr,"[drm] timed out waiting for page-flip completion\n");
  else fprintf(stderr,"[drm] page-flip event wait failed: %s\n",strerror(errno));
  return false;
 }
 return true;
}

static uint32_t drm_find_crtc(int fd,drmModeResPtr resources,drmModeConnectorPtr connector){
 /* Retaining the current encoder/CRTC avoids unnecessary output handoff. */
 if(connector->encoder_id){
  drmModeEncoderPtr encoder=drmModeGetEncoder(fd,connector->encoder_id);
  if(encoder){
   uint32_t current=encoder->crtc_id;drmModeFreeEncoder(encoder);
   if(current)return current;
  }
 }
 for(int e=0;e<connector->count_encoders;e++){
  drmModeEncoderPtr encoder=drmModeGetEncoder(fd,connector->encoders[e]);
  if(!encoder)continue;
  for(int c=0;c<resources->count_crtcs;c++)if(encoder->possible_crtcs&(1u<<c)){
   uint32_t id=resources->crtcs[c];drmModeFreeEncoder(encoder);return id;
  }
  drmModeFreeEncoder(encoder);
 }
 return 0;
}

static bool drm_choose_mode(DrmState *st,drmModeConnectorPtr connector,drmModeCrtcPtr current,int request_w,int request_h){
 if(request_w>0&&request_h>0){
  for(int i=0;i<connector->count_modes;i++)if(connector->modes[i].hdisplay==request_w&&connector->modes[i].vdisplay==request_h){
   st->mode=connector->modes[i];return true;
  }
  return false;
 }
 if(current&&current->mode_valid&&current->mode.hdisplay&&current->mode.vdisplay){st->mode=current->mode;return true;}
 for(int i=0;i<connector->count_modes;i++)if(connector->modes[i].type&DRM_MODE_TYPE_PREFERRED){st->mode=connector->modes[i];return true;}
 if(connector->count_modes>0){st->mode=connector->modes[0];return true;}
 return false;
}

static void drm_save_crtc_connectors(DrmState *st,drmModeResPtr resources){
 /* A CRTC can feed several cloned connectors. Driving only the selected one
  * must not silently forget the others when the previous CRTC is restored. */
 if(!st->saved_crtc||!st->saved_crtc->mode_valid||resources->count_connectors<=0)return;
 st->saved_connectors=calloc((size_t)resources->count_connectors,sizeof(*st->saved_connectors));
 if(!st->saved_connectors){fprintf(stderr,"[drm] cannot save cloned connector set; restoration will use the selected connector\n");return;}
 for(int i=0;i<resources->count_connectors;i++){
  drmModeConnectorPtr connector=drmModeGetConnector(st->fd,resources->connectors[i]);
  if(!connector)continue;
  drmModeEncoderPtr encoder=connector->encoder_id?drmModeGetEncoder(st->fd,connector->encoder_id):NULL;
  if(connector->connection==DRM_MODE_CONNECTED&&encoder&&encoder->crtc_id==st->crtc_id)
   st->saved_connectors[st->saved_connector_count++]=connector->connector_id;
  if(encoder)drmModeFreeEncoder(encoder);
  drmModeFreeConnector(connector);
 }
}

static bool drm_select_output(Server *s,DrmState *st){
 drmModeResPtr resources=drmModeGetResources(st->fd);
 if(!resources){fprintf(stderr,"[drm] drmModeGetResources failed on %s: %s\n",st->path,strerror(errno));return false;}
 for(int i=0;i<resources->count_connectors;i++){
  drmModeConnectorPtr connector=drmModeGetConnector(st->fd,resources->connectors[i]);
  if(!connector)continue;
  if(connector->connection!=DRM_MODE_CONNECTED||connector->count_modes==0){drmModeFreeConnector(connector);continue;}
  uint32_t crtc_id=drm_find_crtc(st->fd,resources,connector);
  if(!crtc_id){drmModeFreeConnector(connector);continue;}
  drmModeCrtcPtr current=drmModeGetCrtc(st->fd,crtc_id);
  if(!drm_choose_mode(st,connector,current,s->drm_mode_width,s->drm_mode_height)){
   if(current)drmModeFreeCrtc(current);drmModeFreeConnector(connector);continue;
  }
  st->connector_id=connector->connector_id;st->crtc_id=crtc_id;st->saved_crtc=current;
  drm_save_crtc_connectors(st,resources);
  const char *type=drmModeGetConnectorTypeName(connector->connector_type);
  snprintf(st->connector_name,sizeof(st->connector_name),"%s-%u",type?type:"connector",connector->connector_type_id);
  drmModeFreeConnector(connector);drmModeFreeResources(resources);return true;
 }
 if(s->drm_mode_width>0&&s->drm_mode_height>0)
  fprintf(stderr,"[drm] requested mode %dx%d is unavailable on every connected output of %s\n",s->drm_mode_width,s->drm_mode_height,st->path);
 else fprintf(stderr,"[drm] no connected connector with a usable CRTC/mode on %s\n",st->path);
 drmModeFreeResources(resources);return false;
}

static bool drm_egl_init(Server *s,DrmState *st){
 st->gbm=gbm_create_device(st->fd);
 if(!st->gbm){fprintf(stderr,"[drm] gbm_create_device failed\n");return false;}
 PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
 if(!get_platform){fprintf(stderr,"[drm] EGL_EXT_platform_base is unavailable\n");return false;}
 s->egl_display=get_platform(EGL_PLATFORM_GBM_KHR,st->gbm,NULL);
 EGLint major=0,minor=0;
 if(s->egl_display==EGL_NO_DISPLAY||!eglInitialize(s->egl_display,&major,&minor)){
  fprintf(stderr,"[drm] eglInitialize(GBM) failed: 0x%x\n",eglGetError());return false;
 }
 if(!eglBindAPI(EGL_OPENGL_API)){fprintf(stderr,"[drm] eglBindAPI(EGL_OPENGL_API) failed\n");return false;}
 EGLint attributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,
  /* render_to_texture normally supplies its own depth/stencil FBO, but the
   * direct path and allocation-failure fallback draw 3D into this default
   * framebuffer. Match the SDL backend instead of accepting a depthless config. */
  EGL_DEPTH_SIZE,24,EGL_STENCIL_SIZE,8,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
 EGLConfig configs[64];EGLint count=0;
 if(!eglChooseConfig(s->egl_display,attributes,configs,64,&count)||count<1){
  fprintf(stderr,"[drm] no desktop-GL GBM EGL configs\n");return false;
 }
 EGLConfig config=NULL;
 for(EGLint i=0;i<count;i++){
  EGLint visual=0;eglGetConfigAttrib(s->egl_display,configs[i],EGL_NATIVE_VISUAL_ID,&visual);
  if((uint32_t)visual==GBM_FORMAT_XRGB8888){config=configs[i];break;}
 }
 if(!config){fprintf(stderr,"[drm] no EGL config matching GBM_FORMAT_XRGB8888\n");return false;}
 s->egl_context=eglCreateContext(s->egl_display,config,EGL_NO_CONTEXT,NULL);
 if(s->egl_context==EGL_NO_CONTEXT){fprintf(stderr,"[drm] eglCreateContext failed: 0x%x\n",eglGetError());return false;}
 st->surface=gbm_surface_create(st->gbm,st->mode.hdisplay,st->mode.vdisplay,GBM_FORMAT_XRGB8888,GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
 if(!st->surface){fprintf(stderr,"[drm] gbm_surface_create failed\n");return false;}
 st->egl_surface=eglCreateWindowSurface(s->egl_display,config,(EGLNativeWindowType)st->surface,NULL);
 if(st->egl_surface==EGL_NO_SURFACE){fprintf(stderr,"[drm] eglCreateWindowSurface(GBM) failed: 0x%x\n",eglGetError());return false;}
 if(!eglMakeCurrent(s->egl_display,st->egl_surface,st->egl_surface,s->egl_context)){
  fprintf(stderr,"[drm] eglMakeCurrent(GBM) failed: 0x%x\n",eglGetError());return false;
 }
 eglSwapInterval(s->egl_display,0); /* KMS page-flip events, not a second EGL throttle, pace scanout. */
 s->target_fbo=s->fbo_color_rb=s->fbo_depth_rb=0;s->readback_pixels=NULL;
 s->width=(int)st->mode.hdisplay;s->height=(int)st->mode.vdisplay;s->mouse_x=s->width/2;s->mouse_y=s->height/2;
 fprintf(stderr,"[backend] DRM/KMS + GBM/EGL (direct scanout, no readback) — %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
 return true;
}

bool backend_drm_init(Server *s){
 DrmState *st=calloc(1,sizeof(*st));if(!st){fprintf(stderr,"[drm] out of memory\n");return false;}
 st->fd=-1;st->egl_surface=EGL_NO_SURFACE;s->drm_state=st;
 /* Install VT_PROCESS before taking DRM master or starting evdev. Any later
  * init failure goes through the same mode/signalfd restoration path. */
 backend_vt_init(s);
 s->egl_display=EGL_NO_DISPLAY;s->egl_context=EGL_NO_CONTEXT;s->target_fbo=0;s->readback_pixels=NULL;
 snprintf(st->path,sizeof(st->path),"%s",s->drm_path[0]?s->drm_path:"/dev/dri/card0");
 st->fd=open(st->path,O_RDWR|O_CLOEXEC);
 if(st->fd<0){fprintf(stderr,"[drm] cannot open %s: %s (use a KMS card node and video-group/udev access)\n",st->path,strerror(errno));goto fail;}
 if(drmSetMaster(st->fd)<0){fprintf(stderr,"[drm] drmSetMaster failed on %s: %s (stop the active KMS compositor or grant logind seat access)\n",st->path,strerror(errno));goto fail;}
 st->master=true;
 if(!drm_select_output(s,st))goto fail;
 if(!drm_egl_init(s,st))goto fail;
 fprintf(stderr,"[drm] %s: connector %s (%u), CRTC %u, %ux%u@%u\n",st->path,
  st->connector_name,st->connector_id,st->crtc_id,st->mode.hdisplay,st->mode.vdisplay,st->mode.vrefresh);
 /* Managed VT switching needs /dev/tty. Without it, grab would make the
  * kernel Ctrl+Alt+F* chord unreachable, so downgrade rather than trap input. */
 if(s->drm_grab_input&&!backend_vt_managed(s)){
  s->drm_grab_input=false;
  fprintf(stderr,"[drm] no managed VT available; EVIOCGRAB downgraded so kernel VT switching stays reachable (TTY input isolation is unavailable)\n");
 }
 if(s->fbdev_evdev&&!backend_evdev_init(s))goto fail;
 if(s->fbdev_evdev&&(!s->evdev_state||((EvdevState*)s->evdev_state)->device_count==0))
  fprintf(stderr,"[drm] no local evdev devices opened; display continues without local input\n");
 return true;
fail:
 backend_drm_shutdown(s);return false;
}

static int drm_poll_events(Server *s,DrmState *st){
 int result=0;
 while((result=drm_wait_event(s,0))>0){}
 if(result==-1&&!st->warned_event){fprintf(stderr,"[drm] DRM event polling failed: %s\n",strerror(errno));st->warned_event=true;}
 return result;
}

void backend_drm_poll(Server *s){
 DrmState *st=drm_state(s);if(!st)return;
 if(s->fbdev_evdev)backend_evdev_poll(s);
 drm_poll_events(s,st);
}

PresentResult backend_drm_present(Server *s){
 DrmState *st=drm_state(s);
 if(!st||st->egl_surface==EGL_NO_SURFACE||!st->master)return CD_PRESENT_SKIPPED;
 if(backend_vt_is_suspended(s))return CD_PRESENT_SKIPPED;
 /* A pending flip owns the next GBM buffer until its event releases the old
  * one. Reap only events that are already available; if scanout is still busy,
  * skip this output frame rather than blocking client callbacks on vblank.
  * VT release/shutdown retain their bounded drains before destroying buffers. */
 if(st->page_flip_pending){
  if(drm_poll_events(s,st)==-2)return CD_PRESENT_INTERRUPTED;
  if(st->page_flip_pending)return CD_PRESENT_SKIPPED;
 }
 if(!st->master||backend_vt_is_suspended(s))return CD_PRESENT_SKIPPED;
 if(!eglSwapBuffers(s->egl_display,st->egl_surface)){
  fprintf(stderr,"[drm] eglSwapBuffers failed: 0x%x\n",eglGetError());return CD_PRESENT_SKIPPED;
 }
 struct gbm_bo *bo=gbm_surface_lock_front_buffer(st->surface);
 if(!bo){fprintf(stderr,"[drm] gbm_surface_lock_front_buffer failed\n");return CD_PRESENT_SKIPPED;}
 /* A VT signal can arrive while EGL is swapping even when no older flip was
  * pending. Poll once more before KMS registration/queueing; on interruption
  * release this newly locked BO and let main perform the managed VT release. */
 if(drm_poll_events(s,st)==-2){drm_release_bo(st,bo);return CD_PRESENT_INTERRUPTED;}
 if(!st->master||backend_vt_is_suspended(s)){drm_release_bo(st,bo);return CD_PRESENT_SKIPPED;}
 uint32_t fb_id=drm_bo_framebuffer(st,bo);
 if(!fb_id){drm_release_bo(st,bo);return CD_PRESENT_SKIPPED;}
 if(!st->mode_set){
  if(drmModeSetCrtc(st->fd,st->crtc_id,fb_id,0,0,&st->connector_id,1,&st->mode)<0){
   fprintf(stderr,"[drm] initial drmModeSetCrtc failed: %s\n",strerror(errno));drm_release_bo(st,bo);return CD_PRESENT_SKIPPED;
  }
  st->mode_set=true;st->current_bo=bo;
  /* SetCrtc installs the first buffer immediately. Later presents queue at
   * most one flip and use its event solely for buffer retirement, not pacing. */
  return CD_PRESENT_OK;
 }
 if(drmModePageFlip(st->fd,st->crtc_id,fb_id,DRM_MODE_PAGE_FLIP_EVENT,st)<0){
  fprintf(stderr,"[drm] drmModePageFlip failed: %s\n",strerror(errno));drm_release_bo(st,bo);return CD_PRESENT_SKIPPED;
 }
 st->pending_bo=bo;st->page_flip_pending=true;
 /* The flip is queued, not completed. Its event is collected by the ordinary
  * nonblocking backend poll; client pacing must not wait for physical scanout. */
 return CD_PRESENT_OK;
}

static void drm_restore_crtc(DrmState *st){
 if(!st->mode_set)return;
 int result;
 if(st->saved_crtc&&st->saved_crtc->mode_valid){
  uint32_t *connectors=st->saved_connector_count>0?st->saved_connectors:&st->connector_id;
  int connector_count=st->saved_connector_count>0?st->saved_connector_count:1;
  result=drmModeSetCrtc(st->fd,st->crtc_id,st->saved_crtc->buffer_id,st->saved_crtc->x,st->saved_crtc->y,
   connectors,connector_count,&st->saved_crtc->mode);
  /* A clone may have disappeared since startup. Do not let one stale connector
   * prevent restoration of every other originally active output. */
  if(result<0&&st->saved_connector_count>1){
   fprintf(stderr,"[drm] cloned CRTC restore failed; trying original connectors individually\n");
   for(int i=0;i<st->saved_connector_count;i++){
    uint32_t connector=st->saved_connectors[i];
    result=drmModeSetCrtc(st->fd,st->crtc_id,st->saved_crtc->buffer_id,st->saved_crtc->x,st->saved_crtc->y,
     &connector,1,&st->saved_crtc->mode);
    if(result==0)break;
   }
  }
 }
 else result=drmModeSetCrtc(st->fd,st->crtc_id,0,0,0,NULL,0,NULL);
 if(result<0)fprintf(stderr,"[drm] failed to restore the previous CRTC: %s\n",strerror(errno));
 else st->mode_set=false;
}

/* Called before the release acknowledgement. Drain the outstanding flip while
 * still master, disable the CRTC, then release only buffers that are provably
 * no longer scanned out. Failure is fatal to the session: main will quit
 * instead of trying to reacquire stale DRM state, but still acknowledges VT. */
bool backend_drm_release_for_vt(Server *s){
 DrmState *st=drm_state(s);
 if(!st||st->fd<0)return true;
 bool drained=true,retired=true,dropped=true;
 if(st->page_flip_pending)drained=drm_wait_for_flip(s,1000);
 if(st->mode_set){
  if(drmModeSetCrtc(st->fd,st->crtc_id,0,0,0,NULL,0,NULL)<0){
   fprintf(stderr,"[drm] disabling CRTC for VT release failed: %s\n",strerror(errno));retired=false;
  }
  st->mode_set=false;
 }
 if(drained&&retired){
  if(st->current_bo){drm_release_bo(st,st->current_bo);st->current_bo=NULL;}
  if(st->pending_bo){drm_release_bo(st,st->pending_bo);st->pending_bo=NULL;}
  st->page_flip_pending=false;
 }else fprintf(stderr,"[drm] VT release could not retire all scanout buffers; retaining them for cleanup\n");
 if(st->master){
  if(drmDropMaster(st->fd)<0){
   fprintf(stderr,"[drm] drmDropMaster for VT release failed: %s\n",strerror(errno));dropped=false;
  }
  /* A failed drop is not proof that master survives the subsequent VT
   * switch. Always force the next successful acquire through drmSetMaster. */
  st->master=false;
 }
 return drained&&retired&&dropped;
}

bool backend_drm_acquire_after_vt(Server *s){
 DrmState *st=drm_state(s);
 if(!st||st->fd<0)return false;
 if(!st->master){
  if(drmSetMaster(st->fd)<0){
   fprintf(stderr,"[drm] drmSetMaster after VT acquire failed: %s\n",strerror(errno));
   return false;
  }
  st->master=true;
 }
 return true;
}

static void drm_destroy(Server *s,bool inactive){
 DrmState *st=drm_state(s);if(!st)return;
 if(s->evdev_state)backend_evdev_shutdown(s);
 if(st->fd>=0){
  if(!inactive){
   if(st->page_flip_pending)drm_wait_for_flip(s,1000);
   drm_restore_crtc(st);
  }else{
   fprintf(stderr,"[drm] cleaning up while VT is inactive; no CRTC restore or flip wait\n");
  }
  if(st->current_bo){drm_release_bo(st,st->current_bo);st->current_bo=NULL;}
  if(st->pending_bo){drm_release_bo(st,st->pending_bo);st->pending_bo=NULL;st->page_flip_pending=false;}
 }
 if(s->egl_display!=EGL_NO_DISPLAY){
  eglMakeCurrent(s->egl_display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
  if(st->egl_surface!=EGL_NO_SURFACE)eglDestroySurface(s->egl_display,st->egl_surface);
  if(s->egl_context!=EGL_NO_CONTEXT)eglDestroyContext(s->egl_display,s->egl_context);
  eglTerminate(s->egl_display);
 }
 st->egl_surface=EGL_NO_SURFACE;s->egl_display=EGL_NO_DISPLAY;s->egl_context=EGL_NO_CONTEXT;s->target_fbo=0;
 if(st->surface)gbm_surface_destroy(st->surface);
 if(st->gbm)gbm_device_destroy(st->gbm);
 if(st->saved_crtc)drmModeFreeCrtc(st->saved_crtc);
 free(st->saved_connectors);
 if(st->fd>=0){if(st->master)drmDropMaster(st->fd);close(st->fd);}
 free(st);s->drm_state=NULL;
}

void backend_drm_shutdown(Server *s){
 DrmState *st=drm_state(s);
 /* Input reacquisition runs after drmSetMaster. If it then fails, the VT
  * stays suspended to prevent frames, but this VT is active and master is
  * held again: cleanup must restore the saved CRTC rather than taking the
  * inactive-VT path. A genuinely released session is masterless. */
 bool suspended=backend_vt_is_suspended(s);
 bool reacquired=suspended&&st&&st->master;
 /* Release cleared mode_set after disabling the CRTC. No new frame is allowed
  * on this failure path, but the original console mode still needs restoring. */
 if(reacquired)st->mode_set=true;
 drm_destroy(s,suspended&&!reacquired);
 backend_vt_shutdown(s);
}
