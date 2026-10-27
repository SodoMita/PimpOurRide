/* zwp_linux_dmabuf_v1 (version 3): real zero-copy GPU buffer import via EGLImage.
 *
 * eglCreateImageKHR(..., EGL_LINUX_DMA_BUF_EXT, ...) + glEGLImageTargetTexture2DOES
 * import a client's dma-buf fd directly into a GL texture with no pixel copy —
 * this is the same mechanism wlroots/Mutter/KWin use for GPU-accelerated clients
 * (most browsers, Electron, mpv --gpu, etc).
 *
 * This sandbox's software EGL driver (llvmpipe) genuinely advertises
 * EGL_EXT_image_dma_buf_import (verified at runtime below), so this file is a
 * real, working import path — but the sandbox exposes no /dev/dri and no
 * /dev/udmabuf, so nothing in this environment can *produce* a dma-buf fd to
 * feed it. The negotiation (format/modifier advertisement) and failure paths
 * are fully exercised by tests/protocol_probe.c; the success path will run on
 * any real machine with a GPU or a /dev/udmabuf-capable kernel.
 */
#include "chrome.h"
#include "linux-dmabuf-server.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CD_DRM_FORMAT_ARGB8888 0x34325241u /* 'AR24' */
#define CD_DRM_FORMAT_XRGB8888 0x34325258u /* 'XR24' */
#define CD_DRM_FORMAT_MOD_LINEAR 0ull
#define CD_DRM_FORMAT_MOD_INVALID 0xffffffffffffffffull

typedef struct {bool set;int fd,offset,stride;uint64_t modifier;} DmabufPlane;
typedef struct {struct wl_resource *resource;Server *server;DmabufPlane planes[4];int plane_count;bool used;} Params;

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_texture;
static PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers;
static const struct wl_buffer_interface dmabuf_buffer_impl;

static void destroy_request(struct wl_client *c,struct wl_resource *r){wl_resource_destroy(r);}

void dmabuf_probe(Server *s){
 s->dmabuf_supported=false;s->dmabuf_format_count=0;
 if(s->egl_display==EGL_NO_DISPLAY){fprintf(stderr,"[dmabuf] SDL/GLX windowed backend has no EGL context — dma-buf import is only wired up for the EGL backends (headless/fbdev)\n");return;}
 const char *egl_ext=eglQueryString(s->egl_display,EGL_EXTENSIONS);
 if(!egl_ext||!strstr(egl_ext,"EGL_EXT_image_dma_buf_import")){fprintf(stderr,"[dmabuf] EGL_EXT_image_dma_buf_import not available on this driver\n");return;}
 create_image=(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
 destroy_image=(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
 target_texture=(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
 query_formats=(PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
 query_modifiers=(PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
 if(!create_image||!destroy_image||!target_texture){fprintf(stderr,"[dmabuf] required EGL/GL entry points missing\n");return;}
 const char *gl_ext=(const char*)glGetString(GL_EXTENSIONS);
 if(!gl_ext||!strstr(gl_ext,"GL_OES_EGL_image")){fprintf(stderr,"[dmabuf] GL_OES_EGL_image not available\n");return;}
 uint32_t wanted[2]={CD_DRM_FORMAT_ARGB8888,CD_DRM_FORMAT_XRGB8888};
 EGLint all[64];EGLint count=0;if(query_formats)query_formats(s->egl_display,64,all,&count);
 for(int w=0;w<2;w++){
  bool present=!query_formats;for(int i=0;i<count&&!present;i++)if((uint32_t)all[i]==wanted[w])present=true;
  if(!present)continue;
  DmabufFormat *f=&s->dmabuf_formats[s->dmabuf_format_count];f->format=wanted[w];f->modifier_count=0;
  if(query_modifiers){
   EGLuint64KHR mods[CD_DMABUF_MAX_MODIFIERS];EGLBoolean external[CD_DMABUF_MAX_MODIFIERS];EGLint mcount=0;
   if(query_modifiers(s->egl_display,wanted[w],CD_DMABUF_MAX_MODIFIERS,mods,external,&mcount))
    for(int m=0;m<mcount&&m<CD_DMABUF_MAX_MODIFIERS;m++)if(!external[m])f->modifiers[f->modifier_count++]=mods[m];
  }
  if(f->modifier_count==0)f->modifiers[f->modifier_count++]=CD_DRM_FORMAT_MOD_LINEAR;
  s->dmabuf_format_count++;
 }
 s->dmabuf_supported=s->dmabuf_format_count>0;
 if(s->dmabuf_supported){
  fprintf(stderr,"[dmabuf] EGL_EXT_image_dma_buf_import active — %d importable format(s) (ARGB8888/XRGB8888, linear)\n",s->dmabuf_format_count);
  fprintf(stderr,"[dmabuf] note: this sandbox has no /dev/dri or /dev/udmabuf, so no local client can produce a dma-buf fd to feed it — the import code is real and will activate on hardware with a GPU/DRM render node.\n");
 }
}
static int validate_params(Params *p,int32_t w,int32_t h,uint32_t format){
 if(p->plane_count<1)return ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE;
 if(p->plane_count>1)return ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT; /* multi-planar formats not implemented yet */
 if(format!=CD_DRM_FORMAT_ARGB8888&&format!=CD_DRM_FORMAT_XRGB8888)return ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT;
 if(w<1||h<1||w>8192||h>8192||(size_t)w*h>16777216)return ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS;
 return -1;
}
static DmabufBuffer *import_dmabuf(Server *s,Params *p,int32_t width,int32_t height,uint32_t format){
 DmabufPlane *pl=&p->planes[0];EGLint attribs[16];int n=0;
 attribs[n++]=EGL_WIDTH;attribs[n++]=width;attribs[n++]=EGL_HEIGHT;attribs[n++]=height;attribs[n++]=EGL_LINUX_DRM_FOURCC_EXT;attribs[n++]=(EGLint)format;
 attribs[n++]=EGL_DMA_BUF_PLANE0_FD_EXT;attribs[n++]=pl->fd;attribs[n++]=EGL_DMA_BUF_PLANE0_OFFSET_EXT;attribs[n++]=pl->offset;attribs[n++]=EGL_DMA_BUF_PLANE0_PITCH_EXT;attribs[n++]=pl->stride;
 if(pl->modifier!=CD_DRM_FORMAT_MOD_LINEAR&&pl->modifier!=CD_DRM_FORMAT_MOD_INVALID){attribs[n++]=EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;attribs[n++]=(EGLint)(pl->modifier&0xffffffffu);attribs[n++]=EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;attribs[n++]=(EGLint)(pl->modifier>>32);}
 attribs[n++]=EGL_NONE;
 EGLImageKHR image=create_image(s->egl_display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,(EGLClientBuffer)NULL,attribs);
 if(image==EGL_NO_IMAGE_KHR){fprintf(stderr,"[dmabuf] eglCreateImageKHR failed: 0x%x (expected in this sandbox — no real dma-buf backing is possible here)\n",eglGetError());close(pl->fd);pl->fd=-1;return NULL;}
 GLuint tex;glGenTextures(1,&tex);client_texture_params(tex,s->client_filter);
 target_texture(GL_TEXTURE_2D,image);
 close(pl->fd);pl->fd=-1;
 DmabufBuffer *db=calloc(1,sizeof(*db));if(!db){destroy_image(s->egl_display,image);glDeleteTextures(1,&tex);return NULL;}
 db->server=s;db->texture=tex;db->image=image;db->width=width;db->height=height;db->has_alpha=(format==CD_DRM_FORMAT_ARGB8888);return db;
}
static void dmabuf_buffer_destroyed(struct wl_resource *r){
 DmabufBuffer *db=wl_resource_get_user_data(r);if(!db)return;Server *s=db->server;Surface *p;
 wl_list_for_each(p,&s->surfaces,link){if(p->dmabuf==db){p->dmabuf=NULL;p->texture=0;}if(p->pending_dmabuf==db)p->pending_dmabuf=NULL;}
 if(db->image!=EGL_NO_IMAGE_KHR)destroy_image(s->egl_display,db->image);if(db->texture)glDeleteTextures(1,&db->texture);free(db);
}
static const struct wl_buffer_interface dmabuf_buffer_impl={.destroy=destroy_request};
bool dmabuf_is_buffer(struct wl_resource *r){return r&&wl_resource_instance_of(r,&wl_buffer_interface,&dmabuf_buffer_impl);}
DmabufBuffer *dmabuf_from_resource(struct wl_resource *r){return dmabuf_is_buffer(r)?wl_resource_get_user_data(r):NULL;}
static void params_destroyed(struct wl_resource *r){Params *p=wl_resource_get_user_data(r);if(!p)return;for(int i=0;i<4;i++)if(p->planes[i].set&&p->planes[i].fd>=0)close(p->planes[i].fd);free(p);}
static void params_add(struct wl_client *c,struct wl_resource *r,int32_t fd,uint32_t plane_idx,uint32_t offset,uint32_t stride,uint32_t mod_hi,uint32_t mod_lo){
 Params *p=wl_resource_get_user_data(r);
 if(p->used){wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,"params already used to create a buffer");close(fd);return;}
 if(plane_idx>=4){wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,"plane index out of range");close(fd);return;}
 if(p->planes[plane_idx].set){wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,"plane already set");close(fd);return;}
 p->planes[plane_idx]=(DmabufPlane){.set=true,.fd=fd,.offset=(int)offset,.stride=(int)stride,.modifier=((uint64_t)mod_hi<<32)|mod_lo};
 if((int)plane_idx+1>p->plane_count)p->plane_count=(int)plane_idx+1;
}
static void params_create(struct wl_client *c,struct wl_resource *r,int32_t width,int32_t height,uint32_t format,uint32_t flags){
 Params *p=wl_resource_get_user_data(r);int err=validate_params(p,width,height,format);
 if(err>=0){wl_resource_post_error(r,err,"invalid dma-buf parameters");return;}
 p->used=true;DmabufBuffer *db=import_dmabuf(p->server,p,width,height,format);
 if(!db){zwp_linux_buffer_params_v1_send_failed(r);return;}
 db->resource=wl_resource_create(c,&wl_buffer_interface,1,0);
 if(!db->resource){destroy_image(p->server->egl_display,db->image);glDeleteTextures(1,&db->texture);free(db);wl_client_post_no_memory(c);return;}
 wl_resource_set_implementation(db->resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroyed);
 zwp_linux_buffer_params_v1_send_created(r,db->resource);
}
static void params_create_immed(struct wl_client *c,struct wl_resource *r,uint32_t id,int32_t width,int32_t height,uint32_t format,uint32_t flags){
 Params *p=wl_resource_get_user_data(r);int err=validate_params(p,width,height,format);
 if(err>=0){wl_resource_post_error(r,err,"invalid dma-buf parameters");return;}
 p->used=true;DmabufBuffer *db=import_dmabuf(p->server,p,width,height,format);
 if(!db){wl_resource_post_error(r,ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,"dma-buf import failed");return;}
 db->resource=wl_resource_create(c,&wl_buffer_interface,1,id);
 if(!db->resource){destroy_image(p->server->egl_display,db->image);glDeleteTextures(1,&db->texture);free(db);wl_client_post_no_memory(c);return;}
 wl_resource_set_implementation(db->resource,&dmabuf_buffer_impl,db,dmabuf_buffer_destroyed);
}
static const struct zwp_linux_buffer_params_v1_interface params_impl={.destroy=destroy_request,.add=params_add,.create=params_create,.create_immed=params_create_immed};
static void dmabuf_create_params(struct wl_client *c,struct wl_resource *r,uint32_t id){
 Server *s=wl_resource_get_user_data(r);Params *p=calloc(1,sizeof(*p));if(!p){wl_client_post_no_memory(c);return;}
 for(int i=0;i<4;i++)p->planes[i].fd=-1;p->server=s;
 p->resource=wl_resource_create(c,&zwp_linux_buffer_params_v1_interface,wl_resource_get_version(r),id);
 if(!p->resource){free(p);wl_client_post_no_memory(c);return;}
 wl_resource_set_implementation(p->resource,&params_impl,p,params_destroyed);
}
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl={.destroy=destroy_request,.create_params=dmabuf_create_params};
static void bind_dmabuf(struct wl_client *c,void *data,uint32_t v,uint32_t id){
 Server *s=data;uint32_t version=v<3?v:3;struct wl_resource *r=wl_resource_create(c,&zwp_linux_dmabuf_v1_interface,version,id);
 if(!r){wl_client_post_no_memory(c);return;}
 wl_resource_set_implementation(r,&dmabuf_impl,s,NULL);
 for(int i=0;i<s->dmabuf_format_count;i++){
  DmabufFormat *f=&s->dmabuf_formats[i];zwp_linux_dmabuf_v1_send_format(r,f->format);
  if(version>=3)for(int m=0;m<f->modifier_count;m++)zwp_linux_dmabuf_v1_send_modifier(r,f->format,(uint32_t)(f->modifiers[m]>>32),(uint32_t)f->modifiers[m]);
 }
}
void dmabuf_register_global(Server *s){if(s->dmabuf_supported)wl_global_create(s->display,&zwp_linux_dmabuf_v1_interface,3,s,bind_dmabuf);}
