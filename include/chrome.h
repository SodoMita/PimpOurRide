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
#include "vehicle.h"
#include "telemetry.h"
/* Cockpit backdrop (src/scene.c environment()), selectable in
 * chromedrive.conf / the config GUI: the real 3D garage (default), the
 * original flat garage.webp poster, or nothing at all. */
#define CD_BG_3D 0
#define CD_BG_TEXTURE 1
#define CD_BG_NONE 2
/* Startup display backend. HEADLESS and FBDEV share the surfaceless-EGL/FBO
 * renderer; WINDOWED is the nested SDL2/GLX development window. */
typedef enum { CD_BACKEND_HEADLESS=0,CD_BACKEND_WINDOWED=1,CD_BACKEND_FBDEV=2 } Backend;
#define CD_APP_BAYS 8
#define CD_CLUSTER_SLOT 8
#define CD_MAX_MONITORS 9 /* eight app mounts + one fixed instrument cluster */
#define CD_APP_W 1000
#define CD_APP_H 625
#define CD_CLUSTER_W 1000
#define CD_CLUSTER_H 404
#define CD_VERSION "0.3.0"
#define CD_DMABUF_MAX_FORMATS 8
#define CD_DMABUF_MAX_MODIFIERS 16
#include <linux/input.h>
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
 int win_w,win_h,min_w,min_h,max_w,max_h; /* last requested toplevel size + client hints (0 = unset) */
 int gx,gy,gw,gh,popup_w,popup_h;
 struct wl_resource *pending_buffer;
 struct wl_listener buffer_destroy;
 uint32_t configure_serial,format,pending_format;
 cairo_region_t *input_region,*pending_input_region;
 bool input_changed;
 char title[256],app_id[256];
};
typedef struct { Plane plane;Surface *surface;GLuint label;char label_text[300];float reveal,target_reveal;bool custom_transform; } Monitor;
/* One Launch Control entry. name/sub/icon/enabled are user-configurable
 * (chromedrive.conf [app.*], edited by bin/chromedrive-config) and applied
 * live: DashButton owns copies, ui_refresh_buttons() re-renders textures.
 * `action` says what a click does: launch the slot's assigned app, or run
 * one of the two built-in cockpit actions. Any physical slot may hold any
 * of the three; slots 5/6 merely default to the built-ins. */
enum { CD_BUTTON_APP=0, CD_BUTTON_NEON=1, CD_BUTTON_COCKPIT=2 };
typedef struct { Plane plane;char name[48],sub[64];int action,icon;bool enabled;GLuint texture; } DashButton;
typedef enum {HIT_NONE,HIT_APP,HIT_BUTTON,HIT_CLOSE,HIT_COG,HIT_QUIT,HIT_WHEEL,HIT_FRAME,HIT_HANDLES_BUTTON,HIT_ARRANGE_HANDLE} HitKind;
/* Arrange gizmo handles, in a monitor's local plane coordinates. Resize
 * IDs are ordered clockwise from north-west so scene.c/main.c can share
 * the same geometry table without another mapping layer. */
enum { CD_HANDLE_NONE=0,CD_HANDLE_MOVE,CD_HANDLE_AXIS_X,CD_HANDLE_AXIS_Y,CD_HANDLE_AXIS_Z,
 CD_HANDLE_ROTATE_YAW,CD_HANDLE_ROTATE_PITCH,CD_HANDLE_ROTATE_ROLL,CD_HANDLE_SCALE,
 CD_HANDLE_RESIZE_NW,CD_HANDLE_RESIZE_N,CD_HANDLE_RESIZE_NE,CD_HANDLE_RESIZE_E,
 CD_HANDLE_RESIZE_SE,CD_HANDLE_RESIZE_S,CD_HANDLE_RESIZE_SW,CD_HANDLE_RESIZE_W };
typedef struct { HitKind kind;Surface *surface;int slot,button,handle;float x,y,distance; } Hit;
typedef struct { uint32_t format;uint64_t modifiers[CD_DMABUF_MAX_MODIFIERS];int modifier_count; } DmabufFormat;
#define CD_MAX_TOUCH_POINTS 10
/* One active wl_touch contact. The surface is the exact child surface that
 * received wl_touch.down; motion/up keep going there (implicit grab) until
 * up/cancel, with coordinates converted through scene_pick_surface(). */
typedef struct { bool active;int32_t id;Surface *surface;float x,y; } CdTouchPoint;
/* SDL backend finger routing: app contacts become wl_touch points, while a
 * single primary finger may drive cockpit chrome as the left pointer. */
enum { CD_TOUCH_ROUTE_NONE=0,CD_TOUCH_ROUTE_APP,CD_TOUCH_ROUTE_CHROME,
 CD_TOUCH_ROUTE_POINTER,CD_TOUCH_ROUTE_WHEEL,CD_TOUCH_ROUTE_LOOK };
typedef struct { bool active;int64_t device_id,finger_id;int32_t id;int route,last_x,last_y; } SdlTouchFinger;
struct Server {
 struct wl_display *display;struct wl_event_loop *loop;
 struct wl_list surfaces,pointers,keyboards,touch_resources,outputs,data_devices;
 struct wl_global *output_globals[CD_MAX_MONITORS];
 struct { Server *server;int index; } output_info[CD_MAX_MONITORS];
 struct xkb_context *xkb_context;struct xkb_keymap *keymap;struct xkb_state *xkb_state;
 int keymap_fd;uint32_t keymap_size;bool keys[256];
 Surface *keyboard_focus,*pointer_focus,*pointer_grab;int held_buttons;uint32_t held_button_mask;
 float pointer_sx,pointer_sy;int mouse_x,mouse_y;
 bool touch_available;CdTouchPoint touch_points[CD_MAX_TOUCH_POINTS];
SdlTouchFinger sdl_touch[CD_MAX_TOUCH_POINTS];int sdl_touch_chrome_id;
  int evdev_chrome_touch_id;
  Monitor monitors[CD_MAX_MONITORS];DashButton buttons[6];
 /* The settings cog: a slowly spinning 3D gear medallioned into the sloped
  * dash between wheel and console (scene.c draw_cog). Clicking it opens
  * bin/chromedrive-config. Picked by ray against cog's plane + radial test,
  * like the buttons. */
 Plane cog;bool cog_hover,wheel_hover;float cog_angle;double cog_spin_until;
 /* Red EXIT button on the same sloped dash as the cog. The first click
  * only arms it (quit_armed_until) so an accidental hit cannot end a session;
  * a second click within that window quits. Meta+Shift+Q bypasses the guard. */
 Plane quit_button;bool quit_hover;double quit_armed_until;
 DataSource *selection;
 /* Backend: exactly one of these is used, chosen at startup. */
 Backend backend;
 SDL_Window *window;SDL_GLContext gl;                 /* windowed/dev backend (X11 via SDL2, GLX) */
 EGLDisplay egl_display;EGLContext egl_context;        /* EGL backends (surfaceless EGL, no X) */
 GLuint target_fbo,fbo_color_rb,fbo_depth_rb;          /* EGL render target; 0 (default) when windowed */
 uint8_t *readback_pixels;                             /* shared single GL_RGBA readback scratch for EGL present */
 void *vnc_screen;int vnc_port;                        /* rfbScreenInfoPtr, opaque here (see backend_vnc.c) */
 int vnc_prev_mask,vnc_prev_x,vnc_prev_y;              /* RFB pointer edge-detection state (no separate down/up events in the protocol) */
 char fbdev_path[256];bool fbdev_vsync,fbdev_pan,fbdev_vnc_mirror,fbdev_modeset,fbdev_evdev,fbdev_grab_input;int fbdev_mode_width,fbdev_mode_height,fbdev_mode_bpp;void *fbdev_state;char fbdev_input_devices[8][256];int fbdev_input_device_count;void *evdev_state;
 bool dmabuf_supported;int dmabuf_format_count;DmabufFormat dmabuf_formats[CD_DMABUF_MAX_FORMATS];
 int width,height,fps_limit,theme,hover_button,hover_close,active_slot;
 int bg_mode; /* CD_BG_*: which environment() backdrop to draw */
 bool running,middle_look,focus_view,show_help,hud_visible,fullscreen,demo,launch_pending;
 /* F6 Arrange mode: session-only whole-monitor translate/rotate/scale.
  * The effective transform lives in monitors[slot].plane and is reset when
  * a new top-level maps into that bay. Mount animation yields to this custom
  * plane instead of overwriting the user's arrangement every frame. */
 bool arrange_mode,arrange_dragging,arrange_handles;int arrange_slot,arrange_drag_button,arrange_last_x,arrange_last_y;
 int arrange_handle,arrange_start_x,arrange_start_y;Plane arrange_start_plane;int arrange_start_win_w,arrange_start_win_h;float arrange_start_local_x,arrange_start_local_y;
 /* KWin-like titlebar/frame drag outside Arrange mode: just translation,
  * without making the whole input path modal. */
 bool frame_dragging;int frame_drag_slot,frame_last_x,frame_last_y;
 float yaw,pitch,target_yaw,target_pitch,fov,target_fov;
 Vec3 eye;
 /* Native electric hover-car rig and input routing. In CLIENT mode drive keys
  * continue to Wayland clients and the wheel emits pointer-axis input; in
  * DRIVE mode the same physical controls are consumed by vehicle.c instead. */
 Vehicle vehicle;
 VehicleSteeringMode steering_mode;
 VehicleCameraMode camera_mode;
 VehicleSeat camera_seat;
 bool drive_forward,drive_reverse,drive_left,drive_right,drive_brake;
 bool door_shortcut_down,autopilot_shortcut_down,quit_shortcut_down;
 bool wheel_drag;
 float wheel_steer,wheel_client_angle,wheel_axis_accum,wheel_last_x,wheel_last_y;
 double time,dt,fps,start_time,last_ui_time,last_windshield_time,notice_until;
 int telemetry_fd;CdTelemetry *telemetry;
 uint64_t frames,commits;
 char socket[128],root[4096],runtime[4096],notice[256];
 char *launch_commands[6];char *exec_commands[16];int exec_count;
 /* Launch-menu configuration per Launch Control slot (see [app.*] in
  * chromedrive.conf). Every slot may launch an app: app_ids[s] is the
  * substring matched against a mapped surface's app_id so re-clicking
  * focuses the running app; app_autostart[s] joins the --demo set. */
 char app_ids[6][128];bool app_autostart[6];
 pid_t children[128];int child_count;
 GLuint garage_tex,carbon_tex,console_tex,hub_tex,hud_tex,windshield_tex,help_tex,plate_tex,handles_tex;
 GLuint scene_list;bool rebuild_scene;
 /* Cached real city geometry and destination signs. Dynamic traffic/rain are
  * rendered separately from the static display list. */
 GLuint city_list,city_signs[4];
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
bool server_touch_down(Server *s,Hit hit,int32_t id,uint32_t time);
bool server_touch_motion(Server *s,int32_t id,float x,float y,uint32_t time);
bool server_touch_up(Server *s,int32_t id,uint32_t time);
void server_touch_cancel(Server *s);
void server_key(Server *s,uint32_t key,bool down,uint32_t time);
void server_release_keys(Server *s,uint32_t time);
void server_focus(Server *s,Surface *surface);
void server_close_surface(Surface *surface);
bool server_request_window_size(Surface *surface,int width,int height,int *out_w,int *out_h);
Surface *surface_root(Surface *s);
Surface *surface_at(Surface *root,float x,float y,float *sx,float *sy);
void server_each_surface(Surface *root,void (*fn)(Surface*,int,int,void*),void *data);
uint32_t sdl_to_evdev(SDL_Scancode code);
void scene_init(Server *s);
void city_render(Server *s);
void scene_render(Server *s);
void scene_update_mounts(Server *s);
void scene_finish(Server *s);
Hit scene_pick(Server *s,float x,float y,bool grab);
Hit scene_pick_surface(Server *s,Surface *surface,float x,float y);
void scene_focus_monitor(Server *s,int slot,bool zoom);
void scene_monitor_reset(Server *s,int slot);
void scene_monitor_translate(Server *s,int slot,float right,float up,float toward_eye);
void scene_monitor_rotate(Server *s,int slot,float yaw,float pitch,float roll);
void scene_monitor_scale(Server *s,int slot,float factor);
void scene_monitor_set_plane(Server *s,int slot,Plane plane);
void scene_reset_camera(Server *s);
bool scene_wheel_button(Server *s,Hit hit,int x,int y,bool down,uint32_t time);
void scene_wheel_motion(Server *s,int x,int y,uint32_t time);
void scene_wheel_cancel(Server *s);
void scene_save_screenshot(Server *s,const char *path);
void scene_neon_color(Server *s,float *r,float *g,float *b);
void vehicle_render(Server *s,bool cutaway);
void vehicle_render_glass(Server *s);
void app_release_input(Server *s,uint32_t time);
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
void app_request_quit(Server *s,bool via_hotkey);
void app_exec(Server *s,const char *command);
void app_notice(Server *s,const char *fmt,...);
bool app_global_key(Server *s,uint32_t evdev,bool down);
bool app_arrange_pointer_button(Server *s,Hit hit,uint32_t button,bool down);
void app_arrange_pointer_motion(Server *s,int x,int y);
bool app_arrange_pointer_wheel(Server *s,float steps);
bool app_frame_pointer_button(Server *s,Hit hit,uint32_t button,bool down);
void app_frame_pointer_motion(Server *s,int x,int y);
/* telemetry.c: shared live EV state for the real Wayland instrument client. */
bool telemetry_init(Server *s);
void telemetry_update(Server *s);
void telemetry_finish(Server *s);
/* dmabuf.c: zwp_linux_dmabuf_v1, real EGLImage-backed zero-copy GPU import. */
void dmabuf_probe(Server *s);
void dmabuf_register_global(Server *s);
bool dmabuf_is_buffer(struct wl_resource *buffer);
DmabufBuffer *dmabuf_from_resource(struct wl_resource *buffer);
uint32_t keysym_to_evdev(uint32_t keysym);
/* backend_egl.c: shared surfaceless-EGL/FBO setup for headless and fbdev. */
bool backend_egl_init(Server *s,const char *tag);
void backend_egl_shutdown(Server *s);
/* backend_sdl.c: windowed/dev backend — SDL2 window + GLX context on an X server. */
bool backend_sdl_init(Server *s);
void backend_sdl_poll(Server *s);
void backend_sdl_present(Server *s);
void backend_sdl_shutdown(Server *s);
/* backend_vnc.c: embedded RFB server used by headless and the fbdev mirror. */
bool backend_vnc_init(Server *s,bool loopback_only);
void backend_vnc_poll(Server *s);
void backend_vnc_present_readback(Server *s);
void backend_vnc_shutdown(Server *s);
/* backend_headless.c: default backend — surfaceless EGL + embedded VNC server, no X at all. */
bool backend_headless_init(Server *s);
void backend_headless_poll(Server *s);
void backend_headless_present(Server *s);
void backend_headless_shutdown(Server *s);
/* backend_fbdev.c: direct /dev/fb* output through the shared EGL/FBO renderer. */
bool backend_fbdev_init(Server *s);
void backend_fbdev_poll(Server *s);
void backend_fbdev_present(Server *s);
void backend_fbdev_shutdown(Server *s);
/* backend_evdev.c: raw evdev input for fbdev backend (keyboard, pointer, touch). */
bool backend_evdev_init(Server *s);
void backend_evdev_poll(Server *s);
void backend_evdev_shutdown(Server *s);

#define EVDEV_MAX_DEVICES 8
#define EVDEV_MAX_SLOTS 16
typedef enum {
    EVDEV_DEV_NONE = 0,
    EVDEV_DEV_KEYBOARD,
    EVDEV_DEV_REL_POINTER,
    EVDEV_DEV_ABS_POINTER,
    EVDEV_DEV_MULTITOUCH,
    EVDEV_DEV_SINGLE_TOUCH
} EvdevDevType;
typedef struct {
    int tracking_id;
    int x, y;
    bool active;
    int touch_id;
    int route;
    int last_x, last_y;
    bool is_chrome_owner;
} EvdevFinger;
typedef struct {
    int fd;
    char path[256];
    char name[256];
    EvdevDevType type;
    bool grabbed;
    bool has_rel_x, has_rel_y;
    bool has_abs_x, has_abs_y;
    bool has_keys;
    struct input_absinfo abs_x, abs_y;
    struct input_absinfo abs_mt_x, abs_mt_y;
    bool is_direct;
    EvdevFinger mt_fingers[EVDEV_MAX_SLOTS];
    EvdevFinger st_finger;
    int current_slot;
    int rel_accum_x, rel_accum_y;
    int abs_x_val, abs_y_val;
    int wheel_accum, hwheel_accum;
    uint32_t held_button_mask;
    uint32_t sent_button_mask;
    bool btn_left, btn_right, btn_middle, btn_side, btn_extra;
    bool middle_look_held;
} EvdevDevice;
typedef struct {
    EvdevDevice devices[EVDEV_MAX_DEVICES];
    int device_count;
    bool grab_input;
    char explicit_paths[EVDEV_MAX_DEVICES][256];
    int explicit_count;
    bool evdev_enabled;
} EvdevState;
#endif
