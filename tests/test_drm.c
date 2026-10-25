#include "chrome.h"

#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;} } while(0)

#define MAX_FBS 32
#define MAX_BOS 32
#define MAX_EVENTS 8
#define MAX_CRTC_CALLS 16

typedef struct {
 bool active;
 uint32_t id;
 uint32_t handle;
} FakeFb;

typedef struct {
 uint32_t id, handle;
 uint32_t stride, width, height, offset;
 uint64_t modifier;
 bool locked;
 int releases;
} FakeBoInfo;

struct gbm_bo {
 FakeBoInfo info;
 struct gbm_surface *owner;
};

struct gbm_device {
 int fd;
 bool destroyed;
};

struct gbm_surface {
 uint32_t width, height, format, flags;
 struct gbm_bo *bos[MAX_BOS];
 int bo_count;
 int locked_count;
 int swaps;
 bool destroyed;
};

typedef struct {
 uint32_t crtc_id, buffer_id, x, y;
 uint32_t connector_id;
 uint32_t connector_ids[4];
 int connector_count;
 drmModeModeInfo mode;
 bool has_mode;
 bool ok;
} CrtcCall;

enum EglFail {
 EG_NONE = 0,
 EG_NO_PROC,
 EG_PLATFORM_NULL,
 EG_INIT,
 EG_BIND,
 EG_CHOOSE,
 EG_VISUAL,
 EG_CONTEXT,
 EG_GBM_SURFACE,
 EG_WINDOW_SURFACE,
 EG_MAKE_CURRENT,
 EG_SWAP
};

static struct {
 drmModeRes resources;
 drmModeConnector connector,connector2;
 drmModeEncoder encoder,encoder2;
 drmModeCrtc crtc;
 drmModeModeInfo modes[4];
 uint32_t connectors[2];
 uint32_t crtcs[1];
 uint32_t encoders[1],encoders2[1];
 bool have_crtc,have_second_connector;

 bool fail_set_master;
 bool fail_get_resources;
 bool fail_get_connector;
 bool fail_get_encoder;
 bool fail_get_crtc;
 bool fail_add_fb;
 bool fail_add_fb2_modifiers;
 bool fail_set_crtc;
 bool fail_cloned_restore;
 bool fail_page_flip;
 bool fail_rm_fb;
 bool fail_handle_event;
 bool fail_gbm_create;
 bool fail_lock_front;
 uint64_t modifier_cap,bo_modifier;
 uint32_t bo_offset;
 enum EglFail egl_fail;

 bool auto_complete_flip;
 bool poll_fail;
 bool poll_bad_revents;

 int set_master_calls;
 int drop_master_calls;
 int master_fd;
 bool master_active;

 int add_fb_calls;
 int add_fb2_calls,add_fb2_modifier_calls;
 uint32_t last_fb_offset;
 uint64_t last_fb_modifier;
 bool last_fb_with_modifiers;
 int rm_fb_calls;
 int set_crtc_calls;
 CrtcCall crtc_calls[MAX_CRTC_CALLS];
 int page_flip_calls;
 int gbm_create_calls;
 int gbm_create_success;
 int gbm_destroy_calls;
 int surface_create_calls;
 int surface_create_success;
 int surface_destroy_calls;
 int egl_initialize_calls;
 int egl_terminate_calls;
 int egl_context_create_calls;
 int egl_context_create_success;
 int egl_context_destroy_calls;
 int egl_surface_create_calls;
 int egl_surface_create_success;
 int egl_surface_destroy_calls;
 int egl_make_current_calls;
 int egl_make_current_none_calls;
 int egl_swap_calls;
 int egl_swap_interval_calls;
 int gl_get_string_calls;
 int evdev_init_calls;
 int evdev_poll_calls;
 int evdev_shutdown_calls;
 bool evdev_init_result;
 bool evdev_alloc_state;
 EvdevState evdev;

 int poll_calls;
 int poll_timeouts;
 int poll_errors;

 int violations;
 int max_fb_count;
 int max_locked_bos;
 int add_fb_over_bound;

 uint32_t next_fb_id;
 uint32_t next_handle;
 FakeFb fbs[MAX_FBS];
 int fb_count;
 struct gbm_bo *all_bos[MAX_BOS];
 int all_bo_count;
 struct gbm_device *gbm;
 struct gbm_surface *surface;

 struct {
  void *user_data;
 } events[MAX_EVENTS];
 int event_head;
 int event_count;
 void *last_flip_user_data;
} W;

static void violation(const char *what) {
 fprintf(stderr,"fake KMS violation: %s\n",what);
 W.violations++;
}

static void reset_world(void) {
 for(int i=0;i<W.all_bo_count;i++)free(W.all_bos[i]);
 if(W.surface)free(W.surface);
 if(W.gbm)free(W.gbm);
 memset(&W,0,sizeof(W));
 W.master_fd=-1;
 W.evdev_init_result=true;
 W.evdev_alloc_state=true;
 W.evdev.device_count=1;

 W.resources.count_connectors=1;
 W.resources.connectors=W.connectors;
 W.resources.count_crtcs=1;
 W.resources.crtcs=W.crtcs;
 W.resources.count_encoders=1;
 W.resources.encoders=W.encoders;
 W.resources.min_width=320;
 W.resources.max_width=4096;
 W.resources.min_height=200;
 W.resources.max_height=4096;

 W.connectors[0]=7;
 W.crtcs[0]=3;
 W.encoders[0]=9;

 memset(&W.connector,0,sizeof(W.connector));
 W.connector.connector_id=7;
 W.connector.encoder_id=9;
 W.connector.connector_type=DRM_MODE_CONNECTOR_HDMIA;
 W.connector.connector_type_id=1;
 W.connector.connection=DRM_MODE_CONNECTED;
 W.connector.count_modes=2;
 W.connector.modes=W.modes;
 W.connector.count_encoders=1;
 W.connector.encoders=W.encoders;

 W.connectors[1]=8;W.encoders2[0]=10;
 memset(&W.connector2,0,sizeof(W.connector2));
 W.connector2.connector_id=8;
 W.connector2.encoder_id=10;
 W.connector2.connector_type=DRM_MODE_CONNECTOR_HDMIA;
 W.connector2.connector_type_id=2;
 W.connector2.connection=DRM_MODE_CONNECTED;
 W.connector2.count_modes=2;
 W.connector2.modes=W.modes;
 W.connector2.count_encoders=1;
 W.connector2.encoders=W.encoders2;

 memset(&W.encoder,0,sizeof(W.encoder));
 W.encoder.encoder_id=9;
 W.encoder.crtc_id=3;
 W.encoder.possible_crtcs=1;
 memset(&W.encoder2,0,sizeof(W.encoder2));
 W.encoder2.encoder_id=10;
 W.encoder2.crtc_id=3;
 W.encoder2.possible_crtcs=1;

 memset(&W.modes,0,sizeof(W.modes));
 W.modes[0].hdisplay=800;
 W.modes[0].vdisplay=600;
 W.modes[0].vrefresh=60;
 W.modes[0].type=DRM_MODE_TYPE_PREFERRED;
 W.modes[1].hdisplay=1920;
 W.modes[1].vdisplay=1080;
 W.modes[1].vrefresh=60;

 memset(&W.crtc,0,sizeof(W.crtc));
 W.crtc.crtc_id=3;
 W.crtc.buffer_id=42;
 W.crtc.x=5;
 W.crtc.y=7;
 W.crtc.width=800;
 W.crtc.height=600;
 W.crtc.mode_valid=1;
 W.crtc.mode=W.modes[0];
 W.have_crtc=true;

 W.next_fb_id=100;
 W.next_handle=1000;
 W.bo_modifier=DRM_FORMAT_MOD_LINEAR;
 W.auto_complete_flip=true;
}

static void init_server(Server *s,const char *drm_path) {
 memset(s,0,sizeof(*s));
 s->egl_display=EGL_NO_DISPLAY;
 s->egl_context=EGL_NO_CONTEXT;
 s->target_fbo=77;
 s->fbo_color_rb=78;
 s->fbo_depth_rb=79;
 s->readback_pixels=(uint8_t *)(uintptr_t)1;
 snprintf(s->drm_path,sizeof(s->drm_path),"%s",drm_path);
}

static void make_temp_card(char *path,size_t size) {
 snprintf(path,size,"/tmp/cd-drm-test-XXXXXX");
 int fd=mkstemp(path);
 CHECK(fd>=0);
 if(fd>=0)close(fd);
}

static bool fd_is_closed(int fd) {
 if(fd<0)return true;
 errno=0;
 return fcntl(fd,F_GETFD)==-1&&errno==EBADF;
}

static int active_locked_bos(void) {
 int n=0;
 for(int i=0;i<W.all_bo_count;i++)if(W.all_bos[i]->info.locked)n++;
 return n;
}

static struct gbm_bo *new_bo(struct gbm_surface *surface) {
 if(W.all_bo_count>=MAX_BOS){violation("too many GBM buffers");errno=ENOMEM;return NULL;}
 struct gbm_bo *bo=calloc(1,sizeof(*bo));
 if(!bo){errno=ENOMEM;return NULL;}
 bo->info.id=W.next_handle;
 bo->info.handle=W.next_handle++;
 bo->info.width=surface->width;
 bo->info.height=surface->height;
 bo->info.stride=surface->width*4u;
 bo->info.offset=W.bo_offset;
 bo->info.modifier=W.bo_modifier;
 bo->info.locked=true;
 bo->owner=surface;
 W.all_bos[W.all_bo_count++]=bo;
 if(surface->bo_count>=MAX_BOS){violation("surface buffer list full");return NULL;}
 surface->bos[surface->bo_count++]=bo;
 surface->locked_count++;
 int locked=active_locked_bos();
 if(locked>W.max_locked_bos)W.max_locked_bos=locked;
 return bo;
}

static void enqueue_flip_event(void *user_data) {
 if(W.event_count>=MAX_EVENTS){violation("event queue overflow");return;}
 int index=(W.event_head+W.event_count)%MAX_EVENTS;
 W.events[index].user_data=user_data;
 W.event_count++;
}

static FakeFb *find_fb(uint32_t id) {
 for(int i=0;i<MAX_FBS;i++)if(W.fbs[i].active&&W.fbs[i].id==id)return &W.fbs[i];
 return NULL;
}

static FakeFb *find_fb_by_handle(uint32_t handle) {
 for(int i=0;i<MAX_FBS;i++)if(W.fbs[i].active&&W.fbs[i].handle==handle)return &W.fbs[i];
 return NULL;
}

int drmSetMaster(int fd) {
 W.set_master_calls++;
 W.master_fd=fd;
 if(W.fail_set_master){errno=EPERM;return -1;}
 W.master_active=true;
 return 0;
}

int drmDropMaster(int fd) {
 W.drop_master_calls++;
 if(fd!=W.master_fd)violation("drmDropMaster on wrong fd");
 if(!W.master_active)violation("drmDropMaster without master");
 W.master_active=false;
 return 0;
}

drmModeResPtr drmModeGetResources(int fd) {
 (void)fd;
 W.resources.count_connectors=W.have_second_connector?2:1;
 if(W.fail_get_resources){errno=EIO;return NULL;}
 return &W.resources;
}

void drmModeFreeResources(drmModeResPtr resources) {
 if(resources!=&W.resources)violation("freeing foreign resources");
}

drmModeConnectorPtr drmModeGetConnector(int fd,uint32_t connector_id) {
 (void)fd;
 if(W.fail_get_connector){errno=ENOENT;return NULL;}
 if(connector_id==W.connector.connector_id)return &W.connector;
 if(W.have_second_connector&&connector_id==W.connector2.connector_id)return &W.connector2;
 errno=ENOENT;return NULL;
}

void drmModeFreeConnector(drmModeConnectorPtr connector) {
 if(connector!=&W.connector&&(!W.have_second_connector||connector!=&W.connector2))violation("freeing foreign connector");
}

drmModeEncoderPtr drmModeGetEncoder(int fd,uint32_t encoder_id) {
 (void)fd;
 if(W.fail_get_encoder){errno=ENOENT;return NULL;}
 if(encoder_id==W.encoder.encoder_id)return &W.encoder;
 if(W.have_second_connector&&encoder_id==W.encoder2.encoder_id)return &W.encoder2;
 errno=ENOENT;return NULL;
}

void drmModeFreeEncoder(drmModeEncoderPtr encoder) {
 if(encoder!=&W.encoder&&(!W.have_second_connector||encoder!=&W.encoder2))violation("freeing foreign encoder");
}

drmModeCrtcPtr drmModeGetCrtc(int fd,uint32_t crtc_id) {
 (void)fd;
 if(W.fail_get_crtc||!W.have_crtc||crtc_id!=W.crtc.crtc_id){errno=ENOENT;return NULL;}
 return &W.crtc;
}

void drmModeFreeCrtc(drmModeCrtcPtr crtc) {
 if(crtc!=&W.crtc)violation("freeing foreign CRTC");
}

const char *drmModeGetConnectorTypeName(uint32_t connector_type) {
 (void)connector_type;
 return "FakeHDMI";
}

int drmGetCap(int fd,uint64_t capability,uint64_t *value) {
 (void)fd;
 if(!value){errno=EINVAL;return -1;}
 if(capability==DRM_CAP_ADDFB2_MODIFIERS){*value=W.modifier_cap;return 0;}
 errno=EINVAL;return -1;
}

int drmModeAddFB(int fd,uint32_t width,uint32_t height,uint8_t depth,uint8_t bpp,
 uint32_t pitch,uint32_t bo_handle,uint32_t *buf_id) {
 (void)fd;
 W.add_fb_calls++;
 if(W.fb_count>=2)W.add_fb_over_bound++;
 if(W.fail_add_fb){errno=EIO;return -1;}
 if(width==0||height==0||depth!=24||bpp!=32||pitch==0||!buf_id){violation("invalid drmModeAddFB arguments");errno=EINVAL;return -1;}
 if(find_fb_by_handle(bo_handle)){violation("duplicate AddFB for GBM handle");errno=EINVAL;return -1;}
 int slot=-1;
 for(int i=0;i<MAX_FBS;i++)if(!W.fbs[i].active){slot=i;break;}
 if(slot<0){violation("framebuffer table full");errno=ENOSPC;return -1;}
 uint32_t id=W.next_fb_id++;
 W.fbs[slot]=(FakeFb){.active=true,.id=id,.handle=bo_handle};
 W.fb_count++;
 if(W.fb_count>W.max_fb_count)W.max_fb_count=W.fb_count;
 *buf_id=id;
 return 0;
}

static int fake_add_fb2(int fd,uint32_t width,uint32_t height,uint32_t format,
 const uint32_t handles[4],const uint32_t pitches[4],const uint32_t offsets[4],
 const uint64_t modifiers[4],uint32_t *fb_id,uint32_t flags,bool with_modifiers) {
 W.add_fb2_calls++;
 if(with_modifiers)W.add_fb2_modifier_calls++;
 if(with_modifiers&&W.fail_add_fb2_modifiers){errno=EIO;return -1;}
 if(format!=GBM_FORMAT_XRGB8888||!handles||!pitches||!offsets||!fb_id||(with_modifiers&&!modifiers)){
  violation("invalid AddFB2 arguments");errno=EINVAL;return -1;
 }
 for(int i=1;i<4;i++)if(handles[i]||pitches[i]||offsets[i]||(with_modifiers&&modifiers[i])){
  violation("unexpected extra plane in XRGB8888 AddFB2");errno=EINVAL;return -1;
 }
 if(with_modifiers&&(!(flags&DRM_MODE_FB_MODIFIERS)||!modifiers)){
  violation("AddFB2 modifiers flag/array mismatch");errno=EINVAL;return -1;
 }
 W.last_fb_offset=offsets[0];
 W.last_fb_modifier=with_modifiers?modifiers[0]:DRM_FORMAT_MOD_INVALID;
 W.last_fb_with_modifiers=with_modifiers;
 /* The fake registry is shared with legacy AddFB; the production call under
  * test is still the real AddFB2/AddFB2WithModifiers branch. */
 return drmModeAddFB(fd,width,height,24,32,pitches[0],handles[0],fb_id);
}

int drmModeAddFB2(int fd,uint32_t width,uint32_t height,uint32_t format,
 const uint32_t handles[4],const uint32_t pitches[4],const uint32_t offsets[4],
 uint32_t *fb_id,uint32_t flags) {
 return fake_add_fb2(fd,width,height,format,handles,pitches,offsets,NULL,fb_id,flags,false);
}

int drmModeAddFB2WithModifiers(int fd,uint32_t width,uint32_t height,uint32_t format,
 const uint32_t handles[4],const uint32_t pitches[4],const uint32_t offsets[4],
 const uint64_t modifiers[4],uint32_t *fb_id,uint32_t flags) {
 return fake_add_fb2(fd,width,height,format,handles,pitches,offsets,modifiers,fb_id,flags,true);
}

int drmModeRmFB(int fd,uint32_t buffer_id) {
 (void)fd;
 W.rm_fb_calls++;
 FakeFb *fb=find_fb(buffer_id);
 if(!fb){violation("drmModeRmFB for unknown framebuffer");errno=EINVAL;return -1;}
 if(W.fail_rm_fb){errno=EIO;return -1;}
 fb->active=false;
 W.fb_count--;
 return 0;
}

int drmModeSetCrtc(int fd,uint32_t crtc_id,uint32_t buffer_id,uint32_t x,uint32_t y,
 uint32_t *connectors,int count,drmModeModeInfoPtr mode) {
 (void)fd;
 bool fail=W.fail_set_crtc||(W.fail_cloned_restore&&count>1);
 CrtcCall *call=NULL;
 if(W.set_crtc_calls<MAX_CRTC_CALLS)call=&W.crtc_calls[W.set_crtc_calls];
 W.set_crtc_calls++;
 if(call){
  call->crtc_id=crtc_id;
  call->buffer_id=buffer_id;
  call->x=x;
  call->y=y;
  call->connector_count=count;
  call->connector_id=(connectors&&count>0)?connectors[0]:0;
  for(int i=0;connectors&&i<count&&i<4;i++)call->connector_ids[i]=connectors[i];
  call->has_mode=mode!=NULL;
  if(mode)call->mode=*mode;
  call->ok=!fail;
 }
 if(fail){errno=EIO;return -1;}
 if(crtc_id!=W.crtc.crtc_id){violation("SetCrtc on unexpected CRTC");errno=EINVAL;return -1;}
 if(count>0&&!connectors)violation("SetCrtc connector mismatch");
 for(int i=0;connectors&&i<count;i++)if(connectors[i]!=W.connector.connector_id&&
  (!W.have_second_connector||connectors[i]!=W.connector2.connector_id))violation("SetCrtc connector mismatch");
 return 0;
}

int drmModePageFlip(int fd,uint32_t crtc_id,uint32_t fb_id,uint32_t flags,void *user_data) {
 (void)fd;
 W.page_flip_calls++;
 if(W.fail_page_flip){errno=EIO;return -1;}
 if(crtc_id!=W.crtc.crtc_id)violation("PageFlip on unexpected CRTC");
 if(!find_fb(fb_id))violation("PageFlip with inactive framebuffer");
 if(!(flags&DRM_MODE_PAGE_FLIP_EVENT))violation("PageFlip without event flag");
 W.last_flip_user_data=user_data;
 if(W.auto_complete_flip)enqueue_flip_event(user_data);
 return 0;
}

int drmHandleEvent(int fd,drmEventContextPtr context) {
 if(W.fail_handle_event){errno=EIO;return -1;}
 if(!context)return 0;
 if(W.event_count==0)return 0;
 void *user_data=W.events[W.event_head].user_data;
 W.event_head=(W.event_head+1)%MAX_EVENTS;
 W.event_count--;
 if(context->page_flip_handler)
  context->page_flip_handler(fd,1,0,0,user_data);
 else if(context->page_flip_handler2)
  context->page_flip_handler2(fd,1,0,0,W.crtc.crtc_id,user_data);
 else if(context->vblank_handler)
  context->vblank_handler(fd,1,0,0,user_data);
 return 0;
}

struct gbm_device *gbm_create_device(int fd) {
 W.gbm_create_calls++;
 if(W.fail_gbm_create){errno=ENODEV;return NULL;}
 W.gbm=calloc(1,sizeof(*W.gbm));
 if(!W.gbm){errno=ENOMEM;return NULL;}
 W.gbm->fd=fd;
 W.gbm_create_success++;
 return W.gbm;
}

void gbm_device_destroy(struct gbm_device *gbm) {
 if(gbm!=W.gbm)violation("destroying foreign GBM device");
 if(gbm&&gbm->destroyed)violation("double GBM device destroy");
 if(gbm)gbm->destroyed=true;
 W.gbm_destroy_calls++;
}

struct gbm_surface *gbm_surface_create(struct gbm_device *gbm,uint32_t width,uint32_t height,
 uint32_t format,uint32_t flags) {
 W.surface_create_calls++;
 if(W.egl_fail==EG_GBM_SURFACE||gbm!=W.gbm){errno=EIO;return NULL;}
 W.surface=calloc(1,sizeof(*W.surface));
 if(!W.surface){errno=ENOMEM;return NULL;}
 W.surface_create_success++;
 W.surface->width=width;
 W.surface->height=height;
 W.surface->format=format;
 W.surface->flags=flags;
 return W.surface;
}

void gbm_surface_destroy(struct gbm_surface *surface) {
 if(surface!=W.surface)violation("destroying foreign GBM surface");
 if(surface&&surface->destroyed)violation("double GBM surface destroy");
 if(surface&&surface->locked_count!=0)violation("destroying GBM surface with locked buffers");
 if(surface)surface->destroyed=true;
 W.surface_destroy_calls++;
}

struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface) {
 if(!surface||surface!=W.surface||surface->destroyed||W.fail_lock_front){errno=EIO;return NULL;}
 surface->swaps++;
 return new_bo(surface);
}

void gbm_surface_release_buffer(struct gbm_surface *surface,struct gbm_bo *bo) {
 if(!surface||surface!=W.surface){violation("release on foreign GBM surface");return;}
 if(!bo||bo->owner!=surface){violation("release of foreign GBM buffer");return;}
 if(!bo->info.locked){violation("double release of GBM buffer");return;}
 bo->info.locked=false;
 bo->info.releases++;
 surface->locked_count--;
}

uint32_t gbm_bo_get_width(struct gbm_bo *bo) {
 if(!bo){violation("gbm_bo_get_width(NULL)");return 0;}
 return bo->info.width;
}

uint32_t gbm_bo_get_height(struct gbm_bo *bo) {
 if(!bo){violation("gbm_bo_get_height(NULL)");return 0;}
 return bo->info.height;
}

uint32_t gbm_bo_get_stride(struct gbm_bo *bo) {
 if(!bo){violation("gbm_bo_get_stride(NULL)");return 0;}
 return bo->info.stride;
}

uint32_t gbm_bo_get_offset(struct gbm_bo *bo,int plane) {
 if(!bo||plane!=0){violation("invalid GBM plane offset query");return 0;}
 return bo->info.offset;
}

uint64_t gbm_bo_get_modifier(struct gbm_bo *bo) {
 if(!bo){violation("gbm_bo_get_modifier(NULL)");return DRM_FORMAT_MOD_INVALID;}
 return bo->info.modifier;
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo) {
 union gbm_bo_handle handle;
 memset(&handle,0,sizeof(handle));
 if(!bo){violation("gbm_bo_get_handle(NULL)");return handle;}
 handle.u32=bo->info.handle;
 return handle;
}

static EGLDisplay fake_display=(EGLDisplay)(uintptr_t)0x1001;
static EGLContext fake_context=(EGLContext)(uintptr_t)0x1002;
static EGLSurface fake_egl_surface=(EGLSurface)(uintptr_t)0x1003;
static EGLConfig fake_config=(EGLConfig)(uintptr_t)0x1004;

static EGLDisplay fake_get_platform_display(EGLenum platform,void *native_display,const EGLint *attrib_list) {
 (void)attrib_list;
 if(platform!=EGL_PLATFORM_GBM_KHR)violation("unexpected EGL platform");
 if(native_display!=W.gbm)violation("EGL platform display without GBM device");
 if(W.egl_fail==EG_PLATFORM_NULL)return EGL_NO_DISPLAY;
 return fake_display;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
 if(W.egl_fail==EG_NO_PROC)return NULL;
 if(!procname||strcmp(procname,"eglGetPlatformDisplayEXT")!=0)return NULL;
 return (__eglMustCastToProperFunctionPointerType)fake_get_platform_display;
}

EGLBoolean eglInitialize(EGLDisplay dpy,EGLint *major,EGLint *minor) {
 W.egl_initialize_calls++;
 if(dpy!=fake_display||W.egl_fail==EG_INIT)return EGL_FALSE;
 if(major)*major=1;
 if(minor)*minor=5;
 return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api) {
 (void)api;
 return W.egl_fail==EG_BIND?EGL_FALSE:EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy,const EGLint *attrib_list,EGLConfig *configs,
 EGLint config_size,EGLint *num_config) {
 bool depth=false,stencil=false;
 for(int i=0;attrib_list&&attrib_list[i]!=EGL_NONE;i+=2){
  if(attrib_list[i]==EGL_DEPTH_SIZE&&attrib_list[i+1]>=24)depth=true;
  if(attrib_list[i]==EGL_STENCIL_SIZE&&attrib_list[i+1]>=8)stencil=true;
 }
 if(!depth||!stencil)violation("DRM EGL config lacks direct-render depth/stencil");
 if(dpy!=fake_display||config_size<1||!configs||!num_config)return EGL_FALSE;
 if(W.egl_fail==EG_CHOOSE){*num_config=0;return EGL_FALSE;}
 configs[0]=fake_config;
 *num_config=1;
 return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy,EGLConfig config,EGLint attribute,EGLint *value) {
 if(dpy!=fake_display||config!=fake_config||!value)return EGL_FALSE;
 if(attribute==EGL_NATIVE_VISUAL_ID)
  *value=W.egl_fail==EG_VISUAL?0x1234:(EGLint)GBM_FORMAT_XRGB8888;
 else
  *value=0;
 return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy,EGLConfig config,EGLContext share_context,
 const EGLint *attrib_list) {
 (void)share_context;(void)attrib_list;
 W.egl_context_create_calls++;
 if(dpy!=fake_display||config!=fake_config||W.egl_fail==EG_CONTEXT)return EGL_NO_CONTEXT;
 W.egl_context_create_success++;
 return fake_context;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy,EGLConfig config,EGLNativeWindowType win,
 const EGLint *attrib_list) {
 (void)attrib_list;
 W.egl_surface_create_calls++;
 if(dpy!=fake_display||config!=fake_config||(void *)win!=(void *)W.surface||W.egl_fail==EG_WINDOW_SURFACE)
  return EGL_NO_SURFACE;
 W.egl_surface_create_success++;
 return fake_egl_surface;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy,EGLSurface draw,EGLSurface read,EGLContext ctx) {
 W.egl_make_current_calls++;
 if(draw==EGL_NO_SURFACE&&read==EGL_NO_SURFACE&&ctx==EGL_NO_CONTEXT)
  W.egl_make_current_none_calls++;
 if(dpy!=fake_display||W.egl_fail==EG_MAKE_CURRENT)return EGL_FALSE;
 return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy,EGLint interval) {
 (void)interval;
 if(dpy!=fake_display)return EGL_FALSE;
 W.egl_swap_interval_calls++;
 return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy,EGLSurface surface) {
 W.egl_swap_calls++;
 if(dpy!=fake_display||surface!=fake_egl_surface||W.egl_fail==EG_SWAP)return EGL_FALSE;
 return EGL_TRUE;
}

EGLint eglGetError(void) {
 return W.egl_fail==EG_NONE?EGL_SUCCESS:0x3001;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy,EGLSurface surface) {
 W.egl_surface_destroy_calls++;
 if(dpy!=fake_display||surface!=fake_egl_surface)violation("destroying foreign EGL surface");
 return EGL_TRUE;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy,EGLContext ctx) {
 W.egl_context_destroy_calls++;
 if(dpy!=fake_display||ctx!=fake_context)violation("destroying foreign EGL context");
 return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
 W.egl_terminate_calls++;
 if(dpy!=fake_display)violation("terminating foreign EGL display");
 return EGL_TRUE;
}

const GLubyte *glGetString(GLenum name) {
 W.gl_get_string_calls++;
 if(name==GL_RENDERER)return (const GLubyte *)"fake-renderer";
 if(name==GL_VERSION)return (const GLubyte *)"fake-version";
 return (const GLubyte *)"fake";
}

int poll(struct pollfd *fds,nfds_t nfds,int timeout) {
 (void)timeout;
 W.poll_calls++;
 if(W.poll_fail){W.poll_errors++;errno=EIO;return -1;}
 if(!fds||nfds<1){W.poll_errors++;errno=EINVAL;return -1;}
 fds[0].revents=0;
 if(W.poll_bad_revents){fds[0].revents=POLLERR;W.poll_errors++;return 1;}
 if(W.event_count>0){fds[0].revents=POLLIN;return 1;}
 W.poll_timeouts++;
 return 0;
}

bool backend_evdev_init(Server *s) {
 W.evdev_init_calls++;
 if(!W.evdev_init_result)return false;
 if(W.evdev_alloc_state)s->evdev_state=&W.evdev;
 return true;
}

void backend_evdev_poll(Server *s) {
 (void)s;
 W.evdev_poll_calls++;
}

void backend_evdev_shutdown(Server *s) {
 W.evdev_shutdown_calls++;
 if(s->evdev_state==&W.evdev)s->evdev_state=NULL;
}

static bool init_ok(Server *s,const char *path) {
 init_server(s,path);
 bool ok=backend_drm_init(s);
 CHECK(ok);
 return ok;
}

static void check_all_clean(Server *s,bool master_expected) {
 CHECK(W.violations==0);
 CHECK(W.fb_count==0);
 CHECK(active_locked_bos()==0);
 CHECK(W.gbm_create_success==W.gbm_destroy_calls);
 CHECK(W.surface_create_success==W.surface_destroy_calls);
 CHECK(W.egl_context_create_success==W.egl_context_destroy_calls);
 CHECK(W.egl_surface_create_success==W.egl_surface_destroy_calls);
 CHECK(W.max_fb_count<=2);
 CHECK(W.max_locked_bos<=2);
 CHECK(W.add_fb_over_bound==0);
 CHECK(s->drm_state==NULL);
 CHECK(s->egl_display==EGL_NO_DISPLAY);
 CHECK(s->egl_context==EGL_NO_CONTEXT);
 CHECK(s->target_fbo==0);
 CHECK(!W.master_active);
 CHECK(W.drop_master_calls==(master_expected?1:0));
 CHECK(fd_is_closed(W.master_fd));
}

static void test_success_lifecycle(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 init_server(&s,path);

 CHECK(backend_drm_init(&s));
 CHECK(W.set_master_calls==1);
 CHECK(W.master_active);
 CHECK(W.drop_master_calls==0);
 CHECK(W.gbm_create_calls==1);
 CHECK(W.surface_create_calls==1);
 CHECK(W.egl_initialize_calls==1);
 CHECK(W.egl_make_current_calls==1);
 CHECK(W.egl_make_current_none_calls==0);
 CHECK(W.egl_swap_interval_calls==1);
 CHECK(W.gl_get_string_calls==2);
 CHECK(s.width==800);
 CHECK(s.height==600);
 CHECK(s.mouse_x==400);
 CHECK(s.mouse_y==300);
 CHECK(s.target_fbo==0);
 CHECK(s.readback_pixels==NULL);
 CHECK(s.egl_display==fake_display);
 CHECK(s.egl_context==fake_context);
 CHECK(W.surface&&W.surface->width==800&&W.surface->height==600);
 CHECK(W.surface&&W.surface->format==GBM_FORMAT_XRGB8888);
 CHECK(W.surface&&(W.surface->flags&GBM_BO_USE_SCANOUT));
 CHECK(W.surface&&(W.surface->flags&GBM_BO_USE_RENDERING));

 backend_drm_present(&s);
 CHECK(W.egl_swap_calls==1);
 CHECK(W.add_fb_calls==1);
 CHECK(W.add_fb2_calls==1);
 CHECK(W.add_fb2_modifier_calls==0);
 CHECK(!W.last_fb_with_modifiers);
 CHECK(W.set_crtc_calls==1);
 CHECK(W.page_flip_calls==0);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.crtc_calls[0].crtc_id==3);
 CHECK(W.crtc_calls[0].buffer_id==100);
 CHECK(W.crtc_calls[0].connector_id==7);
 CHECK(W.crtc_calls[0].connector_count==1);
 CHECK(W.crtc_calls[0].has_mode);
 CHECK(W.crtc_calls[0].mode.hdisplay==800);
 CHECK(W.crtc_calls[0].mode.vdisplay==600);

 for(int i=0;i<4;i++){
  backend_drm_present(&s);
  CHECK(W.fb_count==1);
  CHECK(active_locked_bos()==1);
  CHECK(W.page_flip_calls==i+1);
  CHECK(W.rm_fb_calls==i+1);
  CHECK(W.fb_count<=2);
 }
 CHECK(W.add_fb_calls==5);
 CHECK(W.max_fb_count==2);
 CHECK(W.max_locked_bos==2);
 CHECK(W.add_fb_over_bound==0);

 backend_drm_shutdown(&s);
 CHECK(W.set_crtc_calls==2);
 CHECK(W.crtc_calls[1].crtc_id==3);
 CHECK(W.crtc_calls[1].buffer_id==42);
 CHECK(W.crtc_calls[1].x==5);
 CHECK(W.crtc_calls[1].y==7);
 CHECK(W.crtc_calls[1].connector_id==7);
 CHECK(W.crtc_calls[1].connector_count==1);
 CHECK(W.crtc_calls[1].has_mode);
 CHECK(W.crtc_calls[1].mode.hdisplay==800);
 CHECK(W.crtc_calls[1].mode.vdisplay==600);
 CHECK(W.egl_make_current_none_calls==1);
 CHECK(W.egl_surface_destroy_calls==1);
 CHECK(W.egl_context_destroy_calls==1);
 CHECK(W.egl_terminate_calls==1);
 CHECK(W.surface_destroy_calls==1);
 CHECK(W.gbm_destroy_calls==1);
 CHECK(W.evdev_init_calls==0);
 CHECK(W.evdev_shutdown_calls==0);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_modifier_fb_registration(void) {
 reset_world();
 W.modifier_cap=1;
 W.bo_modifier=(1ull<<56)|0x42u; /* opaque non-linear vendor modifier */
 W.bo_offset=64;
 char path[64];make_temp_card(path,sizeof(path));
 Server s;CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 CHECK(W.add_fb_calls==1);
 CHECK(W.add_fb2_calls==1);
 CHECK(W.add_fb2_modifier_calls==1);
 CHECK(W.last_fb_with_modifiers);
 CHECK(W.last_fb_modifier==W.bo_modifier);
 CHECK(W.last_fb_offset==64);
 CHECK(W.fb_count==1);
 CHECK(W.set_crtc_calls==1);
 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_linear_modifier_registration_fallback(void) {
 reset_world();
 W.modifier_cap=1;
 W.fail_add_fb2_modifiers=true;
 char path[64];make_temp_card(path,sizeof(path));
 Server s;CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 CHECK(W.add_fb2_calls==2); /* rejected modifier form, then safe linear AddFB2 */
 CHECK(W.add_fb2_modifier_calls==1);
 CHECK(W.add_fb_calls==1);
 CHECK(!W.last_fb_with_modifiers);
 CHECK(W.fb_count==1);
 CHECK(W.set_crtc_calls==1);
 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_nonlinear_without_modifier_cap_is_rejected(void) {
 reset_world();
 W.bo_modifier=(1ull<<56)|0x42u;
 char path[64];make_temp_card(path,sizeof(path));
 Server s;CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 /* A legacy linear registration would be an unsafe fallback here. */
 CHECK(W.add_fb_calls==0);
 CHECK(W.add_fb2_calls==0);
 CHECK(W.add_fb2_modifier_calls==0);
 CHECK(W.fb_count==0);
 CHECK(W.set_crtc_calls==0);
 CHECK(W.page_flip_calls==0);
 CHECK(active_locked_bos()==0);
 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_cloned_connector_restore(void) {
 reset_world();
 W.have_second_connector=true;
 char path[64];make_temp_card(path,sizeof(path));
 Server s;CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 CHECK(W.set_crtc_calls==1);
 CHECK(W.crtc_calls[0].connector_count==1);
 CHECK(W.crtc_calls[0].connector_ids[0]==7);
 backend_drm_shutdown(&s);
 CHECK(W.set_crtc_calls==2);
 CHECK(W.crtc_calls[1].connector_count==2);
 CHECK(W.crtc_calls[1].connector_ids[0]==7);
 CHECK(W.crtc_calls[1].connector_ids[1]==8);
 CHECK(W.crtc_calls[1].buffer_id==42);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_cloned_connector_restore_fallback(void) {
 reset_world();
 W.have_second_connector=true;
 W.fail_cloned_restore=true;
 char path[64];make_temp_card(path,sizeof(path));
 Server s;CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 backend_drm_shutdown(&s);
 CHECK(W.set_crtc_calls==3);
 CHECK(W.crtc_calls[1].connector_count==2);
 CHECK(!W.crtc_calls[1].ok);
 CHECK(W.crtc_calls[2].connector_count==1);
 CHECK(W.crtc_calls[2].ok);
 CHECK(W.crtc_calls[2].buffer_id==42);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_pending_flip_timeout_recovery(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 CHECK(init_ok(&s,path));

 backend_drm_present(&s);
 CHECK(W.fb_count==1);
 CHECK(W.set_crtc_calls==1);

 W.auto_complete_flip=false;
 backend_drm_present(&s);
 CHECK(W.page_flip_calls==1);
 CHECK(W.fb_count==2);
 CHECK(active_locked_bos()==2);
 CHECK(W.rm_fb_calls==0);
 CHECK(W.poll_timeouts>=1);
 CHECK(W.event_count==0);

 int timeouts_before=W.poll_timeouts;
 int swaps_before=W.egl_swap_calls;
 int adds_before=W.add_fb_calls;
 backend_drm_present(&s);
 CHECK(W.poll_timeouts>timeouts_before);
 CHECK(W.egl_swap_calls==swaps_before);
 CHECK(W.add_fb_calls==adds_before);
 CHECK(W.page_flip_calls==1);
 CHECK(W.fb_count==2);
 CHECK(active_locked_bos()==2);

 enqueue_flip_event(W.last_flip_user_data);
 CHECK(W.event_count==1);
 backend_drm_poll(&s);
 CHECK(W.event_count==0);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.rm_fb_calls==1);
 CHECK(W.poll_errors==0);

 W.auto_complete_flip=true;
 backend_drm_present(&s);
 CHECK(W.page_flip_calls==2);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.rm_fb_calls==2);

 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_shutdown_with_pending_flip(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 W.auto_complete_flip=false;
 backend_drm_present(&s);
 CHECK(W.fb_count==2);
 CHECK(active_locked_bos()==2);
 backend_drm_shutdown(&s);
 CHECK(W.set_crtc_calls==2);
 CHECK(W.rm_fb_calls==2);
 CHECK(W.surface_destroy_calls==1);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_addfb_failure(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 CHECK(W.add_fb_calls==1);

 W.fail_add_fb=true;
 backend_drm_present(&s);
 /* Both AddFB2 and the permitted linear legacy fallback were attempted. */
 CHECK(W.add_fb_calls==3);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.rm_fb_calls==0);
 CHECK(W.page_flip_calls==0);

 W.fail_add_fb=false;
 backend_drm_present(&s);
 CHECK(W.add_fb_calls==4);
 CHECK(W.page_flip_calls==1);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.rm_fb_calls==1);

 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_setcrtc_failure(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 CHECK(init_ok(&s,path));

 W.fail_set_crtc=true;
 backend_drm_present(&s);
 CHECK(W.set_crtc_calls==1);
 CHECK(W.add_fb_calls==1);
 CHECK(W.fb_count==0);
 CHECK(active_locked_bos()==0);
 CHECK(W.rm_fb_calls==1);
 CHECK(W.page_flip_calls==0);

 W.fail_set_crtc=false;
 backend_drm_present(&s);
 CHECK(W.set_crtc_calls==2);
 CHECK(W.add_fb_calls==2);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.page_flip_calls==0);

 backend_drm_shutdown(&s);
 CHECK(W.set_crtc_calls==3);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_pageflip_failure(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 CHECK(W.fb_count==1);

 W.fail_page_flip=true;
 backend_drm_present(&s);
 CHECK(W.page_flip_calls==1);
 CHECK(W.add_fb_calls==2);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.rm_fb_calls==1);

 W.fail_page_flip=false;
 backend_drm_present(&s);
 CHECK(W.page_flip_calls==2);
 CHECK(W.add_fb_calls==3);
 CHECK(W.fb_count==1);
 CHECK(active_locked_bos()==1);
 CHECK(W.rm_fb_calls==2);

 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_restore_without_saved_crtc(void) {
 reset_world();
 W.have_crtc=false;
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 CHECK(init_ok(&s,path));
 backend_drm_present(&s);
 CHECK(W.set_crtc_calls==1);
 CHECK(W.crtc_calls[0].has_mode);
 CHECK(W.crtc_calls[0].mode.hdisplay==800);
 backend_drm_shutdown(&s);
 CHECK(W.set_crtc_calls==2);
 CHECK(W.crtc_calls[1].buffer_id==0);
 CHECK(W.crtc_calls[1].connector_count==0);
 CHECK(W.crtc_calls[1].connector_id==0);
 CHECK(!W.crtc_calls[1].has_mode);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_requested_mode(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 init_server(&s,path);
 s.drm_mode_width=1920;
 s.drm_mode_height=1080;
 CHECK(backend_drm_init(&s));
 CHECK(s.width==1920);
 CHECK(s.height==1080);
 CHECK(W.surface&&W.surface->width==1920&&W.surface->height==1080);
 backend_drm_present(&s);
 CHECK(W.crtc_calls[0].mode.hdisplay==1920);
 CHECK(W.crtc_calls[0].mode.vdisplay==1080);
 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_evdev_lifecycle(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 init_server(&s,path);
 s.fbdev_evdev=true;
 CHECK(backend_drm_init(&s));
 CHECK(W.evdev_init_calls==1);
 CHECK(s.evdev_state==&W.evdev);
 backend_drm_poll(&s);
 CHECK(W.evdev_poll_calls==1);
 backend_drm_shutdown(&s);
 CHECK(W.evdev_shutdown_calls==1);
 CHECK(s.evdev_state==NULL);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_evdev_pre_shutdown_is_not_repeated(void) {
 reset_world();
 char path[64];make_temp_card(path,sizeof(path));
 Server s;init_server(&s,path);s.fbdev_evdev=true;
 CHECK(backend_drm_init(&s));
 /* main uses this order on normal and partial-Wayland-init exits. */
 backend_evdev_shutdown(&s);
 CHECK(s.evdev_state==NULL);
 CHECK(W.evdev_shutdown_calls==1);
 backend_drm_shutdown(&s);
 CHECK(W.evdev_shutdown_calls==1);
 check_all_clean(&s,true);
 unlink(path);
}

static void test_poll_error(void) {
 reset_world();
 char path[64];
 make_temp_card(path,sizeof(path));
 Server s;
 CHECK(init_ok(&s,path));
 W.poll_fail=true;
 backend_drm_poll(&s);
 CHECK(W.poll_errors>=1);
 W.poll_fail=false;
 W.poll_bad_revents=true;
 backend_drm_poll(&s);
 CHECK(W.poll_errors>=2);
 W.poll_bad_revents=false;
 backend_drm_poll(&s);
 CHECK(W.event_count==0);
 backend_drm_poll(&s);
 backend_drm_shutdown(&s);
 check_all_clean(&s,true);
 unlink(path);
}

typedef struct {
 const char *name;
 void (*setup)(Server *s,const char *path);
 bool master_expected;
} InitFailure;

static void setup_open_failure(Server *s,const char *path) {
 init_server(s,path);
}

static void setup_set_master_failure(Server *s,const char *path) {
 init_server(s,path);
 W.fail_set_master=true;
}

static void setup_resources_failure(Server *s,const char *path) {
 init_server(s,path);
 W.fail_get_resources=true;
}

static void setup_connector_failure(Server *s,const char *path) {
 init_server(s,path);
 W.fail_get_connector=true;
}

static void setup_disconnected(Server *s,const char *path) {
 init_server(s,path);
 W.connector.connection=DRM_MODE_DISCONNECTED;
}

static void setup_no_modes(Server *s,const char *path) {
 init_server(s,path);
 W.connector.count_modes=0;
}

static void setup_no_crtc(Server *s,const char *path) {
 init_server(s,path);
 W.connector.encoder_id=0;
 W.encoder.crtc_id=0;
 W.encoder.possible_crtcs=0;
}

static void setup_requested_mode_missing(Server *s,const char *path) {
 init_server(s,path);
 s->drm_mode_width=640;
 s->drm_mode_height=480;
}

static void setup_gbm_failure(Server *s,const char *path) {
 init_server(s,path);
 W.fail_gbm_create=true;
}

static void setup_no_platform(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_NO_PROC;
}

static void setup_null_platform(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_PLATFORM_NULL;
}

static void setup_egl_init(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_INIT;
}

static void setup_egl_bind(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_BIND;
}

static void setup_egl_choose(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_CHOOSE;
}

static void setup_egl_visual(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_VISUAL;
}

static void setup_egl_context(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_CONTEXT;
}

static void setup_gbm_surface(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_GBM_SURFACE;
}

static void setup_egl_window_surface(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_WINDOW_SURFACE;
}

static void setup_egl_make_current(Server *s,const char *path) {
 init_server(s,path);
 W.egl_fail=EG_MAKE_CURRENT;
}

static void setup_evdev_failure(Server *s,const char *path) {
 init_server(s,path);
 s->fbdev_evdev=true;
 W.evdev_init_result=false;
 W.evdev_alloc_state=false;
}

static void test_init_failures(void) {
 InitFailure cases[]={
  {"open",setup_open_failure,false},
  {"set-master",setup_set_master_failure,false},
  {"resources",setup_resources_failure,true},
  {"connector",setup_connector_failure,true},
  {"disconnected",setup_disconnected,true},
  {"no-modes",setup_no_modes,true},
  {"no-crtc",setup_no_crtc,true},
  {"requested-mode-missing",setup_requested_mode_missing,true},
  {"gbm-create",setup_gbm_failure,true},
  {"no-platform-proc",setup_no_platform,true},
  {"null-platform",setup_null_platform,true},
  {"egl-init",setup_egl_init,true},
  {"egl-bind",setup_egl_bind,true},
  {"egl-choose",setup_egl_choose,true},
  {"egl-visual",setup_egl_visual,true},
  {"egl-context",setup_egl_context,true},
  {"gbm-surface",setup_gbm_surface,true},
  {"egl-window-surface",setup_egl_window_surface,true},
  {"egl-make-current",setup_egl_make_current,true},
  {"evdev-init",setup_evdev_failure,true},
 };
 for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  reset_world();
  char path[64];
  if(strcmp(cases[i].name,"open")==0)
   snprintf(path,sizeof(path),"/tmp/cd-drm-missing-%ld",(long)getpid());
  else
   make_temp_card(path,sizeof(path));
  Server s;
  cases[i].setup(&s,path);
  bool ok=backend_drm_init(&s);
  if(ok)fprintf(stderr,"expected init failure for %s\n",cases[i].name);
  CHECK(!ok);
  CHECK(W.violations==0);
  CHECK(W.fb_count==0);
  CHECK(active_locked_bos()==0);
  CHECK(s.drm_state==NULL);
  CHECK(s.egl_display==EGL_NO_DISPLAY);
  CHECK(s.egl_context==EGL_NO_CONTEXT);
  CHECK(s.target_fbo==0);
  CHECK(!W.master_active);
  CHECK(W.drop_master_calls==(cases[i].master_expected?1:0));
  CHECK(fd_is_closed(W.master_fd));
  CHECK(W.gbm_create_success==W.gbm_destroy_calls);
  CHECK(W.surface_create_success==W.surface_destroy_calls);
  CHECK(W.egl_context_create_success==W.egl_context_destroy_calls);
  CHECK(W.egl_surface_create_success==W.egl_surface_destroy_calls);
  if(strcmp(cases[i].name,"evdev-init")==0){
   CHECK(W.evdev_init_calls==1);
   CHECK(W.evdev_shutdown_calls==0);
   CHECK(s.evdev_state==NULL);
  }
  if(strcmp(cases[i].name,"open")!=0)unlink(path);
 }
}

int main(void) {
 test_success_lifecycle();
 test_modifier_fb_registration();
 test_linear_modifier_registration_fallback();
 test_nonlinear_without_modifier_cap_is_rejected();
 test_cloned_connector_restore();
 test_cloned_connector_restore_fallback();
 test_pending_flip_timeout_recovery();
 test_shutdown_with_pending_flip();
 test_addfb_failure();
 test_setcrtc_failure();
 test_pageflip_failure();
 test_restore_without_saved_crtc();
 test_requested_mode();
 test_evdev_lifecycle();
 test_evdev_pre_shutdown_is_not_repeated();
 test_poll_error();
 test_init_failures();
 if(failures){
  fprintf(stderr,"test-drm: %d failure(s)\n",failures);
  return 1;
 }
 printf("test-drm: all KMS lifecycle checks passed\n");
 return 0;
}
