#ifndef CHROMEDRIVE_H
#define CHROMEDRIVE_H
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon.h>
#include "math3d.h"
/* Cockpit backdrop (src/scene.c environment()), selectable in
 * chromedrive.conf / the config GUI: the real 3D garage (default), the
 * original flat garage.webp poster, or nothing at all. */
#define CD_BG_3D 0
#define CD_BG_TEXTURE 1
#define CD_BG_NONE 2
#define CD_MAX_MONITORS 8
#define CD_APP_W 1000
#define CD_APP_H 625
#define CD_VERSION "0.2.0"
#define CD_DMABUF_MAX_FORMATS 8
#define CD_DMABUF_MAX_MODIFIERS 16
typedef struct Server Server;
typedef struct Surface Surface;
typedef struct DataSource DataSource;
/* A GL texture imported zero-copy from a client's dma-buf via EGLImage.
 * Real import code path; see dmabuf.c. Requires EGL_EXT_image_dma_buf_import,
 * which this sandbox's software (llvmpipe) EGL driver happens to expose even
 * without a GPU — but producing an actual dma-buf fd needs a DRM device or
 * /dev/udmabuf, neither of which exists in this sandbox, so the success path
 * can only be exercised on real hardware. The failure/negotiation paths are
 * fully exercised by tests/protocol_probe.c. */
typedef struct DmabufBuffer {
 struct wl_resource *resource;
 Server *server;
 GLuint texture;
 EGLImageKHR image;
 int width,height;
 bool has_alpha;
} DmabufBuffer;
typedef struct { struct wl_list link;struct wl_resource *resource;int slot; } Resource;
typedef struct { struct wl_list link;struct wl_resource *resource; } Callback;
struct Surface {
 struct wl_list link,callbacks,pending_callbacks;
 Server *server;
 struct wl_resource *resource,*xdg,*toplevel,*popup,*subsurface;
 Surface *parent;
 int x,y,pending_x,pending_y,slot;
 bool mapped,configured,configure_sent,buffer_changed,pending_ready,sub_sync,has_role,is_cursor;
 bool pending_null;
 GLuint texture;
 bool texture_owned;
 DmabufBuffer *dmabuf,*pending_dmabuf;
 uint8_t *pending_pixels;
 int bw,bh,width,height,scale,transform,pending_w,pending_h,pending_scale,pending_transform;
 int gx,gy,gw,gh,popup_w,popup_h;
 struct wl_resource *pending_buffer;
 struct wl_listener buffer_destroy;
 uint32_t configure_serial,format,pending_format;
 cairo_region_t *input_region,*pending_input_region;
 bool input_changed;
 char title[256],app_id[256];
};
typedef struct { Plane plane;Surface *surface;GLuint label;char label_text[300]; } Monitor;
/* One Launch Control entry. name/sub/icon/enabled are user-configurable
 * (chromedrive.conf [app.*], edited by bin/chromedrive-config) and applied
 * live: DashButton owns copies, ui_refresh_buttons() re-renders textures.
 * `action` says what a click does: launch the slot's assigned app, or run
 * one of the two built-in cockpit actions. Any physical slot may hold any
 * of the three; slots 5/6 merely default to the built-ins. */
enum { CD_BUTTON_APP=0, CD_BUTTON_NEON=1, CD_BUTTON_COCKPIT=2 };
typedef struct { Plane plane;char name[48],sub[64];int action,icon;bool enabled;GLuint texture; } DashButton;
typedef enum {HIT_NONE,HIT_APP,HIT_BUTTON,HIT_CLOSE,HIT_COG,HIT_FRAME,HIT_HANDLES_BUTTON,HIT_ARRANGE_HANDLE} HitKind;
/* Arrange gizmo handles, in a monitor's local plane coordinates. Resize
 * IDs are ordered clockwise from north-west so scene.c/main.c can share
 * the same geometry table without another mapping layer. */
enum { CD_HANDLE_NONE=0,CD_HANDLE_MOVE,CD_HANDLE_AXIS_X,CD_HANDLE_AXIS_Y,CD_HANDLE_AXIS_Z,
 CD_HANDLE_ROTATE_YAW,CD_HANDLE_ROTATE_PITCH,CD_HANDLE_ROTATE_ROLL,CD_HANDLE_SCALE,
 CD_HANDLE_RESIZE_NW,CD_HANDLE_RESIZE_N,CD_HANDLE_RESIZE_NE,CD_HANDLE_RESIZE_E,
 CD_HANDLE_RESIZE_SE,CD_HANDLE_RESIZE_S,CD_HANDLE_RESIZE_SW,CD_HANDLE_RESIZE_W };
typedef struct { HitKind kind;Surface *surface;int slot,button,handle;float x,y,distance; } Hit;
typedef struct { uint32_t format;uint64_t modifiers[CD_DMABUF_MAX_MODIFIERS];int modifier_count; } DmabufFormat;
struct Server {
 struct wl_display *display;struct wl_event_loop *loop;
 struct wl_list surfaces,pointers,keyboards,outputs,data_devices;
 struct wl_global *output_globals[CD_MAX_MONITORS];
 struct { Server *server;int index; } output_info[CD_MAX_MONITORS];
 struct xkb_context *xkb_context;struct xkb_keymap *keymap;struct xkb_state *xkb_state;
 int keymap_fd;uint32_t keymap_size;bool keys[256];
 Surface *keyboard_focus,*pointer_focus,*pointer_grab;int held_buttons;uint32_t held_button_mask;
 float pointer_sx,pointer_sy;int mouse_x,mouse_y;
 Monitor monitors[CD_MAX_MONITORS];DashButton buttons[6];
 /* The settings cog: a slowly spinning 3D gear medallioned into the sloped
  * dash between wheel and console (scene.c draw_cog). Clicking it opens
  * bin/chromedrive-config. Picked by ray against cog's plane + radial test,
  * like the buttons. */
 Plane cog;bool cog_hover;float cog_angle;double cog_spin_until;
 DataSource *selection;
 /* Backend: exactly one of these is used, chosen at startup by --headless (default) or --windowed. */
 bool headless;
 SDL_Window *window;SDL_GLContext gl;                 /* windowed/dev backend (X11 via SDL2, GLX) */
 EGLDisplay egl_display;EGLContext egl_context;        /* headless backend (surfaceless EGL, no X) */
 GLuint target_fbo,fbo_color_rb,fbo_depth_rb;          /* headless render target; 0 (default) when windowed */
 void *vnc_screen;int vnc_port;uint8_t *vnc_pixels;    /* rfbScreenInfoPtr, opaque here (see backend_headless.c) */
 int vnc_prev_mask,vnc_prev_x,vnc_prev_y;              /* RFB pointer edge-detection state (no separate down/up events in the protocol) */
 bool dmabuf_supported;int dmabuf_format_count;DmabufFormat dmabuf_formats[CD_DMABUF_MAX_FORMATS];
 int width,height,fps_limit,theme,hover_button,active_slot;
 int bg_mode; /* CD_BG_*: which environment() backdrop to draw */
 bool running,middle_look,focus_view,show_help,hud_visible,fullscreen,demo,launch_pending;
 /* F5 Arrange mode: session-only whole-monitor translate/rotate/scale.
  * The effective transform lives in monitors[slot].plane and is reset when
  * a new top-level maps into that bay. */
 bool arrange_mode,arrange_dragging,arrange_handles;int arrange_slot,arrange_drag_button,arrange_last_x,arrange_last_y;
 int arrange_handle,arrange_start_x,arrange_start_y;Plane arrange_start_plane;float arrange_start_local_x,arrange_start_local_y;
 /* KWin-like titlebar/frame drag outside Arrange mode: just translation,
  * without making the whole input path modal. */
 bool frame_dragging;int frame_drag_slot,frame_last_x,frame_last_y;
 float yaw,pitch,target_yaw,target_pitch,fov,target_fov;
 Vec3 eye;
 double time,dt,fps,start_time,last_ui_time,notice_until;
 uint64_t frames,commits;
 char socket[128],root[4096],runtime[4096],notice[256];
 char *launch_commands[6];char *exec_commands[16];int exec_count;
 /* Launch-menu configuration per Launch Control slot (see [app.*] in
  * chromedrive.conf). Every slot may launch an app: app_ids[s] is the
  * substring matched against a mapped surface's app_id so re-clicking
  * focuses the running app; app_autostart[s] joins the --demo set. */
 char app_ids[6][128];bool app_autostart[6];
 pid_t children[128];int child_count;
 GLuint garage_tex,carbon_tex,gauge_tex,console_tex,hub_tex,hud_tex,help_tex,plate_tex,handles_tex;
 GLuint scene_list;bool rebuild_scene;
 bool screenshot_requested;char screenshot_path[4096];
};
bool server_init(Server *s,const char *socket_name);
void server_dispatch(Server *s);
void server_finish(Server *s);
void server_frame_done(Server *s,uint32_t time);
void server_pointer_motion(Server *s,Hit hit,uint32_t time);
void server_pointer_button(Server *s,uint32_t button,bool down,uint32_t time);
void server_release_pointer_buttons(Server *s,uint32_t time);
void server_pointer_axis(Server *s,double dx,double dy,uint32_t time);
void server_key(Server *s,uint32_t key,bool down,uint32_t time);
void server_release_keys(Server *s,uint32_t time);
void server_focus(Server *s,Surface *surface);
void server_close_surface(Surface *surface);
Surface *surface_root(Surface *s);
Surface *surface_at(Surface *root,float x,float y,float *sx,float *sy);
void server_each_surface(Surface *root,void (*fn)(Surface*,int,int,void*),void *data);
uint32_t sdl_to_evdev(SDL_Scancode code);
void scene_init(Server *s);
void scene_render(Server *s);
void scene_finish(Server *s);
Hit scene_pick(Server *s,float x,float y,bool grab);
void scene_focus_monitor(Server *s,int slot,bool zoom);
void scene_monitor_reset(Server *s,int slot);
void scene_monitor_translate(Server *s,int slot,float right,float up,float toward_eye);
void scene_monitor_rotate(Server *s,int slot,float yaw,float pitch,float roll);
void scene_monitor_scale(Server *s,int slot,float factor);
void scene_monitor_set_plane(Server *s,int slot,Plane plane);
void scene_reset_camera(Server *s);
void scene_save_screenshot(Server *s,const char *path);
void scene_neon_color(Server *s,float *r,float *g,float *b);
GLuint ui_texture(cairo_surface_t *surface,GLuint reuse);
GLuint ui_load_webp(const char *path);
void ui_init(Server *s);
/* Re-renders the six Launch Control button textures from current
 * name/sub/icon/enabled - called after a config (re)load changes the menu. */
void ui_refresh_buttons(Server *s);
void ui_update(Server *s);
void ui_monitor_label(Server *s,int slot);
void ui_draw_overlay(Server *s);
void app_launch(Server *s,int action,bool force_new);
void app_open_settings(Server *s);
void app_open_settings_slot(Server *s,int slot,bool choose_app);
void app_exec(Server *s,const char *command);
void app_notice(Server *s,const char *fmt,...);
bool app_global_key(Server *s,uint32_t evdev,bool down);
bool app_arrange_pointer_button(Server *s,Hit hit,uint32_t button,bool down);
void app_arrange_pointer_motion(Server *s,int x,int y);
bool app_arrange_pointer_wheel(Server *s,float steps);
bool app_frame_pointer_button(Server *s,Hit hit,uint32_t button,bool down);
void app_frame_pointer_motion(Server *s,int x,int y);
/* dmabuf.c: zwp_linux_dmabuf_v1, real EGLImage-backed zero-copy GPU import. */
void dmabuf_probe(Server *s);
void dmabuf_register_global(Server *s);
bool dmabuf_is_buffer(struct wl_resource *buffer);
DmabufBuffer *dmabuf_from_resource(struct wl_resource *buffer);
uint32_t keysym_to_evdev(uint32_t keysym);
/* backend_sdl.c: windowed/dev backend — SDL2 window + GLX context on an X server. */
bool backend_sdl_init(Server *s);
void backend_sdl_poll(Server *s);
void backend_sdl_present(Server *s);
void backend_sdl_shutdown(Server *s);
/* backend_headless.c: default backend — surfaceless EGL + embedded VNC server, no X at all. */
bool backend_headless_init(Server *s);
void backend_headless_poll(Server *s);
void backend_headless_present(Server *s);
void backend_headless_shutdown(Server *s);
#endif
