/* Native Wayland server. No X11 window capture and no browser surfaces.
 *
 * Architectural "Why" Behind Core Protocol Sequences:
 * 1. Atomic Double-Buffered Surface State (wl_surface.attach / damage / frame / commit):
 *    In Wayland, client requests such as attach, damage, set_input_region, and
 *    frame do NOT mutate the live scene immediately. Instead, they populate
 *    pending_* fields on Surface until wl_surface.commit atomically promotes
 *    them: uploading SHM pixels (or binding an imported DMA-BUF EGLImage) into
 *    the surface's OpenGL texture, splicing pending_callbacks into the active
 *    callbacks list, and releasing the wl_shm buffer immediately after CPU-to-GL
 *    upload so double-buffered clients can reuse their backbuffer without waiting.
 *
 * 2. XDG-Shell Handshake (initial bufferless commit -> configure -> ack_configure):
 *    Per the xdg-shell specification, a newly created xdg_surface + xdg_toplevel
 *    must first perform an initial wl_surface.commit WITH NO BUFFER ATTACHED.
 *    Only in response to that bufferless commit does the compositor send
 *    xdg_toplevel.configure(width, height, states) followed by
 *    xdg_surface.configure(serial). The client must reply with
 *    xdg_surface.ack_configure(serial) before attaching its first pixel buffer;
 *    enforcing configured_sent && acked_configure prevents unconfigured 1x1 or
 *    wrongly-sized flashes on the 3D cockpit monitors.
 *
 * 3. Spatial Monitor Assignment & Passive Instrument Cluster (CD-01 .. CD-09):
 *    Every interactive xdg_toplevel receives its own dedicated 3D spatial monitor
 *    slot (0..CD_APP_MONITORS-1) and a matching wl_surface.enter(wl_output) event.
 *    The instrument cluster client (app_id "chromedrive.cluster") is routed to
 *    slot CD_CLUSTER_MONITOR (CD-09, embedded inside the binnacle behind the
 *    steering wheel) and mapped passively without stealing keyboard/pointer focus
 *    or shifting the driver's camera view.
 *
 * 4. Frame Callback Backpressure (wl_surface.frame -> server_frame_done):
 *    Frame callbacks queued via wl_surface.frame stay in pending_callbacks until
 *    wl_surface.commit moves them to callbacks. Crucially, server_frame_done()
 *    is called ONLY AFTER scene_render() and backend presentation complete,
 *    firing wl_callback.done(compositor_ms) and destroying the one-shot callback
 *    resource. This throttles client rendering to the compositor's actual
 *    presentation rate and prevents background clients from spinning at 100% CPU. */
#include "chrome.h"
#include "xdg-shell-server.h"
#include "xdg-decoration-server.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void resource_destroy_request(struct wl_client *c,struct wl_resource *r){wl_resource_destroy(r);}
static void free_resource(struct wl_resource *r){Resource *item=wl_resource_get_user_data(r);if(item){wl_list_remove(&item->link);free(item);}}
static Resource *resource_new(struct wl_client *c,const struct wl_interface *iface,int ver,uint32_t id,struct wl_list *list){
 Resource *r=calloc(1,sizeof(*r));if(!r){wl_client_post_no_memory(c);return NULL;}
 r->resource=wl_resource_create(c,iface,ver,id);if(!r->resource){free(r);wl_client_post_no_memory(c);return NULL;}
 wl_list_insert(list->prev,&r->link);return r;
}
Surface *surface_root(Surface *s){int limit=32;while(s&&s->parent&&limit--)s=s->parent;return s;}
static void send_configure(Surface *s);
static void selection_for_client(Server *s,struct wl_client *client);
static void surface_unmap(Surface *p){
 Server *s=p->server;
 if(s->keyboard_focus==p)server_focus(s,NULL);
 if(s->pointer_focus==p){s->pointer_focus=NULL;}
 if(s->pointer_grab==p){s->pointer_grab=NULL;s->held_buttons=0;}
 if(p->slot>=0&&s->monitors[p->slot].surface==p){
  Resource *r;wl_list_for_each(r,&s->outputs,link)if(r->slot==p->slot&&wl_resource_get_client(r->resource)==wl_resource_get_client(p->resource))wl_surface_send_leave(p->resource,r->resource);
  s->monitors[p->slot].surface=NULL;s->monitors[p->slot].reveal=0;s->monitors[p->slot].target_reveal=0;
  s->monitors[p->slot].plane=monitor_plane(p->slot);
 }
 p->mapped=false;p->slot=-1;
}
static void callback_destroy(struct wl_resource *r){Callback *cb=wl_resource_get_user_data(r);if(cb){wl_list_remove(&cb->link);free(cb);}}
static void callbacks_clear(struct wl_list *list){Callback *cb,*tmp;wl_list_for_each_safe(cb,tmp,list,link)wl_resource_destroy(cb->resource);}
static void pending_buffer_gone(struct wl_listener *l,void *data){Surface *p=wl_container_of(l,p,buffer_destroy);p->pending_buffer=NULL;wl_list_remove(&l->link);wl_list_init(&l->link);}
static void pending_buffer_release(Surface *p){
 if(p->pending_buffer){wl_buffer_send_release(p->pending_buffer);wl_list_remove(&p->buffer_destroy.link);wl_list_init(&p->buffer_destroy.link);p->pending_buffer=NULL;}
}
static void surface_destroyed(struct wl_resource *r){
 Surface *p=wl_resource_get_user_data(r);if(!p)return;Server *s=p->server;surface_unmap(p);
 Surface *child;wl_list_for_each(child,&s->surfaces,link)if(child->parent==p){child->parent=NULL;child->mapped=false;if(child->popup)xdg_popup_send_popup_done(child->popup);}
 if(p->xdg)wl_resource_set_user_data(p->xdg,NULL);if(p->toplevel)wl_resource_set_user_data(p->toplevel,NULL);
 if(p->popup)wl_resource_set_user_data(p->popup,NULL);if(p->subsurface)wl_resource_set_user_data(p->subsurface,NULL);
 pending_buffer_release(p);free(p->pending_pixels);callbacks_clear(&p->callbacks);callbacks_clear(&p->pending_callbacks);
 if(p->dmabuf)wl_buffer_send_release(p->dmabuf->resource);
 if(p->texture&&p->texture_owned)glDeleteTextures(1,&p->texture);if(p->input_region)cairo_region_destroy(p->input_region);if(p->pending_input_region)cairo_region_destroy(p->pending_input_region);
 wl_list_remove(&p->link);free(p);
}
static void surface_attach(struct wl_client *c,struct wl_resource *r,struct wl_resource *buffer,int32_t x,int32_t y){
 Surface *p=wl_resource_get_user_data(r);pending_buffer_release(p);free(p->pending_pixels);p->pending_pixels=NULL;p->pending_dmabuf=NULL;p->buffer_changed=true;p->pending_null=!buffer;
 if(!buffer)return;
 DmabufBuffer *db=dmabuf_from_resource(buffer);
 if(db){p->pending_dmabuf=db;return;}
 struct wl_shm_buffer *shm=wl_shm_buffer_get(buffer);
 if(!shm){wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_SIZE,"only wl_shm and this compositor's own dma-buf imports are accepted");return;}
 int w=wl_shm_buffer_get_width(shm),h=wl_shm_buffer_get_height(shm),stride=wl_shm_buffer_get_stride(shm);uint32_t format=wl_shm_buffer_get_format(shm);
 if(w<1||h<1||w>8192||h>8192||(size_t)w*h>16777216||stride<w*4||(format!=WL_SHM_FORMAT_ARGB8888&&format!=WL_SHM_FORMAT_XRGB8888)){
  wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_SIZE,"unsupported buffer size/format (64 MiB per buffer limit)");return;
 }
 p->pending_pixels=malloc((size_t)w*h*4);if(!p->pending_pixels){wl_client_post_no_memory(c);return;}
 wl_shm_buffer_begin_access(shm);uint8_t *src=wl_shm_buffer_get_data(shm);
 for(int row=0;row<h;row++)memcpy(p->pending_pixels+(size_t)row*w*4,src+(size_t)row*stride,(size_t)w*4);
 wl_shm_buffer_end_access(shm);
 if(format==WL_SHM_FORMAT_XRGB8888)for(size_t i=3;i<(size_t)w*h*4;i+=4)p->pending_pixels[i]=255;
 p->pending_w=w;p->pending_h=h;p->pending_format=format;p->pending_buffer=buffer;p->buffer_destroy.notify=pending_buffer_gone;wl_resource_add_destroy_listener(buffer,&p->buffer_destroy);
}
static void surface_damage(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){}
static void surface_frame(struct wl_client *c,struct wl_resource *r,uint32_t id){
 /* wl_surface.frame is double-buffered state. A well-behaved client normally
  * requests it before the commit that presents its next buffer; sending done
  * here would acknowledge the previous frame and allow an unbounded render
  * loop. Keep the callback in pending_callbacks, promote it on wl_surface
  * .commit, and only complete it after that committed content was presented. */
 Surface *p=wl_resource_get_user_data(r);Callback *cb=calloc(1,sizeof(*cb));if(!cb){wl_client_post_no_memory(c);return;}
 cb->resource=wl_resource_create(c,&wl_callback_interface,1,id);if(!cb->resource){free(cb);wl_client_post_no_memory(c);return;}
 wl_resource_set_implementation(cb->resource,NULL,cb,callback_destroy);wl_list_insert(p->pending_callbacks.prev,&cb->link);
}
static void surface_opaque(struct wl_client *c,struct wl_resource *r,struct wl_resource *region){}
static void surface_input(struct wl_client *c,struct wl_resource *r,struct wl_resource *region){Surface *p=wl_resource_get_user_data(r);if(p->pending_input_region)cairo_region_destroy(p->pending_input_region);p->pending_input_region=region?cairo_region_copy(wl_resource_get_user_data(region)):NULL;p->input_changed=true;}
static void surface_transform(struct wl_client *c,struct wl_resource *r,int32_t transform){Surface *p=wl_resource_get_user_data(r);if(transform<0||transform>7){wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_TRANSFORM,"invalid transform");return;}p->pending_transform=transform;}
static void surface_scale(struct wl_client *c,struct wl_resource *r,int32_t scale){Surface *p=wl_resource_get_user_data(r);if(scale<1){wl_resource_post_error(r,WL_SURFACE_ERROR_INVALID_SCALE,"scale must be positive");return;}p->pending_scale=scale;}
static void send_surface_enter(Surface *p){Resource *o;Server *s=p->server;wl_list_for_each(o,&s->outputs,link)if(o->slot==p->slot&&wl_resource_get_client(o->resource)==wl_resource_get_client(p->resource))wl_surface_send_enter(p->resource,o->resource);}
static void surface_map(Surface *p){
 Server *s=p->server;if(p->parent){p->mapped=true;return;}if(!p->toplevel)return;
 bool cluster=strstr(p->app_id,"chromedrive.cluster")!=NULL;int slot=-1;
 if(cluster){if(!s->monitors[CD_CLUSTER_SLOT].surface)slot=CD_CLUSTER_SLOT;}
 else{
  int preferred=-1;if(strstr(p->app_id,"files"))preferred=1;else if(strstr(p->app_id,"audio"))preferred=2;else if(strstr(p->app_id,"telemetry"))preferred=3;else if(strstr(p->app_id,"foot"))preferred=0;
  if(preferred>=0&&!s->monitors[preferred].surface)slot=preferred;
  if(slot<0)for(int i=0;i<CD_APP_BAYS;i++)if(!s->monitors[i].surface){slot=i;break;}
 }
 if(slot<0){app_notice(s,cluster?"The instrument cluster is already occupied":"All eight car-part display mounts are occupied");xdg_toplevel_send_close(p->toplevel);return;}
 p->slot=slot;p->mapped=true;Monitor *m=&s->monitors[slot];m->surface=p;
 m->reveal=(slot==CD_CLUSTER_SLOT||monitor_mount(slot)==MOUNT_WINDSHIELD||monitor_mount(slot)==MOUNT_REAR_FIXED)?1:0;
 m->target_reveal=1;m->plane=monitor_plane_at(slot,m->reveal);
 send_surface_enter(p);send_configure(p);
 fprintf(stderr,"[wayland] mapped %s [%s] -> CD-%02d / %s (%dx%d)\n",p->title,p->app_id,slot+1,monitor_mount_name(slot),p->width,p->height);
 /* A passive instrument must never steal app focus merely by starting up. */
 if(!cluster){server_focus(s,p);if(s->time>3.5){scene_focus_monitor(s,slot,false);app_notice(s,"CD-%02d / %s / %s",slot+1,monitor_mount_name(slot),p->title);}}
}
static bool effective_sync(Surface *p){for(int i=0;p&&p->parent&&i<32;i++,p=p->parent)if(p->subsurface&&p->sub_sync)return true;return false;}
static void surface_apply(Surface *p){
 Server *s=p->server;
 p->scale=p->pending_scale;p->transform=p->pending_transform;
 if(p->buffer_changed){
  if(p->pending_null){
   surface_unmap(p);if(p->xdg){p->configured=false;p->configure_sent=false;}
   if(p->dmabuf){wl_buffer_send_release(p->dmabuf->resource);p->dmabuf=NULL;}
   if(p->texture&&p->texture_owned)glDeleteTextures(1,&p->texture);p->texture=0;p->texture_owned=true;
  }else if(p->pending_dmabuf){
   /* Zero-copy: reference the already GL-imported texture directly, no pixel copy. */
   if(p->texture&&p->texture_owned)glDeleteTextures(1,&p->texture);
   if(p->dmabuf&&p->dmabuf!=p->pending_dmabuf)wl_buffer_send_release(p->dmabuf->resource);
   p->dmabuf=p->pending_dmabuf;p->pending_dmabuf=NULL;p->texture=p->dmabuf->texture;p->texture_owned=false;
   p->bw=p->dmabuf->width;p->bh=p->dmabuf->height;p->format=p->dmabuf->has_alpha?WL_SHM_FORMAT_ARGB8888:WL_SHM_FORMAT_XRGB8888;
  }else if(p->pending_pixels){
   if(p->pending_w%p->scale||p->pending_h%p->scale){wl_resource_post_error(p->resource,WL_SURFACE_ERROR_INVALID_SIZE,"buffer dimensions must be divisible by scale");return;}
   if(p->dmabuf){wl_buffer_send_release(p->dmabuf->resource);p->dmabuf=NULL;}
   if(!p->texture_owned)p->texture=0;
   if(!p->texture)glGenTextures(1,&p->texture);glBindTexture(GL_TEXTURE_2D,p->texture);p->texture_owned=true;
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
   glPixelStorei(GL_UNPACK_ALIGNMENT,4);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,p->pending_w,p->pending_h,0,GL_BGRA,GL_UNSIGNED_BYTE,p->pending_pixels);
   p->bw=p->pending_w;p->bh=p->pending_h;p->format=p->pending_format;
  }
  free(p->pending_pixels);p->pending_pixels=NULL;pending_buffer_release(p);p->buffer_changed=false;
 }
 p->width=((p->transform&1)?p->bh:p->bw)/p->scale;p->height=((p->transform&1)?p->bw:p->bh)/p->scale;
 if(p->input_changed){if(p->input_region)cairo_region_destroy(p->input_region);p->input_region=p->pending_input_region;p->pending_input_region=NULL;p->input_changed=false;}
 /* Atomic pending -> committed transition: a frame callback belongs to the
  * buffer/state committed by this surface_apply, never to an arbitrary later
  * commit. This is the server half of the frame-callback pacing pattern. */
 wl_list_insert_list(p->callbacks.prev,&p->pending_callbacks);wl_list_init(&p->pending_callbacks);
 if(!p->mapped&&p->texture&&!p->pending_null)surface_map(p);
 p->pending_ready=false;
 Surface *child;wl_list_for_each(child,&s->surfaces,link)if(child->parent==p){if(child->subsurface){child->x=child->pending_x;child->y=child->pending_y;}if(child->pending_ready&&effective_sync(child))surface_apply(child);}
}
static void surface_commit(struct wl_client *c,struct wl_resource *r){
 /* xdg-shell has a stricter sequence than bare wl_surface: the initial
  * bufferless commit receives configure(size,states) + xdg_surface.configure
  * (serial); the client must ack that serial before attaching its first real
  * buffer. Posting a protocol error here is important—silently accepting an
  * unconfigured buffer leaves toolkits with contradictory surface geometry. */
 Surface *p=wl_resource_get_user_data(r);p->server->commits++;
 if(p->xdg&&!p->configure_sent){
  if(p->buffer_changed&&!p->pending_null){wl_resource_post_error(p->xdg,XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,"commit without a buffer before mapping");return;}
  send_configure(p);return;
 }
 if(p->xdg&&p->buffer_changed&&!p->pending_null&&!p->configured){wl_resource_post_error(p->xdg,XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,"ack configure before attaching a buffer");return;}
 p->pending_ready=true;if(!effective_sync(p))surface_apply(p);
}
static const struct wl_surface_interface surface_impl={.destroy=resource_destroy_request,.attach=surface_attach,.damage=surface_damage,.frame=surface_frame,.set_opaque_region=surface_opaque,.set_input_region=surface_input,.commit=surface_commit,.set_buffer_transform=surface_transform,.set_buffer_scale=surface_scale,.damage_buffer=surface_damage};
static void compositor_surface(struct wl_client *c,struct wl_resource *r,uint32_t id){
 Server *s=wl_resource_get_user_data(r);Surface *p=calloc(1,sizeof(*p));if(!p){wl_client_post_no_memory(c);return;}
 p->server=s;p->slot=-1;p->scale=p->pending_scale=1;p->sub_sync=true;p->texture_owned=true;
 wl_list_init(&p->callbacks);wl_list_init(&p->pending_callbacks);wl_list_init(&p->buffer_destroy.link);
 p->resource=wl_resource_create(c,&wl_surface_interface,wl_resource_get_version(r),id);if(!p->resource){free(p);wl_client_post_no_memory(c);return;}
 wl_resource_set_implementation(p->resource,&surface_impl,p,surface_destroyed);wl_list_insert(s->surfaces.prev,&p->link);
}
static void region_destroyed(struct wl_resource *r){cairo_region_destroy(wl_resource_get_user_data(r));}
static void region_add(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){if(w<=0||h<=0)return;cairo_rectangle_int_t rect={x,y,w,h};cairo_region_union_rectangle(wl_resource_get_user_data(r),&rect);}
static void region_sub(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){if(w<=0||h<=0)return;cairo_rectangle_int_t rect={x,y,w,h};cairo_region_subtract_rectangle(wl_resource_get_user_data(r),&rect);}
static const struct wl_region_interface region_impl={.destroy=resource_destroy_request,.add=region_add,.subtract=region_sub};
static void compositor_region(struct wl_client *c,struct wl_resource *r,uint32_t id){struct wl_resource *out=wl_resource_create(c,&wl_region_interface,1,id);if(!out){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(out,&region_impl,cairo_region_create(),region_destroyed);}
static const struct wl_compositor_interface compositor_impl={.create_surface=compositor_surface,.create_region=compositor_region};
static void bind_compositor(struct wl_client *c,void *data,uint32_t v,uint32_t id){struct wl_resource *r=wl_resource_create(c,&wl_compositor_interface,v<4?v:4,id);wl_resource_set_implementation(r,&compositor_impl,data,NULL);}

/* xdg-shell: fixed-size logical outputs projected into the cockpit. */
typedef struct {int w,h,ax,ay,aw,ah,ox,oy;uint32_t anchor,gravity,constraint;} Positioner;
static void positioner_destroyed(struct wl_resource *r){free(wl_resource_get_user_data(r));}
static void positioner_size(struct wl_client *c,struct wl_resource *r,int32_t w,int32_t h){Positioner *p=wl_resource_get_user_data(r);if(w<1||h<1){wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid size");return;}p->w=w;p->h=h;}
static void positioner_rect(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){Positioner *p=wl_resource_get_user_data(r);if(w<1||h<1){wl_resource_post_error(r,XDG_POSITIONER_ERROR_INVALID_INPUT,"invalid anchor");return;}p->ax=x;p->ay=y;p->aw=w;p->ah=h;}
static void positioner_anchor(struct wl_client *c,struct wl_resource *r,uint32_t a){Positioner *p=wl_resource_get_user_data(r);p->anchor=a;}
static void positioner_gravity(struct wl_client *c,struct wl_resource *r,uint32_t a){Positioner *p=wl_resource_get_user_data(r);p->gravity=a;}
static void positioner_constraint(struct wl_client *c,struct wl_resource *r,uint32_t a){Positioner *p=wl_resource_get_user_data(r);p->constraint=a;}
static void positioner_offset(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y){Positioner *p=wl_resource_get_user_data(r);p->ox=x;p->oy=y;}
static void positioner_reactive(struct wl_client *c,struct wl_resource *r){}
static void positioner_parent_size(struct wl_client *c,struct wl_resource *r,int32_t w,int32_t h){}
static void positioner_parent_configure(struct wl_client *c,struct wl_resource *r,uint32_t serial){}
static const struct xdg_positioner_interface positioner_impl={.destroy=resource_destroy_request,.set_size=positioner_size,.set_anchor_rect=positioner_rect,.set_anchor=positioner_anchor,.set_gravity=positioner_gravity,.set_constraint_adjustment=positioner_constraint,.set_offset=positioner_offset,.set_reactive=positioner_reactive,.set_parent_size=positioner_parent_size,.set_parent_configure=positioner_parent_configure};
static void wm_positioner(struct wl_client *c,struct wl_resource *r,uint32_t id){struct wl_resource *out=wl_resource_create(c,&xdg_positioner_interface,wl_resource_get_version(r),id);Positioner *p=calloc(1,sizeof(*p));if(!out||!p){wl_client_post_no_memory(c);free(p);return;}wl_resource_set_implementation(out,&positioner_impl,p,positioner_destroyed);}
static void send_configure(Surface *p){
 if(!p->xdg)return;Server *s=p->server;
 if(p->toplevel){struct wl_array states;wl_array_init(&states);if(s->keyboard_focus==p){uint32_t *a=wl_array_add(&states,4);if(a)*a=XDG_TOPLEVEL_STATE_ACTIVATED;}bool cluster=p->slot==CD_CLUSTER_SLOT||strstr(p->app_id,"chromedrive.cluster");xdg_toplevel_send_configure(p->toplevel,cluster?CD_CLUSTER_W:CD_APP_W,cluster?CD_CLUSTER_H:CD_APP_H,&states);wl_array_release(&states);}
 else if(p->popup)xdg_popup_send_configure(p->popup,p->x,p->y,p->popup_w,p->popup_h);
 else return;
 p->configure_serial=wl_display_next_serial(s->display);xdg_surface_send_configure(p->xdg,p->configure_serial);p->configure_sent=true;
}
static void top_destroyed(struct wl_resource *r){Surface *p=wl_resource_get_user_data(r);if(p){surface_unmap(p);p->toplevel=NULL;}}
static void top_parent(struct wl_client *c,struct wl_resource *r,struct wl_resource *parent){}
static void top_title(struct wl_client *c,struct wl_resource *r,const char *title){Surface *p=wl_resource_get_user_data(r);if(p)snprintf(p->title,sizeof(p->title),"%s",title);}
static void top_app_id(struct wl_client *c,struct wl_resource *r,const char *id){Surface *p=wl_resource_get_user_data(r);if(p){snprintf(p->app_id,sizeof(p->app_id),"%s",id);if(strstr(p->app_id,"chromedrive.cluster"))send_configure(p);}}
static void top_menu(struct wl_client *c,struct wl_resource *r,struct wl_resource *seat,uint32_t serial,int32_t x,int32_t y){}
static void top_move(struct wl_client *c,struct wl_resource *r,struct wl_resource *seat,uint32_t serial){}
static void top_resize(struct wl_client *c,struct wl_resource *r,struct wl_resource *seat,uint32_t serial,uint32_t edges){}
static void top_size(struct wl_client *c,struct wl_resource *r,int32_t w,int32_t h){}
static void top_state(struct wl_client *c,struct wl_resource *r){Surface *p=wl_resource_get_user_data(r);if(p)send_configure(p);}
static void top_fullscreen(struct wl_client *c,struct wl_resource *r,struct wl_resource *output){top_state(c,r);}
static void top_minimize(struct wl_client *c,struct wl_resource *r){}
static const struct xdg_toplevel_interface top_impl={.destroy=resource_destroy_request,.set_parent=top_parent,.set_title=top_title,.set_app_id=top_app_id,.show_window_menu=top_menu,.move=top_move,.resize=top_resize,.set_max_size=top_size,.set_min_size=top_size,.set_maximized=top_state,.unset_maximized=top_state,.set_fullscreen=top_fullscreen,.unset_fullscreen=top_state,.set_minimized=top_minimize};
static void xdg_toplevel_create(struct wl_client *c,struct wl_resource *r,uint32_t id){Surface *p=wl_resource_get_user_data(r);if(!p)return;if(p->toplevel||p->popup){wl_resource_post_error(r,XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,"role already constructed");return;}p->toplevel=wl_resource_create(c,&xdg_toplevel_interface,wl_resource_get_version(r),id);if(!p->toplevel){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(p->toplevel,&top_impl,p,top_destroyed);snprintf(p->title,sizeof(p->title),"Wayland application");}
static void popup_position(Surface *p,Positioner *pos){
 /* Positioner enums aren't bitfields: normalize their vertical/horizontal parts. */
 int a=pos->anchor,g=pos->gravity;bool al=(a==3||a==5||a==7),ar=(a==4||a==6||a==8),at=(a==1||a==5||a==6),ab=(a==2||a==7||a==8);
 bool gl=(g==3||g==5||g==7),gr=(g==4||g==6||g==8),gt=(g==1||g==5||g==6),gb=(g==2||g==7||g==8);
 p->x=pos->ax+(al?0:ar?pos->aw:pos->aw/2)+pos->ox-(gl?pos->w:gr?0:pos->w/2);
 p->y=pos->ay+(at?0:ab?pos->ah:pos->ah/2)+pos->oy-(gt?pos->h:gb?0:pos->h/2);p->popup_w=pos->w;p->popup_h=pos->h;
}
static void popup_destroyed(struct wl_resource *r){Surface *p=wl_resource_get_user_data(r);if(p){p->popup=NULL;p->mapped=false;p->parent=NULL;}}
static void popup_grab(struct wl_client *c,struct wl_resource *r,struct wl_resource *seat,uint32_t serial){}
static void popup_reposition(struct wl_client *c,struct wl_resource *r,struct wl_resource *positioner,uint32_t token){Surface *p=wl_resource_get_user_data(r);if(!p)return;popup_position(p,wl_resource_get_user_data(positioner));xdg_popup_send_repositioned(r,token);send_configure(p);}
static const struct xdg_popup_interface popup_impl={.destroy=resource_destroy_request,.grab=popup_grab,.reposition=popup_reposition};
static void xdg_popup_create(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *parent,struct wl_resource *positioner){
 Surface *p=wl_resource_get_user_data(r);if(!p)return;if(p->toplevel||p->popup){wl_resource_post_error(r,XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,"role already constructed");return;}
 if(!parent){wl_resource_post_error(r,XDG_SURFACE_ERROR_NOT_CONSTRUCTED,"popup needs an xdg parent");return;}Positioner *pos=wl_resource_get_user_data(positioner);
 if(pos->w<=0||pos->h<=0){wl_resource_post_error(r,XDG_WM_BASE_ERROR_INVALID_POSITIONER,"incomplete positioner");return;}
 p->parent=wl_resource_get_user_data(parent);popup_position(p,pos);p->popup=wl_resource_create(c,&xdg_popup_interface,wl_resource_get_version(r),id);if(!p->popup){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(p->popup,&popup_impl,p,popup_destroyed);
}
static void xdg_geometry(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h){Surface *p=wl_resource_get_user_data(r);if(!p)return;if(w<=0||h<=0){wl_resource_post_error(r,XDG_SURFACE_ERROR_INVALID_SIZE,"geometry must be positive");return;}p->gx=x;p->gy=y;p->gw=w;p->gh=h;}
static void xdg_ack(struct wl_client *c,struct wl_resource *r,uint32_t serial){Surface *p=wl_resource_get_user_data(r);if(p&&p->configure_sent)p->configured=true;}
static void xdg_destroyed(struct wl_resource *r){Surface *p=wl_resource_get_user_data(r);if(p)p->xdg=NULL;}
static const struct xdg_surface_interface xdg_impl={.destroy=resource_destroy_request,.get_toplevel=xdg_toplevel_create,.get_popup=xdg_popup_create,.set_window_geometry=xdg_geometry,.ack_configure=xdg_ack};
static void wm_surface(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *surface){
 Surface *p=wl_resource_get_user_data(surface);if(p->has_role){wl_resource_post_error(r,XDG_WM_BASE_ERROR_ROLE,"surface already has a role");return;}p->has_role=true;
 p->xdg=wl_resource_create(c,&xdg_surface_interface,wl_resource_get_version(r),id);if(!p->xdg){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(p->xdg,&xdg_impl,p,xdg_destroyed);
}
static void wm_pong(struct wl_client *c,struct wl_resource *r,uint32_t serial){}
static const struct xdg_wm_base_interface wm_impl={.destroy=resource_destroy_request,.create_positioner=wm_positioner,.get_xdg_surface=wm_surface,.pong=wm_pong};
static void bind_wm(struct wl_client *c,void *data,uint32_t v,uint32_t id){struct wl_resource *r=wl_resource_create(c,&xdg_wm_base_interface,v<3?v:3,id);wl_resource_set_implementation(r,&wm_impl,data,NULL);}

/* Synchronized subsurfaces, useful for toolkit widgets and software video. */
static void sub_destroyed(struct wl_resource *r){Surface *p=wl_resource_get_user_data(r);if(p){p->subsurface=NULL;p->parent=NULL;p->mapped=false;}}
static void sub_position(struct wl_client *c,struct wl_resource *r,int32_t x,int32_t y){Surface *p=wl_resource_get_user_data(r);if(p){p->pending_x=x;p->pending_y=y;}}
static void sub_above(struct wl_client *c,struct wl_resource *r,struct wl_resource *sibling){Surface *p=wl_resource_get_user_data(r),*q=wl_resource_get_user_data(sibling);if(!p)return;if(q==p||(!q)||!(q==p->parent||q->parent==p->parent)){wl_resource_post_error(r,WL_SUBSURFACE_ERROR_BAD_SURFACE,"not a sibling");return;}wl_list_remove(&p->link);wl_list_insert(q==p->parent?p->server->surfaces.prev:&q->link,&p->link);}
static void sub_below(struct wl_client *c,struct wl_resource *r,struct wl_resource *sibling){Surface *p=wl_resource_get_user_data(r),*q=wl_resource_get_user_data(sibling);if(!p)return;if(q==p||!q||!(q==p->parent||q->parent==p->parent)){wl_resource_post_error(r,WL_SUBSURFACE_ERROR_BAD_SURFACE,"not a sibling");return;}wl_list_remove(&p->link);wl_list_insert(q==p->parent?&p->server->surfaces:q->link.prev,&p->link);}
static void sub_sync(struct wl_client *c,struct wl_resource *r){Surface *p=wl_resource_get_user_data(r);if(p)p->sub_sync=true;}
static void sub_desync(struct wl_client *c,struct wl_resource *r){Surface *p=wl_resource_get_user_data(r);if(p){p->sub_sync=false;if(p->pending_ready&&!effective_sync(p))surface_apply(p);}}
static const struct wl_subsurface_interface sub_impl={.destroy=resource_destroy_request,.set_position=sub_position,.place_above=sub_above,.place_below=sub_below,.set_sync=sub_sync,.set_desync=sub_desync};
static void sub_get(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *surface,struct wl_resource *parent){Surface *p=wl_resource_get_user_data(surface),*q=wl_resource_get_user_data(parent);if(p==q||p->has_role||surface_root(q)==p){wl_resource_post_error(r,WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,"invalid subsurface parent or existing role");return;}p->has_role=true;p->parent=q;p->sub_sync=true;p->subsurface=wl_resource_create(c,&wl_subsurface_interface,1,id);if(!p->subsurface){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(p->subsurface,&sub_impl,p,sub_destroyed);}
static const struct wl_subcompositor_interface subcomp_impl={.destroy=resource_destroy_request,.get_subsurface=sub_get};
static void bind_subcomp(struct wl_client *c,void *data,uint32_t v,uint32_t id){struct wl_resource *r=wl_resource_create(c,&wl_subcompositor_interface,1,id);wl_resource_set_implementation(r,&subcomp_impl,data,NULL);}

/* Server-side monitor bezels are the decoration. */
static void decoration_mode(struct wl_client *c,struct wl_resource *r,uint32_t mode){zxdg_toplevel_decoration_v1_send_configure(r,ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);}
static void decoration_unset(struct wl_client *c,struct wl_resource *r){decoration_mode(c,r,0);}
static const struct zxdg_toplevel_decoration_v1_interface decoration_impl={.destroy=resource_destroy_request,.set_mode=decoration_mode,.unset_mode=decoration_unset};
static void decoration_get(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *top){struct wl_resource *out=wl_resource_create(c,&zxdg_toplevel_decoration_v1_interface,1,id);if(!out){wl_client_post_no_memory(c);return;}wl_resource_set_implementation(out,&decoration_impl,NULL,NULL);decoration_mode(c,out,0);}
static const struct zxdg_decoration_manager_v1_interface decoration_mgr_impl={.destroy=resource_destroy_request,.get_toplevel_decoration=decoration_get};
static void bind_decoration(struct wl_client *c,void *data,uint32_t v,uint32_t id){struct wl_resource *r=wl_resource_create(c,&zxdg_decoration_manager_v1_interface,1,id);wl_resource_set_implementation(r,&decoration_mgr_impl,data,NULL);}

/* Eight app mounts plus the dedicated real wl_output for the EV cluster. */
static const struct wl_output_interface output_impl={.release=resource_destroy_request};
static void bind_output(struct wl_client *c,void *data,uint32_t v,uint32_t id){
 __typeof__(((Server*)0)->output_info[0]) *info=data;Server *s=info->server;Resource *r=resource_new(c,&wl_output_interface,v<4?v:4,id,&s->outputs);if(!r)return;r->slot=info->index;
 wl_resource_set_implementation(r->resource,&output_impl,r,free_resource);bool cluster=r->slot==CD_CLUSTER_SLOT;
 wl_output_send_geometry(r->resource,r->slot*CD_APP_W,0,cluster?250:320,cluster?101:200,WL_OUTPUT_SUBPIXEL_UNKNOWN,"ChromeDrive",cluster?"Instrument cluster":"Car-part display",WL_OUTPUT_TRANSFORM_NORMAL);
 wl_output_send_mode(r->resource,WL_OUTPUT_MODE_CURRENT|WL_OUTPUT_MODE_PREFERRED,cluster?CD_CLUSTER_W:CD_APP_W,cluster?CD_CLUSTER_H:CD_APP_H,60000);
 int ver=wl_resource_get_version(r->resource);if(ver>=2)wl_output_send_scale(r->resource,1);
 if(ver>=4){char name[32],desc[80];snprintf(name,sizeof(name),"CD-%02d",r->slot+1);snprintf(desc,sizeof(desc),"ChromeDrive %02d / %s",r->slot+1,monitor_mount_name(r->slot));wl_output_send_name(r->resource,name);wl_output_send_description(r->resource,desc);}
 if(ver>=2)wl_output_send_done(r->resource);
 Surface *p;wl_list_for_each(p,&s->surfaces,link)if(p->mapped&&p->slot==r->slot&&wl_resource_get_client(p->resource)==c)wl_surface_send_enter(p->resource,r->resource);
}

/* Seat. SDL supplies physical scancodes; xkbcommon supplies a real keymap. */
static void pointer_cursor(struct wl_client *c,struct wl_resource *r,uint32_t serial,struct wl_resource *surface,int32_t x,int32_t y){if(surface){Surface *p=wl_resource_get_user_data(surface);if(!p->has_role||p->is_cursor){p->has_role=true;p->is_cursor=true;}}}
static const struct wl_pointer_interface pointer_impl={.set_cursor=pointer_cursor,.release=resource_destroy_request};
static const struct wl_keyboard_interface keyboard_impl={.release=resource_destroy_request};
static const struct wl_touch_interface touch_impl={.release=resource_destroy_request};
static void pointer_frame(struct wl_resource *r){if(wl_resource_get_version(r)>=5)wl_pointer_send_frame(r);}
static void send_key_enter(Server *s,struct wl_resource *r,Surface *p){struct wl_array keys;wl_array_init(&keys);for(uint32_t i=0;i<256;i++)if(s->keys[i]){uint32_t *k=wl_array_add(&keys,4);if(k)*k=i;}wl_keyboard_send_enter(r,wl_display_next_serial(s->display),p->resource,&keys);wl_array_release(&keys);}
static void send_modifiers(Server *s,struct wl_resource *r){wl_keyboard_send_modifiers(r,wl_display_next_serial(s->display),xkb_state_serialize_mods(s->xkb_state,XKB_STATE_MODS_DEPRESSED),xkb_state_serialize_mods(s->xkb_state,XKB_STATE_MODS_LATCHED),xkb_state_serialize_mods(s->xkb_state,XKB_STATE_MODS_LOCKED),xkb_state_serialize_layout(s->xkb_state,XKB_STATE_LAYOUT_EFFECTIVE));}
static void seat_pointer(struct wl_client *c,struct wl_resource *r,uint32_t id){Server *s=wl_resource_get_user_data(r);Resource *p=resource_new(c,&wl_pointer_interface,wl_resource_get_version(r),id,&s->pointers);if(!p)return;wl_resource_set_implementation(p->resource,&pointer_impl,p,free_resource);if(s->pointer_focus&&wl_resource_get_client(s->pointer_focus->resource)==c){wl_pointer_send_enter(p->resource,wl_display_next_serial(s->display),s->pointer_focus->resource,wl_fixed_from_double(s->pointer_sx),wl_fixed_from_double(s->pointer_sy));pointer_frame(p->resource);}}
static void seat_keyboard(struct wl_client *c,struct wl_resource *r,uint32_t id){Server *s=wl_resource_get_user_data(r);Resource *p=resource_new(c,&wl_keyboard_interface,wl_resource_get_version(r),id,&s->keyboards);if(!p)return;wl_resource_set_implementation(p->resource,&keyboard_impl,p,free_resource);wl_keyboard_send_keymap(p->resource,WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,s->keymap_fd,s->keymap_size);if(wl_resource_get_version(p->resource)>=4)wl_keyboard_send_repeat_info(p->resource,30,500);if(s->keyboard_focus&&wl_resource_get_client(s->keyboard_focus->resource)==c){send_key_enter(s,p->resource,s->keyboard_focus);send_modifiers(s,p->resource);}}
static void seat_touch(struct wl_client *c,struct wl_resource *r,uint32_t id){struct wl_resource *p=wl_resource_create(c,&wl_touch_interface,wl_resource_get_version(r),id);wl_resource_set_implementation(p,&touch_impl,NULL,NULL);}
static const struct wl_seat_interface seat_impl={.get_pointer=seat_pointer,.get_keyboard=seat_keyboard,.get_touch=seat_touch,.release=resource_destroy_request};
static void bind_seat(struct wl_client *c,void *data,uint32_t v,uint32_t id){struct wl_resource *r=wl_resource_create(c,&wl_seat_interface,v<7?v:7,id);wl_resource_set_implementation(r,&seat_impl,data,NULL);wl_seat_send_capabilities(r,WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);if(wl_resource_get_version(r)>=2)wl_seat_send_name(r,"chromedrive-driver");}
void server_focus(Server *s,Surface *p){
 p=surface_root(p);if(p==s->keyboard_focus)return;Surface *old=s->keyboard_focus;Resource *r;
 if(old)wl_list_for_each(r,&s->keyboards,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(old->resource))wl_keyboard_send_leave(r->resource,wl_display_next_serial(s->display),old->resource);
 s->keyboard_focus=p;if(old&&old->toplevel)send_configure(old);
 if(p){s->active_slot=p->slot;wl_list_for_each(r,&s->keyboards,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(p->resource)){send_key_enter(s,r->resource,p);send_modifiers(s,r->resource);}send_configure(p);selection_for_client(s,wl_resource_get_client(p->resource));}
}
void server_pointer_motion(Server *s,Hit hit,uint32_t time){
 Surface *next=hit.kind==HIT_APP?hit.surface:NULL;Resource *r;uint32_t serial=wl_display_next_serial(s->display);
 if(next!=s->pointer_focus){
  if(s->pointer_focus)wl_list_for_each(r,&s->pointers,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(s->pointer_focus->resource)){wl_pointer_send_leave(r->resource,serial,s->pointer_focus->resource);pointer_frame(r->resource);}
  s->pointer_focus=next;
  if(next)wl_list_for_each(r,&s->pointers,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(next->resource)){wl_pointer_send_enter(r->resource,serial,next->resource,wl_fixed_from_double(hit.x),wl_fixed_from_double(hit.y));pointer_frame(r->resource);}
 }
 s->pointer_sx=hit.x;s->pointer_sy=hit.y;
 if(next)wl_list_for_each(r,&s->pointers,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(next->resource)){wl_pointer_send_motion(r->resource,time,wl_fixed_from_double(hit.x),wl_fixed_from_double(hit.y));pointer_frame(r->resource);}
}
void server_pointer_button(Server *s,uint32_t button,bool down,uint32_t time){
 Surface *p=s->pointer_grab?s->pointer_grab:s->pointer_focus;if(!p)return;
 if(down){if(!s->held_buttons)s->pointer_grab=p;s->held_buttons++;server_focus(s,p);}
 Resource *r;uint32_t serial=wl_display_next_serial(s->display);wl_list_for_each(r,&s->pointers,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(p->resource)){wl_pointer_send_button(r->resource,serial,time,button,down?WL_POINTER_BUTTON_STATE_PRESSED:WL_POINTER_BUTTON_STATE_RELEASED);pointer_frame(r->resource);}
 if(!down){if(s->held_buttons>0)s->held_buttons--;if(!s->held_buttons)s->pointer_grab=NULL;}
}
void server_pointer_axis(Server *s,double dx,double dy,uint32_t time){
 if(!s->pointer_focus)return;Resource *r;wl_list_for_each(r,&s->pointers,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(s->pointer_focus->resource)){
  int ver=wl_resource_get_version(r->resource);if(ver>=5)wl_pointer_send_axis_source(r->resource,WL_POINTER_AXIS_SOURCE_WHEEL);
  if(dy){if(ver>=5)wl_pointer_send_axis_discrete(r->resource,WL_POINTER_AXIS_VERTICAL_SCROLL,(int)-dy);wl_pointer_send_axis(r->resource,time,WL_POINTER_AXIS_VERTICAL_SCROLL,wl_fixed_from_double(-dy*15));}
  if(dx){if(ver>=5)wl_pointer_send_axis_discrete(r->resource,WL_POINTER_AXIS_HORIZONTAL_SCROLL,(int)-dx);wl_pointer_send_axis(r->resource,time,WL_POINTER_AXIS_HORIZONTAL_SCROLL,wl_fixed_from_double(-dx*15));}pointer_frame(r->resource);
 }
}
void server_key(Server *s,uint32_t key,bool down,uint32_t time){
 if(key>=256)return;s->keys[key]=down;xkb_state_update_key(s->xkb_state,key+8,down?XKB_KEY_DOWN:XKB_KEY_UP);if(!s->keyboard_focus)return;
 Resource *r;uint32_t serial=wl_display_next_serial(s->display);wl_list_for_each(r,&s->keyboards,link)if(wl_resource_get_client(r->resource)==wl_resource_get_client(s->keyboard_focus->resource)){wl_keyboard_send_key(r->resource,serial,time,key,down?WL_KEYBOARD_KEY_STATE_PRESSED:WL_KEYBOARD_KEY_STATE_RELEASED);send_modifiers(s,r->resource);}
}
void server_release_keys(Server *s,uint32_t time){for(uint32_t i=0;i<256;i++)if(s->keys[i])server_key(s,i,false,time);}
void server_close_surface(Surface *p){p=surface_root(p);if(p&&p->toplevel)xdg_toplevel_send_close(p->toplevel);}

/* In-compositor clipboard. No host clipboard access. Drag-and-drop not supported. */
typedef struct {struct wl_list link;struct wl_resource *resource;DataSource *source;} DataOffer;
struct DataSource {struct wl_resource *resource;Server *server;char *mimes[64];int count;struct wl_list offers;};
static void data_offer_destroyed(struct wl_resource *r){DataOffer *o=wl_resource_get_user_data(r);if(o){wl_list_remove(&o->link);free(o);}}
static void offer_accept(struct wl_client *c,struct wl_resource *r,uint32_t serial,const char *mime){DataOffer *o=wl_resource_get_user_data(r);if(o->source)wl_data_source_send_target(o->source->resource,mime);}
static void offer_receive(struct wl_client *c,struct wl_resource *r,const char *mime,int32_t fd){DataOffer *o=wl_resource_get_user_data(r);if(o->source)wl_data_source_send_send(o->source->resource,mime,fd);close(fd);}
static void offer_finish(struct wl_client *c,struct wl_resource *r){}
static void offer_actions(struct wl_client *c,struct wl_resource *r,uint32_t actions,uint32_t preferred){}
static const struct wl_data_offer_interface offer_impl={.accept=offer_accept,.receive=offer_receive,.destroy=resource_destroy_request,.finish=offer_finish,.set_actions=offer_actions};
static void selection_for_client(Server *s,struct wl_client *client){
 Resource *d;wl_list_for_each(d,&s->data_devices,link)if(wl_resource_get_client(d->resource)==client){
  if(!s->selection){wl_data_device_send_selection(d->resource,NULL);continue;}
  DataOffer *o=calloc(1,sizeof(*o));if(!o){wl_client_post_no_memory(client);continue;}o->source=s->selection;o->resource=wl_resource_create(client,&wl_data_offer_interface,wl_resource_get_version(d->resource),0);if(!o->resource){free(o);wl_client_post_no_memory(client);continue;}
  wl_list_insert(&o->source->offers,&o->link);wl_resource_set_implementation(o->resource,&offer_impl,o,data_offer_destroyed);wl_data_device_send_data_offer(d->resource,o->resource);
  for(int i=0;i<o->source->count;i++)wl_data_offer_send_offer(o->resource,o->source->mimes[i]);wl_data_device_send_selection(d->resource,o->resource);
 }
}
static void source_destroyed(struct wl_resource *r){DataSource *d=wl_resource_get_user_data(r);if(!d)return;DataOffer *o,*tmp;wl_list_for_each_safe(o,tmp,&d->offers,link){o->source=NULL;wl_list_remove(&o->link);wl_list_init(&o->link);}if(d->server->selection==d){d->server->selection=NULL;if(d->server->keyboard_focus)selection_for_client(d->server,wl_resource_get_client(d->server->keyboard_focus->resource));}for(int i=0;i<d->count;i++)free(d->mimes[i]);free(d);}
static void source_offer(struct wl_client *c,struct wl_resource *r,const char *mime){DataSource *d=wl_resource_get_user_data(r);if(d->count<64)d->mimes[d->count++]=strdup(mime);}
static void source_actions(struct wl_client *c,struct wl_resource *r,uint32_t actions){}
static const struct wl_data_source_interface source_impl={.offer=source_offer,.destroy=resource_destroy_request,.set_actions=source_actions};
static void data_source_create(struct wl_client *c,struct wl_resource *r,uint32_t id){DataSource *d=calloc(1,sizeof(*d));if(!d){wl_client_post_no_memory(c);return;}d->server=wl_resource_get_user_data(r);d->resource=wl_resource_create(c,&wl_data_source_interface,wl_resource_get_version(r),id);if(!d->resource){free(d);wl_client_post_no_memory(c);return;}wl_list_init(&d->offers);wl_resource_set_implementation(d->resource,&source_impl,d,source_destroyed);}
static void data_drag(struct wl_client *c,struct wl_resource *r,struct wl_resource *source,struct wl_resource *origin,struct wl_resource *icon,uint32_t serial){if(source)wl_data_source_send_cancelled(source);}
static void data_selection(struct wl_client *c,struct wl_resource *r,struct wl_resource *source,uint32_t serial){Resource *dev=wl_resource_get_user_data(r);(void)dev;Server *s=NULL;/* Find server through the source or a small owner wrapper below. */
 /* wl_data_device user data is an extended Resource, first member preserved. */
 typedef struct {Resource base;Server *server;} Device;s=((Device*)dev)->server;
 DataSource *next=source?wl_resource_get_user_data(source):NULL;if(s->selection&&s->selection!=next)wl_data_source_send_cancelled(s->selection->resource);s->selection=next;if(s->keyboard_focus)selection_for_client(s,wl_resource_get_client(s->keyboard_focus->resource));
}
static const struct wl_data_device_interface device_impl={.start_drag=data_drag,.set_selection=data_selection,.release=resource_destroy_request};
static void data_device_create(struct wl_client *c,struct wl_resource *r,uint32_t id,struct wl_resource *seat){
 typedef struct {Resource base;Server *server;} Device;Server *s=wl_resource_get_user_data(r);Device *d=calloc(1,sizeof(*d));if(!d){wl_client_post_no_memory(c);return;}d->server=s;d->base.resource=wl_resource_create(c,&wl_data_device_interface,wl_resource_get_version(r),id);if(!d->base.resource){free(d);wl_client_post_no_memory(c);return;}wl_list_insert(&s->data_devices,&d->base.link);wl_resource_set_implementation(d->base.resource,&device_impl,d,free_resource);if(s->keyboard_focus&&wl_resource_get_client(s->keyboard_focus->resource)==c)selection_for_client(s,c);
}
static const struct wl_data_device_manager_interface data_mgr_impl={.create_data_source=data_source_create,.get_data_device=data_device_create};
static void bind_data(struct wl_client *c,void *data,uint32_t v,uint32_t id){struct wl_resource *r=wl_resource_create(c,&wl_data_device_manager_interface,v<3?v:3,id);wl_resource_set_implementation(r,&data_mgr_impl,data,NULL);}

static void each_surface_recursive(Surface *p,int x,int y,int depth,void (*fn)(Surface*,int,int,void*),void *data){if(depth>32)return;if(p->mapped&&p->texture)fn(p,x,y,data);Surface *child;wl_list_for_each(child,&p->server->surfaces,link)if(child->parent==p&&child->mapped)each_surface_recursive(child,x+child->x,y+child->y,depth+1,fn,data);}
void server_each_surface(Surface *root,void (*fn)(Surface*,int,int,void*),void *data){if(root)each_surface_recursive(root,0,0,0,fn,data);}
static Surface *surface_at_rec(Surface *root,float x,float y,float *sx,float *sy,int depth){if(depth>32)return NULL;Surface *child;wl_list_for_each_reverse(child,&root->server->surfaces,link)if(child->parent==root&&child->mapped){Surface *hit=surface_at_rec(child,x-child->x,y-child->y,sx,sy,depth+1);if(hit)return hit;}if(root->mapped&&x>=0&&y>=0&&x<root->width&&y<root->height&&(!root->input_region||cairo_region_contains_point(root->input_region,(int)x,(int)y))){*sx=x;*sy=y;return root;}return NULL;}
Surface *surface_at(Surface *root,float x,float y,float *sx,float *sy){return root?surface_at_rec(root,x,y,sx,sy,0):NULL;}
void server_frame_done(Server *s,uint32_t time){
 /* main calls this after scene_render + backend present/readback, then flushes
  * client sockets. done carries the compositor clock (not a Unix timestamp)
  * and the callback resource is one-shot, so destroy it immediately after the
  * event. Clients can now request exactly one callback for their next frame;
  * this is backpressure, not a timer that tries to guess their render rate. */
 Surface *p;wl_list_for_each(p,&s->surfaces,link){Callback *cb,*tmp;wl_list_for_each_safe(cb,tmp,&p->callbacks,link){wl_callback_send_done(cb->resource,time);wl_resource_destroy(cb->resource);}}
}
bool server_init(Server *s,const char *socket_name){
 wl_list_init(&s->surfaces);wl_list_init(&s->pointers);wl_list_init(&s->keyboards);wl_list_init(&s->outputs);wl_list_init(&s->data_devices);
 s->display=wl_display_create();if(!s->display)return false;s->loop=wl_display_get_event_loop(s->display);
 if(wl_display_init_shm(s->display)<0)return false;
 wl_global_create(s->display,&wl_compositor_interface,4,s,bind_compositor);wl_global_create(s->display,&wl_subcompositor_interface,1,s,bind_subcomp);
 wl_global_create(s->display,&xdg_wm_base_interface,3,s,bind_wm);wl_global_create(s->display,&zxdg_decoration_manager_v1_interface,1,s,bind_decoration);
 wl_global_create(s->display,&wl_seat_interface,7,s,bind_seat);wl_global_create(s->display,&wl_data_device_manager_interface,3,s,bind_data);
 dmabuf_register_global(s); /* no-op (skipped) unless dmabuf_probe() already found a usable EGL dma-buf import path */
 for(int i=0;i<CD_MAX_MONITORS;i++){
  s->output_info[i].server=s;s->output_info[i].index=i;s->output_globals[i]=wl_global_create(s->display,&wl_output_interface,4,&s->output_info[i],bind_output);
  MonitorMount mount=monitor_mount(i);bool sliding=mount==MOUNT_DASH_SLIDE||mount==MOUNT_DOOR_SLIDE||mount==MOUNT_ROOF_FLIP;
  s->monitors[i].plane=monitor_plane(i);s->monitors[i].reveal=s->monitors[i].target_reveal=sliding?0:1;
 }
 s->xkb_context=xkb_context_new(XKB_CONTEXT_NO_FLAGS);struct xkb_rule_names names={.layout=getenv("XKB_DEFAULT_LAYOUT"),.variant=getenv("XKB_DEFAULT_VARIANT"),.options=getenv("XKB_DEFAULT_OPTIONS")};
 s->keymap=xkb_keymap_new_from_names(s->xkb_context,&names,XKB_KEYMAP_COMPILE_NO_FLAGS);if(!s->keymap)return false;s->xkb_state=xkb_state_new(s->keymap);
 char *keymap=xkb_keymap_get_as_string(s->keymap,XKB_KEYMAP_FORMAT_TEXT_V1);if(!keymap)return false;s->keymap_size=strlen(keymap)+1;s->keymap_fd=memfd_create("chromedrive-keymap",MFD_CLOEXEC);if(s->keymap_fd<0||write(s->keymap_fd,keymap,s->keymap_size)!=(ssize_t)s->keymap_size){free(keymap);return false;}free(keymap);
 if(socket_name){if(wl_display_add_socket(s->display,socket_name)<0)return false;snprintf(s->socket,sizeof(s->socket),"%s",socket_name);}else{const char *name=wl_display_add_socket_auto(s->display);if(!name)return false;snprintf(s->socket,sizeof(s->socket),"%s",name);}
 setenv("WAYLAND_DISPLAY",s->socket,1);fprintf(stderr,"\nChromeDrive %s / native Wayland socket: %s\nXDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s\n\n",CD_VERSION,s->socket,getenv("XDG_RUNTIME_DIR"),s->socket);return true;
}
void server_dispatch(Server *s){wl_event_loop_dispatch(s->loop,0);wl_display_flush_clients(s->display);}
void server_finish(Server *s){if(s->display){wl_display_destroy_clients(s->display);wl_display_destroy(s->display);}if(s->xkb_state)xkb_state_unref(s->xkb_state);if(s->keymap)xkb_keymap_unref(s->keymap);if(s->xkb_context)xkb_context_unref(s->xkb_context);if(s->keymap_fd>=0)close(s->keymap_fd);}
uint32_t sdl_to_evdev(SDL_Scancode code){
 static const uint16_t map[SDL_NUM_SCANCODES]={
 [SDL_SCANCODE_A]=KEY_A,[SDL_SCANCODE_B]=KEY_B,[SDL_SCANCODE_C]=KEY_C,[SDL_SCANCODE_D]=KEY_D,[SDL_SCANCODE_E]=KEY_E,[SDL_SCANCODE_F]=KEY_F,[SDL_SCANCODE_G]=KEY_G,[SDL_SCANCODE_H]=KEY_H,[SDL_SCANCODE_I]=KEY_I,[SDL_SCANCODE_J]=KEY_J,[SDL_SCANCODE_K]=KEY_K,[SDL_SCANCODE_L]=KEY_L,[SDL_SCANCODE_M]=KEY_M,[SDL_SCANCODE_N]=KEY_N,[SDL_SCANCODE_O]=KEY_O,[SDL_SCANCODE_P]=KEY_P,[SDL_SCANCODE_Q]=KEY_Q,[SDL_SCANCODE_R]=KEY_R,[SDL_SCANCODE_S]=KEY_S,[SDL_SCANCODE_T]=KEY_T,[SDL_SCANCODE_U]=KEY_U,[SDL_SCANCODE_V]=KEY_V,[SDL_SCANCODE_W]=KEY_W,[SDL_SCANCODE_X]=KEY_X,[SDL_SCANCODE_Y]=KEY_Y,[SDL_SCANCODE_Z]=KEY_Z,
 [SDL_SCANCODE_1]=KEY_1,[SDL_SCANCODE_2]=KEY_2,[SDL_SCANCODE_3]=KEY_3,[SDL_SCANCODE_4]=KEY_4,[SDL_SCANCODE_5]=KEY_5,[SDL_SCANCODE_6]=KEY_6,[SDL_SCANCODE_7]=KEY_7,[SDL_SCANCODE_8]=KEY_8,[SDL_SCANCODE_9]=KEY_9,[SDL_SCANCODE_0]=KEY_0,
 [SDL_SCANCODE_RETURN]=KEY_ENTER,[SDL_SCANCODE_ESCAPE]=KEY_ESC,[SDL_SCANCODE_BACKSPACE]=KEY_BACKSPACE,[SDL_SCANCODE_TAB]=KEY_TAB,[SDL_SCANCODE_SPACE]=KEY_SPACE,[SDL_SCANCODE_MINUS]=KEY_MINUS,[SDL_SCANCODE_EQUALS]=KEY_EQUAL,[SDL_SCANCODE_LEFTBRACKET]=KEY_LEFTBRACE,[SDL_SCANCODE_RIGHTBRACKET]=KEY_RIGHTBRACE,[SDL_SCANCODE_BACKSLASH]=KEY_BACKSLASH,[SDL_SCANCODE_NONUSHASH]=KEY_102ND,[SDL_SCANCODE_SEMICOLON]=KEY_SEMICOLON,[SDL_SCANCODE_APOSTROPHE]=KEY_APOSTROPHE,[SDL_SCANCODE_GRAVE]=KEY_GRAVE,[SDL_SCANCODE_COMMA]=KEY_COMMA,[SDL_SCANCODE_PERIOD]=KEY_DOT,[SDL_SCANCODE_SLASH]=KEY_SLASH,[SDL_SCANCODE_CAPSLOCK]=KEY_CAPSLOCK,
 [SDL_SCANCODE_F1]=KEY_F1,[SDL_SCANCODE_F2]=KEY_F2,[SDL_SCANCODE_F3]=KEY_F3,[SDL_SCANCODE_F4]=KEY_F4,[SDL_SCANCODE_F5]=KEY_F5,[SDL_SCANCODE_F6]=KEY_F6,[SDL_SCANCODE_F7]=KEY_F7,[SDL_SCANCODE_F8]=KEY_F8,[SDL_SCANCODE_F9]=KEY_F9,[SDL_SCANCODE_F10]=KEY_F10,[SDL_SCANCODE_F11]=KEY_F11,[SDL_SCANCODE_F12]=KEY_F12,
 [SDL_SCANCODE_PRINTSCREEN]=KEY_SYSRQ,[SDL_SCANCODE_SCROLLLOCK]=KEY_SCROLLLOCK,[SDL_SCANCODE_PAUSE]=KEY_PAUSE,[SDL_SCANCODE_INSERT]=KEY_INSERT,[SDL_SCANCODE_HOME]=KEY_HOME,[SDL_SCANCODE_PAGEUP]=KEY_PAGEUP,[SDL_SCANCODE_DELETE]=KEY_DELETE,[SDL_SCANCODE_END]=KEY_END,[SDL_SCANCODE_PAGEDOWN]=KEY_PAGEDOWN,[SDL_SCANCODE_RIGHT]=KEY_RIGHT,[SDL_SCANCODE_LEFT]=KEY_LEFT,[SDL_SCANCODE_DOWN]=KEY_DOWN,[SDL_SCANCODE_UP]=KEY_UP,
 [SDL_SCANCODE_NUMLOCKCLEAR]=KEY_NUMLOCK,[SDL_SCANCODE_KP_DIVIDE]=KEY_KPSLASH,[SDL_SCANCODE_KP_MULTIPLY]=KEY_KPASTERISK,[SDL_SCANCODE_KP_MINUS]=KEY_KPMINUS,[SDL_SCANCODE_KP_PLUS]=KEY_KPPLUS,[SDL_SCANCODE_KP_ENTER]=KEY_KPENTER,[SDL_SCANCODE_KP_0]=KEY_KP0,[SDL_SCANCODE_KP_1]=KEY_KP1,[SDL_SCANCODE_KP_2]=KEY_KP2,[SDL_SCANCODE_KP_3]=KEY_KP3,[SDL_SCANCODE_KP_4]=KEY_KP4,[SDL_SCANCODE_KP_5]=KEY_KP5,[SDL_SCANCODE_KP_6]=KEY_KP6,[SDL_SCANCODE_KP_7]=KEY_KP7,[SDL_SCANCODE_KP_8]=KEY_KP8,[SDL_SCANCODE_KP_9]=KEY_KP9,[SDL_SCANCODE_KP_PERIOD]=KEY_KPDOT,[SDL_SCANCODE_APPLICATION]=KEY_COMPOSE,
 [SDL_SCANCODE_LCTRL]=KEY_LEFTCTRL,[SDL_SCANCODE_LSHIFT]=KEY_LEFTSHIFT,[SDL_SCANCODE_LALT]=KEY_LEFTALT,[SDL_SCANCODE_LGUI]=KEY_LEFTMETA,[SDL_SCANCODE_RCTRL]=KEY_RIGHTCTRL,[SDL_SCANCODE_RSHIFT]=KEY_RIGHTSHIFT,[SDL_SCANCODE_RALT]=KEY_RIGHTALT,[SDL_SCANCODE_RGUI]=KEY_RIGHTMETA};
 return code<SDL_NUM_SCANCODES?map[code]:0;
}
