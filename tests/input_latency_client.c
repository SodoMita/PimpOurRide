#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client.h"
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct wl_surface *surface;
static struct xdg_surface *xdg;
static struct xdg_toplevel *toplevel;
static struct wl_buffer *buffer;
static struct wl_shm_pool *pool;
static FILE *log_file;
static const char *ready_path;
static int configured,entered,frames,running=1,z_events,z_target;
static uint64_t mono_ns(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;}
static void timeout_handler(int sig){(void)sig;_exit(124);}
static void keymap(void *d,struct wl_keyboard *k,uint32_t format,int32_t fd,uint32_t size){(void)d;(void)k;(void)format;(void)size;close(fd);}
static void key_enter(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s,struct wl_array *keys){(void)d;(void)k;(void)serial;(void)s;(void)keys;entered=1;}
static void key_leave(void *d,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s){(void)d;(void)k;(void)serial;(void)s;entered=0;}
static void key_event(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t time,uint32_t key,uint32_t state){
 (void)d;(void)k;(void)serial;fprintf(log_file,"K,%u,%u,%u,%llu\n",key,state,time,(unsigned long long)mono_ns());
 if(key==KEY_Z&&++z_events>=z_target&&z_target>0)running=0;
}
static void modifiers(void *d,struct wl_keyboard *k,uint32_t serial,uint32_t dep,uint32_t lat,uint32_t locked,uint32_t group){(void)d;(void)k;(void)serial;(void)dep;(void)lat;(void)locked;(void)group;}
static void repeat_info(void *d,struct wl_keyboard *k,int32_t rate,int32_t delay){(void)d;(void)k;(void)rate;(void)delay;}
static const struct wl_keyboard_listener keyboard_listener={.keymap=keymap,.enter=key_enter,.leave=key_leave,.key=key_event,.modifiers=modifiers,.repeat_info=repeat_info};
static void pointer_enter(void *d,struct wl_pointer *p,uint32_t serial,struct wl_surface *s,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)serial;(void)s;(void)x;(void)y;}
static void pointer_leave(void *d,struct wl_pointer *p,uint32_t serial,struct wl_surface *s){(void)d;(void)p;(void)serial;(void)s;}
static void pointer_motion(void *d,struct wl_pointer *p,uint32_t time,wl_fixed_t x,wl_fixed_t y){(void)d;(void)p;(void)x;(void)y;(void)time;}
static void pointer_button(void *d,struct wl_pointer *p,uint32_t serial,uint32_t time,uint32_t button,uint32_t state){(void)d;(void)p;(void)serial;fprintf(log_file,"B,%u,%u,%u,%llu\n",button,state,time,(unsigned long long)mono_ns());}
static void pointer_axis(void *d,struct wl_pointer *p,uint32_t time,uint32_t axis,wl_fixed_t value){(void)d;(void)p;fprintf(log_file,"A,%u,%f,%u,%llu\n",axis,wl_fixed_to_double(value),time,(unsigned long long)mono_ns());}
static void pointer_frame(void *d,struct wl_pointer *p){(void)d;(void)p;}
static void pointer_axis_source(void *d,struct wl_pointer *p,uint32_t source){(void)d;(void)p;(void)source;}
static void pointer_axis_stop(void *d,struct wl_pointer *p,uint32_t time,uint32_t axis){(void)d;(void)p;(void)time;(void)axis;}
static void pointer_axis_discrete(void *d,struct wl_pointer *p,uint32_t axis,int32_t discrete){(void)d;(void)p;(void)axis;(void)discrete;}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=pointer_motion,.button=pointer_button,.axis=pointer_axis,.frame=pointer_frame,.axis_source=pointer_axis_source,.axis_stop=pointer_axis_stop,.axis_discrete=pointer_axis_discrete};
static void seat_caps(void *d,struct wl_seat *s,uint32_t caps){
 (void)d;
 if((caps&WL_SEAT_CAPABILITY_KEYBOARD)&&!keyboard){keyboard=wl_seat_get_keyboard(s);wl_keyboard_add_listener(keyboard,&keyboard_listener,NULL);}
 if((caps&WL_SEAT_CAPABILITY_POINTER)&&!pointer){pointer=wl_seat_get_pointer(s);wl_pointer_add_listener(pointer,&pointer_listener,NULL);}
}
static void seat_name(void *d,struct wl_seat *s,const char *name){(void)d;(void)s;(void)name;}
static const struct wl_seat_listener seat_listener={.capabilities=seat_caps,.name=seat_name};
static void wm_ping(void *d,struct xdg_wm_base *w,uint32_t serial){(void)d;xdg_wm_base_pong(w,serial);}
static const struct xdg_wm_base_listener wm_listener={.ping=wm_ping};
static void registry_global(void *data,struct wl_registry *r,uint32_t id,const char *iface,uint32_t version){
 (void)data;(void)version;
 if(!strcmp(iface,"wl_compositor"))compositor=wl_registry_bind(r,id,&wl_compositor_interface,4);
 else if(!strcmp(iface,"wl_shm"))shm=wl_registry_bind(r,id,&wl_shm_interface,1);
 else if(!strcmp(iface,"xdg_wm_base")){wm=wl_registry_bind(r,id,&xdg_wm_base_interface,3);xdg_wm_base_add_listener(wm,&wm_listener,NULL);}
 else if(!strcmp(iface,"wl_seat")){seat=wl_registry_bind(r,id,&wl_seat_interface,7);wl_seat_add_listener(seat,&seat_listener,NULL);}
}
static void registry_remove(void *d,struct wl_registry *r,uint32_t id){(void)d;(void)r;(void)id;}
static const struct wl_registry_listener registry_listener={.global=registry_global,.global_remove=registry_remove};
static void xdg_configure(void *d,struct xdg_surface *s,uint32_t serial){(void)d;xdg_surface_ack_configure(s,serial);configured=1;}
static const struct xdg_surface_listener xdg_listener={.configure=xdg_configure};
static void top_configure(void *d,struct xdg_toplevel *t,int32_t w,int32_t h,struct wl_array *states){(void)d;(void)t;(void)w;(void)h;(void)states;}
static void top_close(void *d,struct xdg_toplevel *t){(void)d;(void)t;running=0;}
static const struct xdg_toplevel_listener top_listener={.configure=top_configure,.close=top_close};
static void frame_done(void *d,struct wl_callback *c,uint32_t time){(void)d;(void)time;frames++;wl_callback_destroy(c);}
static const struct wl_callback_listener frame_listener={.done=frame_done};
static int create_buffer(void){
 const int w=1000,h=625;size_t size=(size_t)w*h*4;int fd=memfd_create("input-latency",MFD_CLOEXEC);if(fd<0||ftruncate(fd,(off_t)size)<0)return -1;
 uint32_t *pixels=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(pixels==MAP_FAILED){close(fd);return -1;}
 for(size_t i=0;i<(size_t)w*h;i++)pixels[i]=0xff203846u;munmap(pixels,size);
 pool=wl_shm_create_pool(shm,fd,(int)size);close(fd);if(!pool)return -1;
 buffer=wl_shm_pool_create_buffer(pool,0,w,h,w*4,WL_SHM_FORMAT_XRGB8888);return buffer?0:-1;
}
int main(int argc,char **argv){
 const char *log_path=NULL;int timeout=60;
 for(int i=1;i<argc;i++){
  if(!strcmp(argv[i],"--ready")&&i+1<argc)ready_path=argv[++i];
  else if(!strcmp(argv[i],"--log")&&i+1<argc)log_path=argv[++i];
  else if(!strcmp(argv[i],"--count")&&i+1<argc)z_target=atoi(argv[++i]);
  else if(!strcmp(argv[i],"--timeout")&&i+1<argc)timeout=atoi(argv[++i]);
  else{fprintf(stderr,"usage: %s [--ready PATH] [--log PATH] [--count N] [--timeout SEC]\n",argv[0]);return 2;}
 }
 if(!log_path||z_target<0){fprintf(stderr,"--log is required and --count must be nonnegative\n");return 2;}
 log_file=fopen(log_path,"w");if(!log_file){perror("log");return 1;}setvbuf(log_file,NULL,_IOLBF,0);
 signal(SIGALRM,timeout_handler);alarm((unsigned)timeout);
 display=wl_display_connect(NULL);if(!display){perror("wl_display_connect");return 1;}
 struct wl_registry *registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);
 if(wl_display_roundtrip(display)<0||!compositor||!shm||!wm||!seat)return 1;
 if(wl_display_roundtrip(display)<0||!keyboard||!pointer)return 1;
 surface=wl_compositor_create_surface(compositor);xdg=xdg_wm_base_get_xdg_surface(wm,surface);xdg_surface_add_listener(xdg,&xdg_listener,NULL);
 toplevel=xdg_surface_get_toplevel(xdg);xdg_toplevel_add_listener(toplevel,&top_listener,NULL);xdg_toplevel_set_title(toplevel,"Virtual input receiver");xdg_toplevel_set_app_id(toplevel,"chromedrive.input-benchmark");
 wl_surface_commit(surface);if(wl_display_roundtrip(display)<0||!configured||create_buffer()<0)return 1;
 wl_surface_attach(surface,buffer,0,0);wl_surface_damage_buffer(surface,0,0,1000,625);struct wl_callback *cb=wl_surface_frame(surface);wl_callback_add_listener(cb,&frame_listener,NULL);wl_surface_commit(surface);
 while(running&&(!entered||frames<1))if(wl_display_dispatch(display)<0)return 1;
 if(ready_path){FILE *ready=fopen(ready_path,"w");if(!ready){perror("ready");return 1;}fprintf(ready,"%llu\n",(unsigned long long)mono_ns());fclose(ready);}
 while(running)if(wl_display_dispatch(display)<0)return 1;
 alarm(0);wl_surface_attach(surface,NULL,0,0);wl_surface_commit(surface);wl_display_roundtrip(display);
 xdg_toplevel_destroy(toplevel);xdg_surface_destroy(xdg);wl_surface_destroy(surface);wl_buffer_destroy(buffer);wl_shm_pool_destroy(pool);
 wl_keyboard_release(keyboard);wl_pointer_release(pointer);wl_seat_release(seat);wl_display_disconnect(display);fclose(log_file);return 0;
}
