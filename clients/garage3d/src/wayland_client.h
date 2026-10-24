/*
 * wayland_client.h - raw libwayland-client + EGL platform layer.
 *
 * This is the ONLY module that speaks the Wayland protocol. It owns:
 *   - wl_display connection and registry binding
 *       (wl_compositor, xdg_wm_base, wl_seat, wl_shm, wl_output)
 *   - the wl_surface -> xdg_surface -> xdg_toplevel role stack
 *   - the wl_egl_window and the EGL display/config/context/surface on it
 *   - frame-callback bookkeeping (the vsync throttle)
 *   - pointer / keyboard (xkbcommon) / touch input, translated into the
 *     platform-neutral InputState in input.h
 *
 * Rendering itself (GL calls) lives in renderer.c; presenting a finished
 * frame is wlclient_present(), because with EGL-on-Wayland the
 * wl_surface.attach + commit happens inside eglSwapBuffers (Mesa's Wayland
 * EGL platform owns the buffer lifecycle) - the client must NOT attach or
 * commit manually in this mode. It only adds a wl_surface.frame request
 * before the swap so the request rides along with that commit.
 */
#ifndef GARAGE3D_WAYLAND_CLIENT_H
#define GARAGE3D_WAYLAND_CLIENT_H

#include "input.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client.h"

typedef struct {
    const char *title;
    const char *app_id;
    int width, height;     /* initial logical size request */
    int scale_override;    /* 0 = follow output, >=1 = force buffer scale */
    bool start_fullscreen;
} AppConfig;

typedef struct WlClient WlClient;

typedef struct {
    struct wl_output *output;
    uint32_t global_name;
    int scale;
    WlClient *client;
} WlOutputInfo;

typedef struct {
    bool active;
    int32_t id;
    double x, y;           /* current position (surface-local) */
    double prev_x, prev_y; /* position at the previous touch frame */
    double start_x, start_y;
    double down_time;      /* monotonic seconds (tap detection) */
} TouchPoint;

#define MAX_TOUCH_POINTS 10
#define MAX_OUTPUTS 8

struct WlClient {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    uint32_t seat_global_name;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;
    WlOutputInfo *outputs[MAX_OUTPUTS];
    int output_count;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;

    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;

    struct wl_callback *frame_callback;
    bool configured_once;  /* first xdg_surface.configure acked */
    bool frame_ready;      /* no frame callback outstanding: may draw */
    bool resize_pending;   /* deferred wl_egl_window_resize (see below) */
    bool running;          /* cleared by xdg_toplevel.close / fatal errors */
    bool fullscreen;       /* last compositor-confirmed state */
    uint32_t last_frame_time_ms; /* compositor clock from frame.done */

    /* Sizes: xdg_toplevel.configure speaks in logical (window-geometry)
     * pixels; the EGL window and GL viewport use physical buffer pixels
     * (logical * buffer_scale). Keeping both is what makes HiDPI work. */
    int logical_w, logical_h;
    int pending_logical_w, pending_logical_h;
    int scale;             /* applied buffer scale */
    int desired_scale;     /* from the current output (or --scale) */
    int scale_override;

    /* Cursor (wayland-cursor theme; the surface is a plain wl_surface that
     * gets the cursor role via wl_pointer.set_cursor). */
    struct wl_cursor_theme *cursor_theme;
    struct wl_surface *cursor_surface;

    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    bool keys[256];        /* held evdev codes (physical positions) */

    /* Pointer state; wl_pointer.frame (seat v5+) groups a motion+axis
     * burst, so deltas accumulate here and commit atomically on frame. */
    bool pointer_inside;
    double pointer_x, pointer_y;
    double pending_pointer_dx, pending_pointer_dy;
    double pending_axis_v, pending_axis_h;
    int pending_axis_discrete_v;
    uint32_t pointer_buttons; /* bit0 left, bit1 right, bit2 middle */

    TouchPoint touch_points[MAX_TOUCH_POINTS];
    bool pinch_valid;
    double pinch_centroid_x, pinch_centroid_y, pinch_distance;
    double last_tap_time, last_tap_x, last_tap_y;

    InputState input;
};

/* Monotonic clock (seconds). Used for delta-time and tap timing; the
 * timestamps inside Wayland events use an undefined compositor base and
 * must not be mixed with physics time. */
double wlclient_now(void);

/* Connects, binds globals, performs the initial empty commit and blocks
 * until the first xdg_surface.configure has been acked, and *only then*
 * creates the wl_egl_window/EGL surface at the configured size. Attaching a
 * buffer before that first ack is a protocol error
 * (xdg_surface.unconfigured_buffer) - the ordering here is the whole point. */
bool wlclient_init(WlClient *c, const AppConfig *cfg);
void wlclient_shutdown(WlClient *c);

/* The correct single-threaded dispatch dance:
 *   prepare_read -> flush -> poll(fd) -> read_events -> dispatch_pending
 * prepare_read/flush bracketing is what prevents a race where events arrive
 * between "check the queue" and "sleep in poll" and stall a frame.
 * Returns 0 on success, -1 on a fatal display error. */
int wlclient_dispatch(WlClient *c, int timeout_ms);

/* True when the compositor is ready for a new frame: configured, no frame
 * callback outstanding, non-zero size. This is the vsync gate - the main
 * loop renders only when it is true and blocks in dispatch otherwise, which
 * is how 60 fps throttling works without any sleep()/timer. */
bool wlclient_can_draw(const WlClient *c);

/* Applies a deferred resize (called by the main loop after dispatch, before
 * drawing): wl_egl_window_resize + wl_surface_set_buffer_scale. Deferred
 * because doing EGL work inside the configure callback would re-enter
 * driver code from inside libwayland's dispatch loop. */
void wlclient_apply_pending_resize(WlClient *c);

/* Requests the next frame callback, then eglSwapBuffers (which attaches and
 * commits the rendered buffer). Requesting *before* the swap makes the
 * callback observe the frame just presented; requesting after would attach
 * it to some future commit and can stall the loop by one frame. */
bool wlclient_present(WlClient *c);

void wlclient_toggle_fullscreen(WlClient *c);

static inline int wlclient_phys_width(const WlClient *c) { return c->logical_w * c->scale; }
static inline int wlclient_phys_height(const WlClient *c) { return c->logical_h * c->scale; }

#endif
