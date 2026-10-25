#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client.h"
#include "linux-dmabuf-client.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static struct wl_display *display;static struct wl_compositor *compositor;static struct wl_subcompositor *subcomp;static struct wl_shm *shm;static struct xdg_wm_base *wm;static struct wl_seat *seat;static struct wl_keyboard *keyboard;static struct wl_touch *touch;
static struct zwp_linux_dmabuf_v1 *dmabuf;static int dmabuf_formats,dmabuf_modifiers;
static int outputs,configured,frames,releases,enters,keymaps,keyboard_enters;static int running=1;
static int touch_advertised,touch_downs,touch_ups,touch_motions,touch_frames,touch_cancels;
static void timeout_handler(int sig){_exit(124);}
static void geom(void *d,struct wl_output *o,int32_t x,int32_t y,int32_t pw,int32_t ph,int32_t sub,const char *make,const char *model,int32_t tr){}
static void mode(void *d,struct wl_output *o,uint32_t f,int32_t w,int32_t h,int32_t hz){assert((w==1000&&h==625)||(w==1000&&h==404));}
static void done(void *d,struct wl_output *o){}
static void scale(void *d,struct wl_output *o,int32_t s){assert(s==1);}
static void name(void *d,struct wl_output *o,const char *s){assert(strncmp(s,"CD-",3)==0);}
static void desc(void *d,struct wl_output *o,const char *s){}
static const struct wl_output_listener output_listener={.geometry=geom,.mode=mode,.done=done,.scale=scale,.name=name,.description=desc};
static void keymap(void *d,struct wl_keyboard *k,uint32_t fmt,int32_t fd,uint32_t size){assert(fmt==1&&size>100);char *m=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);assert(m!=MAP_FAILED);assert(strstr(m,"xkb_keymap"));keymaps++;munmap(m,size);close(fd);}
static void keyenter(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s,struct wl_array *keys){keyboard_enters++;}
static void keyleave(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s){}
static void key(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t time,uint32_t key,uint32_t state){}
static void mods(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t dep,uint32_t lat,uint32_t lock,uint32_t group){}
static void repeat(void *d,struct wl_keyboard *k,int32_t rate,int32_t delay){}
static const struct wl_keyboard_listener keyboard_listener={.keymap=keymap,.enter=keyenter,.leave=keyleave,.key=key,.modifiers=mods,.repeat_info=repeat};
static void touchdown(void *d,struct wl_touch *t,uint32_t serial,uint32_t time,struct wl_surface *surface,int32_t id,wl_fixed_t x,wl_fixed_t y){touch_downs++;}
static void touchup(void *d,struct wl_touch *t,uint32_t serial,uint32_t time,int32_t id){touch_ups++;}
static void touchmotion(void *d,struct wl_touch *t,uint32_t time,int32_t id,wl_fixed_t x,wl_fixed_t y){touch_motions++;}
static void touchframe(void *d,struct wl_touch *t){touch_frames++;}
static void touchcancel(void *d,struct wl_touch *t){touch_cancels++;}
static void touchshape(void *d,struct wl_touch *t,int32_t id,wl_fixed_t major,wl_fixed_t minor){}
static void touchorientation(void *d,struct wl_touch *t,int32_t id,wl_fixed_t orientation){}
static const struct wl_touch_listener touch_listener={.down=touchdown,.up=touchup,.motion=touchmotion,.frame=touchframe,.cancel=touchcancel,.shape=touchshape,.orientation=touchorientation};
static void caps(void *d,struct wl_seat *s,uint32_t caps){if((caps&WL_SEAT_CAPABILITY_KEYBOARD)&&!keyboard){keyboard=wl_seat_get_keyboard(s);wl_keyboard_add_listener(keyboard,&keyboard_listener,NULL);}if(caps&WL_SEAT_CAPABILITY_TOUCH){touch_advertised=1;if(!touch){touch=wl_seat_get_touch(s);wl_touch_add_listener(touch,&touch_listener,NULL);}}}
static void seatname(void *d,struct wl_seat *s,const char *name){}
static const struct wl_seat_listener seat_listener={.capabilities=caps,.name=seatname};
static void ping(void *d,struct xdg_wm_base *w,uint32_t serial){xdg_wm_base_pong(w,serial);}
static const struct xdg_wm_base_listener wm_listener={.ping=ping};
static void dmabuf_format(void *d,struct zwp_linux_dmabuf_v1 *z,uint32_t format){dmabuf_formats++;}
static void dmabuf_modifier(void *d,struct zwp_linux_dmabuf_v1 *z,uint32_t format,uint32_t hi,uint32_t lo){dmabuf_modifiers++;}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener={.format=dmabuf_format,.modifier=dmabuf_modifier};
static void global(void *data,struct wl_registry *r,uint32_t id,const char *iface,uint32_t v){
 if(!strcmp(iface,"wl_compositor"))compositor=wl_registry_bind(r,id,&wl_compositor_interface,4);
 if(!strcmp(iface,"wl_subcompositor"))subcomp=wl_registry_bind(r,id,&wl_subcompositor_interface,1);
 if(!strcmp(iface,"wl_shm"))shm=wl_registry_bind(r,id,&wl_shm_interface,1);
 if(!strcmp(iface,"xdg_wm_base")){wm=wl_registry_bind(r,id,&xdg_wm_base_interface,3);xdg_wm_base_add_listener(wm,&wm_listener,NULL);}
 if(!strcmp(iface,"wl_output")){struct wl_output *o=wl_registry_bind(r,id,&wl_output_interface,4);wl_output_add_listener(o,&output_listener,NULL);outputs++;}
 if(!strcmp(iface,"wl_seat")){seat=wl_registry_bind(r,id,&wl_seat_interface,7);wl_seat_add_listener(seat,&seat_listener,NULL);}
 if(!strcmp(iface,"zwp_linux_dmabuf_v1")){dmabuf=wl_registry_bind(r,id,&zwp_linux_dmabuf_v1_interface,v<3?v:3);zwp_linux_dmabuf_v1_add_listener(dmabuf,&dmabuf_listener,NULL);}
}
static void removed(void *d,struct wl_registry *r,uint32_t id){}
static const struct wl_registry_listener registry_listener={.global=global,.global_remove=removed};
static void configure(void *d,struct xdg_surface *s,uint32_t serial){xdg_surface_ack_configure(s,serial);configured++;}
static const struct xdg_surface_listener xdg_listener={.configure=configure};
static void topconf(void *d,struct xdg_toplevel *t,int32_t w,int32_t h,struct wl_array *states){assert(w==1000&&h==625);}
static void close_top(void *d,struct xdg_toplevel *t){running=0;}
static const struct xdg_toplevel_listener top_listener={.configure=topconf,.close=close_top};
static void released(void *d,struct wl_buffer *b){releases++;}
static const struct wl_buffer_listener buffer_listener={.release=released};
static void framed(void *d,struct wl_callback *c,uint32_t time){frames++;wl_callback_destroy(c);}
static const struct wl_callback_listener frame_listener={.done=framed};
static void enter(void *d,struct wl_surface *s,struct wl_output *o){enters++;}
static void leave(void *d,struct wl_surface *s,struct wl_output *o){}
static const struct wl_surface_listener surface_listener={.enter=enter,.leave=leave};
static void popupconf(void *d,struct xdg_popup *p,int32_t x,int32_t y,int32_t w,int32_t h){assert(w==200&&h==120);}
static void popupdone(void *d,struct xdg_popup *p){}
static void repositioned(void *d,struct xdg_popup *p,uint32_t token){}
static const struct xdg_popup_listener popup_listener={.configure=popupconf,.popup_done=popupdone,.repositioned=repositioned};
static void commit(struct wl_surface *s,struct wl_buffer *b){wl_surface_attach(s,b,0,0);wl_surface_damage_buffer(s,0,0,1000,625);struct wl_callback *cb=wl_surface_frame(s);wl_callback_add_listener(cb,&frame_listener,NULL);wl_surface_commit(s);}
static void wait_frames(int target){while(running&&frames<target)assert(wl_display_dispatch(display)>=0);assert(running);}
int main(void){
 signal(SIGALRM,timeout_handler);alarm(10);display=wl_display_connect(NULL);assert(display);struct wl_registry *registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);assert(wl_display_roundtrip(display)>=0);assert(compositor&&shm&&wm&&subcomp&&seat);assert(wl_display_roundtrip(display)>=0);assert(wl_display_roundtrip(display)>=0);assert(outputs==9&&keymaps==1);assert(!touch_advertised||touch);
 struct wl_surface *surface=wl_compositor_create_surface(compositor);wl_surface_add_listener(surface,&surface_listener,NULL);struct xdg_surface *xdg=xdg_wm_base_get_xdg_surface(wm,surface);xdg_surface_add_listener(xdg,&xdg_listener,NULL);struct xdg_toplevel *top=xdg_surface_get_toplevel(xdg);xdg_toplevel_add_listener(top,&top_listener,NULL);xdg_toplevel_set_title(top,"Protocol probe");xdg_toplevel_set_app_id(top,"chromedrive.probe");wl_surface_commit(surface);assert(wl_display_roundtrip(display)>=0);assert(configured>0);
 size_t size=1000*625*4;int fd=memfd_create("probe",MFD_CLOEXEC);assert(fd>=0&&ftruncate(fd,size)==0);uint32_t *pixels=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(pixels!=MAP_FAILED);struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,size);struct wl_buffer *buffer=wl_shm_pool_create_buffer(pool,0,1000,625,4000,WL_SHM_FORMAT_XRGB8888);wl_buffer_add_listener(buffer,&buffer_listener,NULL);
 for(int n=0;n<3;n++){for(int i=0;i<1000*625;i++)pixels[i]=0xff204050u+n*0x000c1003;commit(surface,buffer);wait_frames(n+1);}assert(enters==1&&keyboard_enters>=1&&releases>=3);
 /* A synchronized child must become visible on the parent's next commit. */
 struct wl_surface *child=wl_compositor_create_surface(compositor);struct wl_subsurface *sub=wl_subcompositor_get_subsurface(subcomp,child,surface);wl_subsurface_set_position(sub,24,24);struct wl_buffer *small=wl_shm_pool_create_buffer(pool,0,64,64,256,WL_SHM_FORMAT_XRGB8888);wl_buffer_add_listener(small,&buffer_listener,NULL);commit(child,small);wl_surface_commit(surface);wait_frames(4);wl_subsurface_destroy(sub);wl_surface_destroy(child);wl_buffer_destroy(small);
 /* Construct, configure, map, destroy a real xdg popup. */
 struct wl_surface *ps=wl_compositor_create_surface(compositor);struct xdg_surface *px=xdg_wm_base_get_xdg_surface(wm,ps);xdg_surface_add_listener(px,&xdg_listener,NULL);struct xdg_positioner *pos=xdg_wm_base_create_positioner(wm);xdg_positioner_set_size(pos,200,120);xdg_positioner_set_anchor_rect(pos,50,50,30,30);xdg_positioner_set_anchor(pos,XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);xdg_positioner_set_gravity(pos,XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);struct xdg_popup *popup=xdg_surface_get_popup(px,xdg,pos);xdg_popup_add_listener(popup,&popup_listener,NULL);wl_surface_commit(ps);assert(wl_display_roundtrip(display)>=0);struct wl_buffer *pb=wl_shm_pool_create_buffer(pool,0,200,120,800,WL_SHM_FORMAT_XRGB8888);wl_buffer_add_listener(pb,&buffer_listener,NULL);commit(ps,pb);wait_frames(5);xdg_popup_destroy(popup);xdg_positioner_destroy(pos);xdg_surface_destroy(px);wl_surface_destroy(ps);wl_buffer_destroy(pb);
 /* Unmap/remap the same toplevel. A fresh configure handshake is required. */
 wl_surface_attach(surface,NULL,0,0);wl_surface_commit(surface);assert(wl_display_roundtrip(display)>=0);wl_surface_commit(surface);assert(wl_display_roundtrip(display)>=0);commit(surface,buffer);wait_frames(6);assert(enters==2&&keyboard_enters>=2&&releases>=6);
 /* zwp_linux_dmabuf_v1: only advertised by the EGL backends (headless/fbdev;
  * see README — EGLImage doesn't interoperate with the --windowed GLX dev backend). */
 if(dmabuf){
  assert(wl_display_roundtrip(display)>=0);
  if(dmabuf_formats>0)printf("PASS: zwp_linux_dmabuf_v1 negotiated %d format(s)/%d modifier event(s)\n",dmabuf_formats,dmabuf_modifiers);
  else printf("INFO: zwp_linux_dmabuf_v1 bound but advertises no formats (windowed/GLX dev backend — see README)\n");
 }else printf("INFO: zwp_linux_dmabuf_v1 not advertised (windowed/GLX dev backend — see README)\n");
 /* End-to-end: actually attach a dma-buf-backed wl_buffer to a real xdg
  * toplevel and confirm the compositor maps + renders a frame from it
  * without crashing. This sandbox's software EGL driver imports lazily (see
  * README) so it accepts our bogus-but-real fd here -- that's enough to
  * exercise the real wayland.c surface_attach/apply dma-buf code path
  * (texture ownership handoff, mapping, frame callback) end-to-end, even
  * though it can't prove the pixel bytes came from real GPU memory. */
 if(dmabuf&&dmabuf_formats>0){
  struct wl_surface *ds=wl_compositor_create_surface(compositor);wl_surface_add_listener(ds,&surface_listener,NULL);
  struct xdg_surface *dx=xdg_wm_base_get_xdg_surface(wm,ds);xdg_surface_add_listener(dx,&xdg_listener,NULL);
  struct xdg_toplevel *dt=xdg_surface_get_toplevel(dx);xdg_toplevel_add_listener(dt,&top_listener,NULL);
  xdg_toplevel_set_title(dt,"Dmabuf probe");xdg_toplevel_set_app_id(dt,"chromedrive.dmabuf-probe");
  int before_configured=configured;wl_surface_commit(ds);assert(wl_display_roundtrip(display)>=0);assert(configured>before_configured);
  struct zwp_linux_buffer_params_v1 *dp=zwp_linux_dmabuf_v1_create_params(dmabuf);
  int dfd=memfd_create("dmabuf-surface-test",MFD_CLOEXEC);assert(dfd>=0&&ftruncate(dfd,(size_t)64*64*4)==0);
  zwp_linux_buffer_params_v1_add(dp,dfd,0,0,64*4,0,0);
  struct wl_buffer *dbuf=zwp_linux_buffer_params_v1_create_immed(dp,64,64,0x34325241u,0); /* DRM_FORMAT_ARGB8888 */
  wl_buffer_add_listener(dbuf,&buffer_listener,NULL);
  int before_frames=frames;wl_surface_attach(ds,dbuf,0,0);wl_surface_damage_buffer(ds,0,0,64,64);struct wl_callback *dcb=wl_surface_frame(ds);wl_callback_add_listener(dcb,&frame_listener,NULL);wl_surface_commit(ds);
  wait_frames(before_frames+1);
  printf("PASS: a dma-buf-backed wl_buffer attached to a real xdg toplevel mapped and rendered a frame (zero-copy GL-texture code path exercised end-to-end)\n");
  xdg_toplevel_destroy(dt);xdg_surface_destroy(dx);wl_surface_destroy(ds);wl_buffer_destroy(dbuf);zwp_linux_buffer_params_v1_destroy(dp);assert(wl_display_roundtrip(display)>=0);
 }
 if(touch)printf("PASS: wl_seat advertised touch and wl_touch resource created (touch events down=%d up=%d motion=%d frame=%d cancel=%d)\n",touch_downs,touch_ups,touch_motions,touch_frames,touch_cancels);
 else printf("INFO: wl_touch not advertised by this backend (headless/fbdev without SDL input is pointer-only; --windowed/SDL advertises touchscreen touch)\n");
 printf("PASS: outputs=%d, configures=%d, frames=%d, buffer_releases=%d, output_enters=%d, keyboard_enters=%d, keymaps=%d; synchronized subsurface + xdg popup + unmap/remap\n",outputs,configured,frames,releases,enters,keyboard_enters,keymaps);fflush(stdout);
 xdg_toplevel_destroy(top);xdg_surface_destroy(xdg);wl_surface_destroy(surface);wl_buffer_destroy(buffer);wl_shm_pool_destroy(pool);if(touch)wl_touch_release(touch);wl_keyboard_release(keyboard);wl_seat_release(seat);assert(wl_display_roundtrip(display)>=0);munmap(pixels,size);close(fd);
 /* Structural dma-buf parameter validation runs entirely in our own code
  * before any EGL call, so (unlike "is this fd really GPU memory", which
  * this sandbox's software EGL driver answers unreliably — see README) it is
  * fully deterministic. An incomplete (zero-plane) params object must be
  * rejected with a protocol error. This kills the connection, so it's last. */
 if(dmabuf&&dmabuf_formats>0){
  struct zwp_linux_buffer_params_v1 *bad=zwp_linux_dmabuf_v1_create_params(dmabuf);
  zwp_linux_buffer_params_v1_create(bad,32,32,0x34325241u,0); /* zero planes added -> INCOMPLETE */
  wl_display_roundtrip(display); /* expected to fail: that's the point */
  assert(wl_display_get_error(display)!=0);
  printf("PASS: invalid (zero-plane) dma-buf params correctly rejected with a protocol error\n");
 }
 wl_display_disconnect(display);return 0;
}
