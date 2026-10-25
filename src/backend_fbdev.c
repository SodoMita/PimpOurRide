/* Direct Linux framebuffer output (/dev/fb0 and friends).
 *
 * Rendering is the same surfaceless-EGL/FBO path as the headless backend
 * (backend_egl.c): scene_render() draws into s->target_fbo, then this backend
 * performs exactly one glReadPixels and converts that RGBA image into the
 * framebuffer's packed truecolor format. That is CPU readback + blit into the
 * kernel's current fb mode, not DRM/KMS GPU scanout.
 *
 * Phase 1 adopts the visible mode already programmed on the device (no
 * FBIOPUT_VSCREENINFO modesetting). Phase 2A adds opt-in modesetting via
 * --fbdev-modeset or --fbdev-mode: the requested mode is programmed with
 * FBIOPUT_VSCREENINFO, the original mode is restored on shutdown or init
 * failure. The driver may adjust timings or reject the request. */
#include "chrome.h"
#include "fbdev_pixfmt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct {
 int fd;
 uint8_t *map;
 size_t map_len;
 struct fb_var_screeninfo var,orig_var;
 struct fb_fix_screeninfo fix;
 CdFbFormat fmt;
 CdFbGeometry geo;
 size_t direct_base;
 int display_page;
 bool pan_enabled,vsync_enabled,warned_pan,warned_vsync,panned;
 bool mode_changed;
 uint32_t cur_xoffset,cur_yoffset;
 char path[256];
} FbdevState;

static FbdevState *fbdev_state(Server *s){return s?(FbdevState*)s->fbdev_state:NULL;}

static size_t fbdev_visible_base(const FbdevState *st){
 return cd_fb_visible_offset(st->fix.line_length,(int)st->cur_xoffset,(int)st->cur_yoffset,st->fmt.bytes_per_pixel);
}
static size_t fbdev_page_base(const FbdevState *st,int page){
 return cd_fb_page_offset(st->fix.line_length,(int)st->cur_xoffset,page,(int)st->var.yres,st->fmt.bytes_per_pixel);
}
static bool fbdev_region_fits(const FbdevState *st,size_t base){
 if(base>st->map_len)return false;
 size_t row_bytes=(size_t)st->geo.width*(size_t)st->fmt.bytes_per_pixel;
 size_t last=base+(size_t)(st->geo.height-1)*(size_t)st->fix.line_length+row_bytes;
 return last<=st->map_len;
}
static void fbdev_copy_visible_rows(FbdevState *st,size_t dst,size_t src){
 size_t row_bytes=(size_t)st->geo.width*(size_t)st->fmt.bytes_per_pixel;
 for(int y=0;y<st->geo.height;y++)
  memcpy(st->map+dst+(size_t)y*st->fix.line_length,st->map+src+(size_t)y*st->fix.line_length,row_bytes);
}
static void fbdev_wait_vsync(FbdevState *st){
 if(!st||!st->vsync_enabled||st->fd<0)return;
 int arg=0;
 if(ioctl(st->fd,FBIO_WAITFORVSYNC,&arg)<0){
  if(!st->warned_vsync)fprintf(stderr,"[fbdev] FBIO_WAITFORVSYNC unsupported on %s (%s); continuing without vsync wait\n",st->path,strerror(errno));
  st->warned_vsync=true;
  st->vsync_enabled=false;
 }
}

/* Restore the original video mode via FBIOPUT_VSCREENINFO. Must be called
 * before munmap/close, and only if mode_changed is true. */
static void fbdev_restore_mode(FbdevState *st){
 if(!st||!st->mode_changed||st->fd<0)return;
 if(ioctl(st->fd,FBIOPUT_VSCREENINFO,&st->orig_var)<0)
  fprintf(stderr,"[fbdev] failed to restore original mode on %s: %s\n",st->path,strerror(errno));
 else
  fprintf(stderr,"[fbdev] restored original mode on %s\n",st->path);
 st->mode_changed=false;
}

static void fbdev_close(Server *s){
 FbdevState *st=fbdev_state(s);
 if(!st)return;
 /* If we changed the mode via FBIOPUT_VSCREENINFO, we must unmap FIRST
  * (the framebuffer memory layout may change across the mode switch),
  * then restore the original mode, then close the fd.
  * The pan-offset restore path (mode not changed) stays as-is. */
 if(st->mode_changed){
  if(st->map&&st->map!=MAP_FAILED){
   munmap(st->map,st->map_len);
   st->map=NULL;
  }
  fbdev_restore_mode(st);
 }else if(st->fd>=0&&st->panned){
  struct fb_var_screeninfo var;
  if(ioctl(st->fd,FBIOGET_VSCREENINFO,&var)==0){
   var.xoffset=st->orig_var.xoffset;var.yoffset=st->orig_var.yoffset;
   if(ioctl(st->fd,FBIOPAN_DISPLAY,&var)<0)
    fprintf(stderr,"[fbdev] could not restore original pan offset on %s: %s\n",st->path,strerror(errno));
  }else fprintf(stderr,"[fbdev] could not re-read %s to restore its pan offset: %s\n",st->path,strerror(errno));
 }
 if(st->map&&st->map!=MAP_FAILED)munmap(st->map,st->map_len);
 if(st->fd>=0)close(st->fd);
 free(st);
 s->fbdev_state=NULL;
}

/* Install conventional truecolor bitfields for the given bpp. */
static void fbdev_install_truecolor_bitfields(struct fb_var_screeninfo *v,int bpp){
 v->bits_per_pixel=bpp;
 if(bpp==16){
  v->red.offset=11;v->red.length=5;
  v->green.offset=5;v->green.length=6;
  v->blue.offset=0;v->blue.length=5;
  v->transp.offset=0;v->transp.length=0;
 }else{
  /* 24 and 32 bpp: 8/8/8 */
  v->red.offset=16;v->red.length=8;
  v->green.offset=8;v->green.length=8;
  v->blue.offset=0;v->blue.length=8;
  v->transp.offset=(bpp==32)?24:0;v->transp.length=(bpp==32)?8:0;
 }
 v->red.msb_right=0;v->green.msb_right=0;v->blue.msb_right=0;v->transp.msb_right=0;
}

/* Attempt to set a new video mode via FBIOPUT_VSCREENINFO. Returns true on
 * success, false on failure (original mode is NOT restored here - caller must
 * call fbdev_restore_mode). On success, the actual mode is re-read into
 * st->var and st->fix, and st->mode_changed is set to true. */
static bool fbdev_do_modeset(Server *s,FbdevState *st){
 struct fb_var_screeninfo req=st->var;
 bool have_req_width=s->fbdev_mode_width>0;
 bool have_req_height=s->fbdev_mode_height>0;
 bool have_req_bpp=s->fbdev_mode_bpp>0;
 int target_w=have_req_width?s->fbdev_mode_width:s->width;
 int target_h=have_req_height?s->fbdev_mode_height:s->height;
 int target_bpp=have_req_bpp?s->fbdev_mode_bpp:(int)st->var.bits_per_pixel;
 bool need_modeset=(target_w!=(int)st->var.xres||target_h!=(int)st->var.yres||target_bpp!=(int)st->var.bits_per_pixel);
 bool need_more_virtual=s->fbdev_pan&&(st->var.yres_virtual<(uint32_t)target_h*2u);

 if(!need_modeset&&!need_more_virtual)return true;

 req.xres=target_w;req.yres=target_h;
 req.xoffset=0;req.yoffset=0;
 req.xres_virtual=target_w;
 req.yres_virtual=s->fbdev_pan?target_h*2:target_h;
 if(have_req_bpp){
  fbdev_install_truecolor_bitfields(&req,target_bpp);
 }else{
  /* Keep current bitfields but update bpp if it didn't change */
  req.bits_per_pixel=target_bpp;
 }
 req.activate=FB_ACTIVATE_NOW;
 /* Keep existing timing fields (pixclock, left/right/upper/lower_margin,
  * hsync_len, vsync_len, sync, vmode) - caveat: driver may adjust or
  * reject based on these timings. */

 fprintf(stderr,"[fbdev] requesting modeset on %s: %dx%d@%dbpp virtual=%dx%d pan=%s\n",
  st->path,req.xres,req.yres,req.bits_per_pixel,req.xres_virtual,req.yres_virtual,s->fbdev_pan?"on":"off");

 if(ioctl(st->fd,FBIOPUT_VSCREENINFO,&req)<0){
  fprintf(stderr,"[fbdev] FBIOPUT_VSCREENINFO failed on %s: %s\n",st->path,strerror(errno));
  return false;
 }

 /* Mark mode as changed IMMEDIATELY after successful FBIOPUT_VSCREENINFO,
  * so that any later failure (re-read, validation) still triggers restore. */
 st->mode_changed=true;

 /* Re-read both fix and var after successful modeset */
 if(ioctl(st->fd,FBIOGET_FSCREENINFO,&st->fix)<0){
  fprintf(stderr,"[fbdev] FBIOGET_FSCREENINFO after modeset on %s: %s\n",st->path,strerror(errno));
  return false;
 }
 if(ioctl(st->fd,FBIOGET_VSCREENINFO,&st->var)<0){
  fprintf(stderr,"[fbdev] FBIOGET_VSCREENINFO after modeset on %s: %s\n",st->path,strerror(errno));
  return false;
 }

 /* Warn if driver-adjusted actual mode differs from requested */
 if(st->var.xres!=(uint32_t)target_w||st->var.yres!=(uint32_t)target_h||st->var.bits_per_pixel!=(uint32_t)target_bpp){
  fprintf(stderr,"[fbdev] driver adjusted mode on %s: requested %dx%d@%dbpp, got %ux%u@%ubpp\n",
   st->path,target_w,target_h,target_bpp,st->var.xres,st->var.yres,st->var.bits_per_pixel);
 }

 fprintf(stderr,"[fbdev] modeset succeeded on %s: actual %ux%u@%ubpp virtual=%ux%u\n",
  st->path,st->var.xres,st->var.yres,st->var.bits_per_pixel,st->var.xres_virtual,st->var.yres_virtual);
 return true;
}

static bool fbdev_validate(Server *s,FbdevState *st){
 struct fb_var_screeninfo *v=&st->var;
 if(v->xres<320||v->xres>7680||v->yres<200||v->yres>4320){
   fprintf(stderr,"[fbdev] unsupported visible mode %ux%u on %s\n",v->xres,v->yres,st->path);
   return false;
 }
 if(st->fix.type!=FB_TYPE_PACKED_PIXELS){
   fprintf(stderr,"[fbdev] unsupported framebuffer type %u on %s (need FB_TYPE_PACKED_PIXELS)\n",st->fix.type,st->path);
   return false;
 }
 if(st->fix.visual!=FB_VISUAL_TRUECOLOR&&st->fix.visual!=FB_VISUAL_DIRECTCOLOR){
   fprintf(stderr,"[fbdev] unsupported framebuffer visual %u on %s (need truecolor or directcolor)\n",st->fix.visual,st->path);
   return false;
 }
 if(!cd_fb_format_init(&st->fmt,(int)v->bits_per_pixel,
   v->red.offset,v->red.length,v->green.offset,v->green.length,
   v->blue.offset,v->blue.length,v->transp.offset,v->transp.length)){
   fprintf(stderr,"[fbdev] unsupported pixel format on %s: %ubpp R%u/%u G%u/%u B%u/%u A%u/%u\n",st->path,v->bits_per_pixel,
    v->red.offset,v->red.length,v->green.offset,v->green.length,
    v->blue.offset,v->blue.length,v->transp.offset,v->transp.length);
   return false;
 }
 if(v->xres_virtual<v->xres||v->yres_virtual<v->yres){
   fprintf(stderr,"[fbdev] impossible virtual geometry on %s: visible %ux%u virtual %ux%u\n",st->path,v->xres,v->yres,v->xres_virtual,v->yres_virtual);
   return false;
 }
 if(v->xoffset>v->xres_virtual-v->xres||v->yoffset>v->yres_virtual-v->yres){
   fprintf(stderr,"[fbdev] visible origin %u,%u does not fit virtual %ux%u on %s\n",v->xoffset,v->yoffset,v->xres_virtual,v->yres_virtual,st->path);
   return false;
 }
 if(v->yres_virtual>INT_MAX){
   fprintf(stderr,"[fbdev] yres_virtual %u too large on %s\n",v->yres_virtual,st->path);
   return false;
 }
 size_t line=(size_t)st->fix.line_length;
 size_t row_bytes=(size_t)v->xres*(size_t)st->fmt.bytes_per_pixel;
 if(line<(size_t)(v->xoffset+v->xres)*(size_t)st->fmt.bytes_per_pixel){
   fprintf(stderr,"[fbdev] line_length %u too small for %ux%u %ubpp at xoffset %u on %s\n",st->fix.line_length,v->xres,v->yres,v->bits_per_pixel,v->xoffset,st->path);
   return false;
 }
 size_t visible_end=(size_t)v->yoffset*line+(size_t)v->yres*line;
 if(visible_end>(size_t)st->fix.smem_len||(size_t)st->fix.smem_len<row_bytes){
   fprintf(stderr,"[fbdev] smem_len %u too small for %ux%u stride %u on %s\n",st->fix.smem_len,v->xres,v->yres,st->fix.line_length,st->path);
   return false;
 }
 if(!cd_fb_geometry_init(&st->geo,(int)v->xres,(int)v->yres,(int)v->bits_per_pixel,st->fix.line_length,st->fix.smem_len,(int)v->yres_virtual)){
   fprintf(stderr,"[fbdev] impossible geometry on %s: line_length=%u smem_len=%u virtual=%ux%u\n",st->path,st->fix.line_length,st->fix.smem_len,v->xres_virtual,v->yres_virtual);
   return false;
 }
 if(st->fix.visual==FB_VISUAL_DIRECTCOLOR)
   fprintf(stderr,"[fbdev] %s is FB_VISUAL_DIRECTCOLOR; assuming linear channel ramps (no palette query in phase 1)\n",st->path);
 return true;
}

static bool fbdev_open(Server *s){
 FbdevState *st=calloc(1,sizeof(*st));
 if(!st){fprintf(stderr,"[fbdev] out of memory\n");return false;}
 st->fd=-1;
 st->mode_changed=false;
 const char *path=s->fbdev_path[0]?s->fbdev_path:"/dev/fb0";
 snprintf(st->path,sizeof(st->path),"%s",path);
 s->fbdev_state=st;

 st->fd=open(st->path,O_RDWR|O_CLOEXEC);
 if(st->fd<0){
  fprintf(stderr,"[fbdev] cannot open %s: %s (need write access, usually the video group or a udev rule)\n",st->path,strerror(errno));
  return false;
 }
 if(ioctl(st->fd,FBIOGET_FSCREENINFO,&st->fix)<0){fprintf(stderr,"[fbdev] FBIOGET_FSCREENINFO on %s: %s\n",st->path,strerror(errno));return false;}
 if(ioctl(st->fd,FBIOGET_VSCREENINFO,&st->var)<0){fprintf(stderr,"[fbdev] FBIOGET_VSCREENINFO on %s: %s\n",st->path,strerror(errno));return false;}
 st->orig_var=st->var;

 /* If modeset requested, attempt it before validation/mmap */
 bool modeset_requested=s->fbdev_modeset;
 if(modeset_requested){
  if(!fbdev_do_modeset(s,st)){
   fbdev_restore_mode(st);
   return false;
  }
 }

 if(!fbdev_validate(s,st)){
  if(st->mode_changed)fbdev_restore_mode(st);
  return false;
 }

 /* Capture current offsets after modeset/validation for all panning/base math.
  * Without a modeset cur==orig so phase-1 behavior is unchanged. */
 st->cur_xoffset=st->var.xoffset;
 st->cur_yoffset=st->var.yoffset;

 st->map_len=(size_t)st->fix.smem_len;
 if(st->map_len==0){fprintf(stderr,"[fbdev] %s reports smem_len=0\n",st->path);return false;}
 st->map=mmap(NULL,st->map_len,PROT_READ|PROT_WRITE,MAP_SHARED,st->fd,0);
 if(st->map==MAP_FAILED){st->map=NULL;fprintf(stderr,"[fbdev] mmap %s: %s\n",st->path,strerror(errno));return false;}

 if(st->mode_changed){
  fprintf(stderr,"[fbdev] modeset active on %s: %ux%u@%ubpp (configured %dx%d)\n",
   st->path,st->var.xres,st->var.yres,st->var.bits_per_pixel,s->width,s->height);
 }else if(s->width!=(int)st->var.xres||s->height!=(int)st->var.yres){
  fprintf(stderr,"[fbdev] using current %s mode %ux%u (configured render size %dx%d ignored; no modeset requested)\n",st->path,st->var.xres,st->var.yres,s->width,s->height);
 }
 s->width=(int)st->var.xres;s->height=(int)st->var.yres;
 s->mouse_x=s->width/2;s->mouse_y=s->height/2;

 st->display_page=(int)(st->cur_yoffset/st->var.yres);
 if(st->display_page<0||st->display_page>=st->geo.page_count)st->display_page=0;
 bool aligned=(st->cur_yoffset%st->var.yres)==0;
 st->direct_base=fbdev_visible_base(st);
 st->pan_enabled=s->fbdev_pan&&st->geo.pan_possible&&aligned;
 if(s->fbdev_pan&&st->geo.pan_possible&&!aligned)
  fprintf(stderr,"[fbdev] %s yoffset %u is not page-aligned; dual-page panning stays off\n",st->path,st->cur_yoffset);
 st->vsync_enabled=s->fbdev_vsync;
 fprintf(stderr,"[fbdev] %s: %ux%u %ubpp stride=%u pages=%d pan=%s vsync=%s\n",st->path,st->var.xres,st->var.yres,st->var.bits_per_pixel,st->fix.line_length,st->geo.page_count,st->pan_enabled?"on":"off",st->vsync_enabled?"on":"off");
 return true;
}

bool backend_fbdev_init(Server *s){
  if(!fbdev_open(s)){fbdev_close(s);return false;}
  if(!backend_egl_init(s,"fbdev")){fbdev_close(s);return false;}
  if(s->fbdev_evdev && !backend_evdev_init(s)){
    backend_egl_shutdown(s);
    fbdev_close(s);
    return false;
  }
  if(s->fbdev_vnc_mirror&&!backend_vnc_init(s,true)){
    if(s->fbdev_evdev) backend_evdev_shutdown(s);
    backend_egl_shutdown(s);
    fbdev_close(s);
    return false;
  }
  bool has_local_input = s->fbdev_evdev && ((EvdevState*)s->evdev_state)->device_count > 0;
  if(!s->fbdev_vnc_mirror && !has_local_input)
    fprintf(stderr,"[fbdev] output-only for now: local evdev input is not available (no devices opened or evdev disabled); use --fbdev-vnc-mirror for loopback RFB control\n");
  return true;
}

void backend_fbdev_poll(Server *s){
  if(s->fbdev_evdev) backend_evdev_poll(s);
  if(s->vnc_screen) backend_vnc_poll(s);
}

void backend_fbdev_present(Server *s){
 FbdevState *st=fbdev_state(s);
 if(!st||!st->map||!s->readback_pixels)return;

 glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);
 glReadPixels(0,0,s->width,s->height,GL_RGBA,GL_UNSIGNED_BYTE,s->readback_pixels);

 bool do_pan=st->pan_enabled;
 int back_page=st->display_page;
 size_t target=st->direct_base;
 if(do_pan){
  int next=(st->display_page+1)%st->geo.page_count;
  size_t base=fbdev_page_base(st,next);
  if(fbdev_region_fits(st,base)){back_page=next;target=base;}
  else{
   if(!st->warned_pan)fprintf(stderr,"[fbdev] page %d outside the mapped framebuffer on %s; falling back to direct visible-page writes\n",next,st->path);
   st->warned_pan=true;st->pan_enabled=false;do_pan=false;
  }
 }
 if(!do_pan){fbdev_wait_vsync(st);target=st->direct_base;}
 if(!fbdev_region_fits(st,target))return;

 cd_fb_convert_rgba(&st->fmt,s->readback_pixels,(size_t)s->width*4,s->width,s->height,true,st->map+target,st->fix.line_length);

 if(do_pan){
  fbdev_wait_vsync(st);
  struct fb_var_screeninfo var=st->var;
  var.xoffset=st->cur_xoffset;
  var.yoffset=(uint32_t)back_page*st->var.yres;
  if(ioctl(st->fd,FBIOPAN_DISPLAY,&var)==0){
   st->var=var;st->display_page=back_page;st->direct_base=target;st->panned=true;
  }else{
   if(!st->warned_pan)fprintf(stderr,"[fbdev] FBIOPAN_DISPLAY failed on %s (%s); falling back to direct visible-page writes\n",st->path,strerror(errno));
   st->warned_pan=true;st->pan_enabled=false;
   if(st->direct_base!=target&&fbdev_region_fits(st,st->direct_base))fbdev_copy_visible_rows(st,st->direct_base,target);
  }
 }
 if(s->vnc_screen)backend_vnc_present_readback(s);
}

void backend_fbdev_shutdown(Server *s){
  if(s->fbdev_evdev) backend_evdev_shutdown(s);
  if(s->vnc_screen) backend_vnc_shutdown(s);
  fbdev_close(s);
  backend_egl_shutdown(s);
}
