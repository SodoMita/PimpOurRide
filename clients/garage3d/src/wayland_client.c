/* wayland_client.c - raw Wayland + EGL platform. See wayland_client.h. */
#define _GNU_SOURCE
#include "wayland_client.h"
#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

double wlclient_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void mark_activity(WlClient *c) {
    c->input.last_activity = wlclient_now();
    c->input.any_activity = true;
}

/* Release helpers: wl_*_release requests only exist from certain interface
 * versions; sending one on an older proxy is a protocol error, so fall back
 * to destroying the local proxy (the compositor reaps the object when the
 * client disconnects or the seat capability goes away). */
static void release_pointer(WlClient *c) {
    if (!c->pointer) return;
    if (wl_pointer_get_version(c->pointer) >= 3) wl_pointer_release(c->pointer);
    else wl_proxy_destroy((struct wl_proxy *)c->pointer);
    c->pointer = NULL;
}
static void release_keyboard(WlClient *c) {
    if (!c->keyboard) return;
    if (wl_keyboard_get_version(c->keyboard) >= 3) wl_keyboard_release(c->keyboard);
    else wl_proxy_destroy((struct wl_proxy *)c->keyboard);
    c->keyboard = NULL;
}
static void release_touch(WlClient *c) {
    if (!c->touch) return;
    if (wl_touch_get_version(c->touch) >= 3) wl_touch_release(c->touch);
    else wl_proxy_destroy((struct wl_proxy *)c->touch);
    c->touch = NULL;
}

/* ----------------------------------------------------------------- cursor */

/* On wl_pointer.enter the pointer image is undefined until the client sets
 * one - there is no server-side default cursor in Wayland. We load the
 * system cursor theme once (wayland-cursor, over wl_shm) and hand the
 * compositor a dedicated cursor surface. ChromeDrive draws its own reticle
 * and simply never displays this surface, which is equally valid. */
static void update_cursor(WlClient *c, uint32_t serial) {
    if (!c->pointer || !c->shm || !c->compositor) return;
    if (!c->cursor_theme) {
        int size = 24 * (c->scale > 0 ? c->scale : 1);
        c->cursor_theme = wl_cursor_theme_load(NULL, size, c->shm);
        if (!c->cursor_theme) return;
    }
    struct wl_cursor *cursor = wl_cursor_theme_get_cursor(c->cursor_theme, "default");
    if (!cursor) cursor = wl_cursor_theme_get_cursor(c->cursor_theme, "left_ptr");
    if (!cursor || cursor->image_count == 0) return;
    struct wl_cursor_image *image = cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;
    if (!c->cursor_surface) c->cursor_surface = wl_compositor_create_surface(c->compositor);
    if (!c->cursor_surface) return;
    if (wl_surface_get_version(c->cursor_surface) >= 3 && c->scale > 0)
        wl_surface_set_buffer_scale(c->cursor_surface, c->scale);
    wl_surface_attach(c->cursor_surface, buffer, 0, 0);
    if (wl_surface_get_version(c->cursor_surface) >= 4)
        wl_surface_damage_buffer(c->cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    else
        wl_surface_damage(c->cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(c->cursor_surface);
    wl_pointer_set_cursor(c->pointer, serial, c->cursor_surface,
                          (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
}

/* ---------------------------------------------------------------- pointer */

/* Commit one grouped pointer frame (or one event on pre-v5 seats): turn the
 * accumulated motion/axis into camera deltas exactly once. Axis handling
 * prefers axis_discrete (whole wheel clicks) over the continuous value when
 * both are present - they describe the same motion, and adding both would
 * double-zoom, the classic Wayland scroll bug. */
static void commit_pointer_frame(WlClient *c) {
    if (c->pending_pointer_dx != 0.0 || c->pending_pointer_dy != 0.0) {
        if (c->pointer_buttons & 1u) {
            c->input.orbit_dx += (float)c->pending_pointer_dx;
            c->input.orbit_dy += (float)c->pending_pointer_dy;
        } else if (c->pointer_buttons & 6u) {
            c->input.pan_dx += (float)c->pending_pointer_dx;
            c->input.pan_dy += (float)c->pending_pointer_dy;
        }
        c->pending_pointer_dx = c->pending_pointer_dy = 0.0;
    }
    if (c->pending_axis_discrete_v != 0) {
        c->input.zoom_delta += (float)c->pending_axis_discrete_v;
        c->pending_axis_discrete_v = 0;
        c->pending_axis_v = 0.0; /* same motion, do not count twice */
    } else if (c->pending_axis_v != 0.0) {
        c->input.zoom_delta += (float)(c->pending_axis_v / 15.0);
        c->pending_axis_v = 0.0;
    }
    c->pending_axis_h = 0.0;
}
static bool pointer_grouped(WlClient *c) {
    return c->pointer && wl_pointer_get_version(c->pointer) >= 5;
}
static void maybe_commit(WlClient *c) { if (!pointer_grouped(c)) commit_pointer_frame(c); }

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    WlClient *c = data;
    (void)pointer; (void)surface;
    c->pointer_inside = true;
    c->pointer_x = wl_fixed_to_double(x);
    c->pointer_y = wl_fixed_to_double(y);
    c->pending_pointer_dx = c->pending_pointer_dy = 0.0;
    update_cursor(c, serial);
    mark_activity(c);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    WlClient *c = data;
    (void)pointer; (void)serial; (void)surface;
    commit_pointer_frame(c);
    c->pointer_inside = false;
    c->pointer_buttons = 0;
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y) {
    WlClient *c = data;
    (void)pointer; (void)time;
    double nx = wl_fixed_to_double(x), ny = wl_fixed_to_double(y);
    if (c->pointer_inside) {
        c->pending_pointer_dx += nx - c->pointer_x;
        c->pending_pointer_dy += ny - c->pointer_y;
    }
    c->pointer_x = nx; c->pointer_y = ny;
    c->pointer_inside = true;
    mark_activity(c);
    maybe_commit(c);
}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    WlClient *c = data;
    (void)pointer; (void)serial; (void)time;
    uint32_t bit = button == BTN_LEFT ? 1u : button == BTN_RIGHT ? 2u : button == BTN_MIDDLE ? 4u : 0u;
    if (!bit) return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) c->pointer_buttons |= bit;
    else c->pointer_buttons &= ~bit;
    mark_activity(c);
    maybe_commit(c);
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    WlClient *c = data;
    (void)pointer; (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) c->pending_axis_v += wl_fixed_to_double(value);
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) c->pending_axis_h += wl_fixed_to_double(value);
    mark_activity(c);
    maybe_commit(c);
}
static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)pointer;
    commit_pointer_frame((WlClient *)data);
}
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
    (void)data; (void)pointer; (void)source; /* wheel vs finger: same zoom here */
}
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    WlClient *c = data;
    (void)pointer;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) c->pending_axis_discrete_v += discrete;
    mark_activity(c);
    maybe_commit(c);
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* ---------------------------------------------------------------- keyboard */

static bool key_held(WlClient *c, unsigned code) { return code < 256 && c->keys[code]; }

/* Recompute held-state axes from the physical (evdev) key bitmap. Physical
 * positions (not layout letters) are used for WASD so the hand position is
 * the same on AZERTY/Dvorak; one-shot commands below use keysyms, which do
 * follow the printed labels. State-based axes are also why no key-repeat
 * timer is needed: movement speed comes from dt, not repeat events. */
static void sync_keyboard_axes(WlClient *c) {
    c->input.move_forward =
        (key_held(c, KEY_W) || key_held(c, KEY_UP) ? 1.0f : 0.0f) -
        (key_held(c, KEY_S) || key_held(c, KEY_DOWN) ? 1.0f : 0.0f);
    c->input.move_strafe =
        (key_held(c, KEY_D) || key_held(c, KEY_RIGHT) ? 1.0f : 0.0f) -
        (key_held(c, KEY_A) || key_held(c, KEY_LEFT) ? 1.0f : 0.0f);
    c->input.move_vertical =
        (key_held(c, KEY_E) || key_held(c, KEY_PAGEUP) ? 1.0f : 0.0f) -
        (key_held(c, KEY_Q) || key_held(c, KEY_PAGEDOWN) ? 1.0f : 0.0f);
    c->input.fov_delta =
        (key_held(c, KEY_EQUAL) || key_held(c, KEY_KPPLUS) ? 1.0f : 0.0f) -
        (key_held(c, KEY_MINUS) || key_held(c, KEY_KPMINUS) ? 1.0f : 0.0f);
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size) {
    WlClient *c = data;
    (void)keyboard;
    /* The fd is ours now and must be closed on every path - leaking it per
     * focus change is a classic slow fd-exhaustion bug. Non-xkb formats are
     * legal on paper; in practice every compositor sends XKB_V1, and we fall
     * back to raw evdev codes for the few commands we need if not. */
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0 || !c->xkb_context) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;
    /* new_from_buffer (not _from_string): a compositor supplies the size, so
     * don't trust a NUL terminator. size-1 strips the terminator the
     * protocol does include when present at the end. */
    size_t len = size;
    if (len > 0 && map[len - 1] == '\0') len--;
    struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(
        c->xkb_context, map, len, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map, size);
    if (!keymap) return;
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state) { xkb_keymap_unref(keymap); return; }
    if (c->xkb_state) xkb_state_unref(c->xkb_state);
    if (c->xkb_keymap) xkb_keymap_unref(c->xkb_keymap);
    c->xkb_keymap = keymap;
    c->xkb_state = state;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) {
    WlClient *c = data;
    (void)keyboard; (void)serial; (void)surface;
    /* The enter array lists keys already held when focus arrived; syncing
     * from it prevents "stuck" or missed movement after Alt-Tab. */
    memset(c->keys, 0, sizeof(c->keys));
    uint32_t *codes = keys ? (uint32_t *)keys->data : NULL;
    size_t count = keys ? keys->size / sizeof(uint32_t) : 0;
    for (size_t i = 0; i < count; i++)
        if (codes[i] < 256) c->keys[codes[i]] = true;
    sync_keyboard_axes(c);
    mark_activity(c);
}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface) {
    WlClient *c = data;
    (void)keyboard; (void)serial; (void)surface;
    /* Focus lost: drop all held state or the camera would keep flying while
     * the user types in another window. */
    memset(c->keys, 0, sizeof(c->keys));
    sync_keyboard_axes(c);
}
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state) {
    WlClient *c = data;
    (void)keyboard; (void)serial; (void)time;
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (key < 256) c->keys[key] = pressed;
    sync_keyboard_axes(c);
    mark_activity(c);
    if (!pressed) return;

    bool ctrl = key_held(c, KEY_LEFTCTRL) || key_held(c, KEY_RIGHTCTRL);
    /* xkb keycodes are evdev + 8 (the historical X11 offset). */
    xkb_keysym_t sym = c->xkb_state
        ? xkb_state_key_get_one_sym(c->xkb_state, key + 8) : XKB_KEY_NoSymbol;
    if (sym == XKB_KEY_Escape || (ctrl && (sym == XKB_KEY_q || sym == XKB_KEY_Q)) ||
        (sym == XKB_KEY_NoSymbol && key == KEY_ESC)) {
        c->input.quit = true;
    } else if (sym == XKB_KEY_r || sym == XKB_KEY_R ||
               (sym == XKB_KEY_NoSymbol && key == KEY_R)) {
        c->input.reset_camera = true;
    } else if (sym == XKB_KEY_space || (sym == XKB_KEY_NoSymbol && key == KEY_SPACE)) {
        c->input.toggle_pause = true;
    } else if (sym == XKB_KEY_f || sym == XKB_KEY_F ||
               (sym == XKB_KEY_NoSymbol && key == KEY_F)) {
        c->input.toggle_fullscreen = true;
    }
}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group) {
    WlClient *c = data;
    (void)keyboard; (void)serial;
    /* Only the compositor knows the true modifier state (it owns the evdev
     * devices); mirror it verbatim instead of tracking Shift presses. */
    if (c->xkb_state)
        xkb_state_update_mask(c->xkb_state, depressed, latched, locked, 0, 0, group);
}
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay) {
    /* Deliberately unused: continuous actions read held state each frame
     * (see sync_keyboard_axes), and one-shot actions should not repeat. A
     * text-entry client would need a timer driven by rate/delay here. */
    (void)data; (void)keyboard; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ------------------------------------------------------------------- touch */

/* Touch -> camera gestures (mobile requirement):
 *   1 finger drag        orbit        2-finger pinch      zoom
 *   2-finger drag        pan          double-tap          reset camera
 * Gestures are computed in the wl_touch.frame handler, because down/motion
 * events for several contacts arrive as one atomic group terminated by
 * frame - computing a pinch from half-updated contacts would see sawtooth
 * distances. On 2->1 finger transitions the remaining finger's baseline is
 * reset, which is what stops the customary "jump" when lifting one finger. */
static TouchPoint *touch_find(WlClient *c, int32_t id, bool create) {
    TouchPoint *free_slot = NULL;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) {
        if (c->touch_points[i].active && c->touch_points[i].id == id)
            return &c->touch_points[i];
        if (!c->touch_points[i].active && !free_slot) free_slot = &c->touch_points[i];
    }
    return create ? free_slot : NULL;
}

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
                       struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    WlClient *c = data;
    (void)touch; (void)serial; (void)time; (void)surface;
    TouchPoint *p = touch_find(c, id, true);
    if (!p) return;
    p->active = true;
    p->id = id;
    p->x = p->prev_x = p->start_x = wl_fixed_to_double(x);
    p->y = p->prev_y = p->start_y = wl_fixed_to_double(y);
    p->down_time = wlclient_now();
    mark_activity(c);
}
static void touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time, int32_t id) {
    WlClient *c = data;
    (void)touch; (void)serial; (void)time;
    TouchPoint *p = touch_find(c, id, false);
    if (!p) return;
    double now = wlclient_now();
    double moved = hypot(p->x - p->start_x, p->y - p->start_y);
    if (now - p->down_time < 0.25 && moved < 12.0) { /* a tap */
        double gap = now - c->last_tap_time;
        double dist = hypot(p->x - c->last_tap_x, p->y - c->last_tap_y);
        if (gap < 0.35 && dist < 40.0) c->input.reset_camera = true;
        c->last_tap_time = now;
        c->last_tap_x = p->x; c->last_tap_y = p->y;
    }
    p->active = false;
    mark_activity(c);
}
static void touch_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id,
                         wl_fixed_t x, wl_fixed_t y) {
    WlClient *c = data;
    (void)touch; (void)time;
    TouchPoint *p = touch_find(c, id, false);
    if (!p) return;
    p->x = wl_fixed_to_double(x);
    p->y = wl_fixed_to_double(y);
    mark_activity(c);
}
static void touch_frame(void *data, struct wl_touch *touch) {
    WlClient *c = data;
    (void)touch;
    TouchPoint *active[MAX_TOUCH_POINTS];
    int count = 0;
    for (int i = 0; i < MAX_TOUCH_POINTS; i++)
        if (c->touch_points[i].active) active[count++] = &c->touch_points[i];

    if (count == 1) {
        TouchPoint *p = active[0];
        if (c->pinch_valid) { /* just came from 2 fingers: re-baseline */
            p->prev_x = p->x; p->prev_y = p->y;
        }
        c->input.orbit_dx += (float)(p->x - p->prev_x);
        c->input.orbit_dy += (float)(p->y - p->prev_y);
        c->pinch_valid = false;
    } else if (count == 2) {
        double cx = (active[0]->x + active[1]->x) * 0.5;
        double cy = (active[0]->y + active[1]->y) * 0.5;
        double dist = hypot(active[0]->x - active[1]->x, active[0]->y - active[1]->y);
        if (c->pinch_valid) {
            c->input.pan_dx += (float)(cx - c->pinch_centroid_x);
            c->input.pan_dy += (float)(cy - c->pinch_centroid_y);
            if (dist > 10.0 && c->pinch_distance > 10.0)
                /* log-ratio: spreading fingers (dist grows) zooms in. */
                c->input.zoom_delta += (float)(log(c->pinch_distance / dist) * 8.0);
        }
        c->pinch_centroid_x = cx; c->pinch_centroid_y = cy; c->pinch_distance = dist;
        c->pinch_valid = true;
    } else {
        c->pinch_valid = false;
    }
    for (int i = 0; i < count; i++) {
        active[i]->prev_x = active[i]->x;
        active[i]->prev_y = active[i]->y;
    }
}
static void touch_cancel(void *data, struct wl_touch *touch) {
    WlClient *c = data;
    (void)touch;
    /* The compositor aborted the sequence (e.g. a gesture took over): drop
     * every contact, or stale points would orbit the camera forever. */
    for (int i = 0; i < MAX_TOUCH_POINTS; i++) c->touch_points[i].active = false;
    c->pinch_valid = false;
}
static void touch_shape(void *data, struct wl_touch *touch, int32_t id,
                        wl_fixed_t major, wl_fixed_t minor) {
    (void)data; (void)touch; (void)id; (void)major; (void)minor; /* ellipse unused */
}
static void touch_orientation(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t orientation) {
    (void)data; (void)touch; (void)id; (void)orientation;
}
static const struct wl_touch_listener touch_listener = {
    .down = touch_down, .up = touch_up, .motion = touch_motion, .frame = touch_frame,
    .cancel = touch_cancel, .shape = touch_shape, .orientation = touch_orientation,
};

/* -------------------------------------------------------------------- seat */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    WlClient *c = data;
    /* Capabilities are dynamic (USB hot-plug, touch appearing on a
     * convertible): create devices as they appear, release as they vanish.
     * Creating them once at startup would miss a keyboard plugged in later
     * and leak proxies for a mouse that was unplugged. */
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !c->pointer) {
        c->pointer = wl_seat_get_pointer(seat);
        if (c->pointer) wl_pointer_add_listener(c->pointer, &pointer_listener, c);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && c->pointer) {
        release_pointer(c);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !c->keyboard) {
        c->keyboard = wl_seat_get_keyboard(seat);
        if (c->keyboard) wl_keyboard_add_listener(c->keyboard, &keyboard_listener, c);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && c->keyboard) {
        release_keyboard(c);
        memset(c->keys, 0, sizeof(c->keys));
        sync_keyboard_axes(c);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !c->touch) {
        c->touch = wl_seat_get_touch(seat);
        if (c->touch) wl_touch_add_listener(c->touch, &touch_listener, c);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && c->touch) {
        release_touch(c);
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

/* ------------------------------------------------------------------ output */

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel, const char *make,
                            const char *model, int32_t transform) {
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}
static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh) {
    (void)data; (void)output; (void)flags; (void)w; (void)h; (void)refresh;
}
static void output_done(void *data, struct wl_output *output) { (void)data; (void)output; }
static void output_scale(void *data, struct wl_output *output, int32_t factor) {
    WlOutputInfo *info = data;
    (void)output;
    if (factor > 0) info->scale = factor;
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done,
    .scale = output_scale,
};

/* ----------------------------------------------------------- surface enter */

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output) {
    WlClient *c = data;
    (void)surface;
    if (c->scale_override > 0) return; /* user forced --scale: it wins */
    for (int i = 0; i < c->output_count; i++) {
        if (c->outputs[i] && c->outputs[i]->output == output) {
            int scale = c->outputs[i]->scale > 0 ? c->outputs[i]->scale : 1;
            if (scale != c->desired_scale) {
                c->desired_scale = scale;
                c->resize_pending = true; /* applied outside dispatch */
            }
            break;
        }
    }
}
static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output) {
    (void)data; (void)surface; (void)output;
}
static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter, .leave = surface_leave,
};

/* ---------------------------------------------------------------- xdg-shell */

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data;
    /* Mandatory: an unanswered ping makes compositors mark the app hung. */
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
                               struct wl_array *states) {
    WlClient *c = data;
    (void)toplevel;
    /* Do NOT resize or ack here. xdg-shell batches a state change as
     * toplevel.configure (suggestion) followed by xdg_surface.configure
     * (commit point); only the latter, with its serial, may be acked and
     * acted on. 0x0 means "you choose" - keep the current size. */
    if (w > 0) c->pending_logical_w = w;
    if (h > 0) c->pending_logical_h = h;
    bool fullscreen = false;
    uint32_t *state = states ? (uint32_t *)states->data : NULL;
    size_t count = states ? states->size / sizeof(uint32_t) : 0;
    for (size_t i = 0; i < count; i++)
        if (state[i] == XDG_TOPLEVEL_STATE_FULLSCREEN) fullscreen = true;
    c->fullscreen = fullscreen;
    c->fullscreen_desired = fullscreen; /* compositor's word is final */
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    WlClient *c = data;
    (void)toplevel;
    c->running = false; /* compositor asked us to close: shut down cleanly */
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    WlClient *c = data;
    /* ack_configure FIRST, before any buffer is attached for this state -
     * attaching earlier is the xdg_surface.unconfigured_buffer error. The
     * ack tells the compositor "the next commit uses this state". */
    xdg_surface_ack_configure(xdg, serial);
    c->configured_once = true;
    if ((c->pending_logical_w > 0 && c->pending_logical_w != c->logical_w) ||
        (c->pending_logical_h > 0 && c->pending_logical_h != c->logical_h) ||
        c->desired_scale != c->scale)
        c->resize_pending = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

/* ----------------------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    WlClient *c = data;
    /* Bind the minimum of what we use and what the compositor offers.
     * Capping xdg_wm_base at 3 and wl_seat at 7 keeps every event we can
     * receive inside the listener fields implemented above - and 3/7 are
     * exactly what the in-repo ChromeDrive compositor offers. */
    if (strcmp(interface, wl_compositor_interface.name) == 0 && !c->compositor) {
        uint32_t v = version < 4 ? version : 4;
        c->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, v);
    } else if (strcmp(interface, wl_shm_interface.name) == 0 && !c->shm) {
        c->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0 && !c->wm_base) {
        uint32_t v = version < 3 ? version : 3;
        c->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, v);
        if (c->wm_base) xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && !c->seat) {
        uint32_t v = version < 7 ? version : 7;
        c->seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
        c->seat_global_name = name;
        if (c->seat) wl_seat_add_listener(c->seat, &seat_listener, c);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && c->output_count < MAX_OUTPUTS) {
        uint32_t v = version < 2 ? version : 2; /* v2 = scale event */
        WlOutputInfo *info = calloc(1, sizeof(*info));
        if (!info) return;
        info->client = c;
        info->global_name = name;
        info->scale = 1;
        info->output = wl_registry_bind(registry, name, &wl_output_interface, v);
        if (!info->output) { free(info); return; }
        wl_output_add_listener(info->output, &output_listener, info);
        c->outputs[c->output_count++] = info;
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    WlClient *c = data;
    (void)registry;
    for (int i = 0; i < c->output_count; i++) {
        if (c->outputs[i] && c->outputs[i]->global_name == name) {
            wl_proxy_destroy((struct wl_proxy *)c->outputs[i]->output);
            free(c->outputs[i]);
            c->outputs[i] = c->outputs[--c->output_count];
            c->outputs[c->output_count] = NULL;
            return;
        }
    }
    if (c->seat && name == c->seat_global_name) {
        release_pointer(c); release_keyboard(c); release_touch(c);
        wl_proxy_destroy((struct wl_proxy *)c->seat);
        c->seat = NULL;
    }
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------- frame pacing */

static void frame_done(void *data, struct wl_callback *callback, uint32_t time_ms) {
    WlClient *c = data;
    /* The callback object is single-shot: destroy it in the handler, always.
     * Its arrival means the compositor has presented (or is about to
     * present) our last buffer and is ready for the next one - this flag is
     * the entire 60 fps throttle. If the window is occluded many compositors
     * withhold frame callbacks, and the client simply sleeps in poll(). */
    if (callback) wl_callback_destroy(callback);
    if (c->frame_callback == callback) c->frame_callback = NULL;
    c->last_frame_time_ms = time_ms;
    c->frame_ready = true;
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

bool wlclient_can_draw(const WlClient *c) {
    return c->configured_once && c->frame_ready && !c->frame_callback &&
           c->logical_w > 0 && c->logical_h > 0 && c->egl_surface != EGL_NO_SURFACE;
}

bool wlclient_present(WlClient *c) {
    if (!c->frame_callback) {
        c->frame_callback = wl_surface_frame(c->surface);
        if (!c->frame_callback) return false;
        wl_callback_add_listener(c->frame_callback, &frame_listener, c);
    }
    c->frame_ready = false;
    /* eglSwapBuffers on a Wayland window surface internally does:
     * wl_surface.attach(buffer); wl_surface.damage(...); wl_surface.commit()
     * and blocks per eglSwapInterval(1) if too many buffers are queued. Our
     * frame request above is flushed as part of that same commit, exactly as
     * the protocol intends (frame is double-buffered surface state). */
    if (!eglSwapBuffers(c->egl_display, c->egl_surface)) {
        EGLint err = eglGetError();
        fprintf(stderr, "garage3d: eglSwapBuffers failed (EGL error 0x%x)\n", err);
        return false;
    }
    return true;
}

void wlclient_apply_pending_resize(WlClient *c) {
    if (!c->resize_pending || !c->egl_window) return;
    c->resize_pending = false;
    if (c->pending_logical_w > 0) c->logical_w = c->pending_logical_w;
    if (c->pending_logical_h > 0) c->logical_h = c->pending_logical_h;
    int scale = c->scale_override > 0 ? c->scale_override : c->desired_scale;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    /* set_buffer_scale is a wl_surface v3 request; an ancient compositor
     * without it must be served scale-1 buffers. */
    if (wl_surface_get_version(c->surface) < 3) scale = 1;
    c->scale = scale;
    /* Resizing the wl_egl_window is all that is needed: Mesa reallocates the
     * backing buffers lazily on the next swap. Recreate neither the EGL
     * surface nor the context (the context holds all our GL objects). */
    wl_egl_window_resize(c->egl_window, wlclient_phys_width(c), wlclient_phys_height(c), 0, 0);
    if (wl_surface_get_version(c->surface) >= 3)
        wl_surface_set_buffer_scale(c->surface, scale);
}

void wlclient_set_fullscreen(WlClient *c, bool on) {
    if (!c->toplevel || c->fullscreen_desired == on) return;
    c->fullscreen_desired = on;
    /* This is a *request*: the compositor confirms (or not) via the next
     * toplevel.configure states array, which is why c->fullscreen is only
     * written there, never here. fullscreen_desired merely remembers what
     * we last asked for so a config reload echoing the same value doesn't
     * spam requests. */
    if (on) xdg_toplevel_set_fullscreen(c->toplevel, NULL);
    else xdg_toplevel_unset_fullscreen(c->toplevel);
}

void wlclient_toggle_fullscreen(WlClient *c) {
    wlclient_set_fullscreen(c, !c->fullscreen);
}

void wlclient_set_scale_override(WlClient *c, int scale) {
    if (scale < 0) scale = 0;
    if (scale > 4) scale = 4;
    if (c->scale_override == scale) return;
    c->scale_override = scale;
    c->resize_pending = true; /* applied at the next drawable frame */
}

/* ------------------------------------------------------------------ dispatch */

int wlclient_dispatch(WlClient *c, int timeout_ms) {
    struct wl_display *display = c->display;
    /* prepare_read returns -1 while the queue already has events: drain
     * those first. Once it succeeds we own the read intent and MUST either
     * read_events or cancel_read on every path below. */
    while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) return -1;
    }
    /* Flush outgoing requests (acks, frame requests) before sleeping, or
     * the compositor would never see the ack that unblocks our next frame. */
    int flush_result = wl_display_flush(display);
    if (flush_result < 0 && errno != EAGAIN) {
        wl_display_cancel_read(display);
        return -1;
    }
    struct pollfd pfd = {
        .fd = wl_display_get_fd(display),
        .events = (short)(POLLIN | (flush_result < 0 ? POLLOUT : 0)),
    };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        wl_display_cancel_read(display);
        return errno == EINTR ? 0 : -1; /* signals are handled by main */
    }
    if (ret > 0 && (pfd.revents & POLLOUT)) (void)wl_display_flush(display);
    if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
        if (wl_display_read_events(display) < 0) return -1;
    } else {
        wl_display_cancel_read(display);
    }
    if (wl_display_dispatch_pending(display) < 0) return -1;
    return wl_display_get_error(display) ? -1 : 0;
}

/* ---------------------------------------------------------------------- EGL */

static bool init_egl(WlClient *c) {
    /* Prefer the explicit platform entry point; eglGetDisplay() on a
     * wl_display works on Mesa but conflates native display types. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform)
        c->egl_display = get_platform(EGL_PLATFORM_WAYLAND_KHR, c->display, NULL);
    else
        c->egl_display = eglGetDisplay((EGLNativeDisplayType)c->display);
    if (c->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "garage3d: eglGetPlatformDisplay failed\n");
        return false;
    }
    if (!eglInitialize(c->egl_display, NULL, NULL)) {
        fprintf(stderr, "garage3d: eglInitialize failed (0x%x)\n", eglGetError());
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "garage3d: eglBindAPI(EGL_OPENGL_ES_API) failed\n");
        return false;
    }
    static const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        /* No MSAA by default: 4x MSAA on tile-based mobile GPUs costs real
         * bandwidth, and this dark scene hides aliasing well. */
        EGL_SAMPLE_BUFFERS, 0,
        EGL_NONE,
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(c->egl_display, config_attrs, &c->egl_config, 1, &num_configs) ||
        num_configs < 1) {
        fprintf(stderr, "garage3d: no suitable EGL config (GLES3 window RGBA8/D24)\n");
        return false;
    }
    /* ES 3.1 first (the requested target), ES 3.0 fallback: every feature
     * used is core 3.0, so older mobile drivers still run the same binary. */
    static const int versions[][2] = { { 3, 1 }, { 3, 0 } };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]) && c->egl_context == EGL_NO_CONTEXT; i++) {
        EGLint ctx_attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION, versions[i][0],
            EGL_CONTEXT_MINOR_VERSION, versions[i][1],
            EGL_NONE,
        };
        c->egl_context = eglCreateContext(c->egl_display, c->egl_config, EGL_NO_CONTEXT, ctx_attrs);
        if (c->egl_context != EGL_NO_CONTEXT)
            fprintf(stderr, "garage3d: created OpenGL ES %d.%d context\n", versions[i][0], versions[i][1]);
    }
    if (c->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "garage3d: eglCreateContext failed (0x%x)\n", eglGetError());
        return false;
    }

    c->egl_window = wl_egl_window_create(c->surface, wlclient_phys_width(c), wlclient_phys_height(c));
    if (!c->egl_window) {
        fprintf(stderr, "garage3d: wl_egl_window_create failed\n");
        return false;
    }
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_surface =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    if (create_platform_surface)
        c->egl_surface = create_platform_surface(c->egl_display, c->egl_config, c->egl_window, NULL);
    else
        c->egl_surface = eglCreateWindowSurface(c->egl_display, c->egl_config,
                                                (EGLNativeWindowType)c->egl_window, NULL);
    if (c->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "garage3d: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(c->egl_display, c->egl_surface, c->egl_surface, c->egl_context)) {
        fprintf(stderr, "garage3d: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return false;
    }
    /* Swap interval 1 (don't outrun presentation) *plus* frame callbacks:
     * the interval alone would throttle inside the driver with an extra
     * buffer of latency; frame callbacks tell us the exact presented moment.
     * Using both is the standard Mesa/Wayland recommendation. */
    eglSwapInterval(c->egl_display, 1);
    if (c->scale > 1 && wl_surface_get_version(c->surface) >= 3)
        wl_surface_set_buffer_scale(c->surface, c->scale);
    return true;
}

/* ------------------------------------------------------------------- init */

bool wlclient_init(WlClient *c, const AppConfig *cfg) {
    memset(c, 0, sizeof(*c));
    c->egl_display = EGL_NO_DISPLAY;
    c->egl_context = EGL_NO_CONTEXT;
    c->egl_surface = EGL_NO_SURFACE;
    c->running = true;
    c->logical_w = cfg->width > 0 ? cfg->width : 1280;
    c->logical_h = cfg->height > 0 ? cfg->height : 720;
    c->scale_override = cfg->scale_override;
    c->scale = cfg->scale_override > 0 ? cfg->scale_override : 1;
    c->desired_scale = c->scale;
    c->input.last_activity = wlclient_now();

    c->xkb_context = xkb_context_new(0); /* NULL is tolerated (evdev fallback) */

    /* NULL name: libwayland uses $WAYLAND_DISPLAY, else
     * $XDG_RUNTIME_DIR/wayland-0. This is also how the client finds
     * ChromeDrive: point WAYLAND_DISPLAY at its printed socket. */
    c->display = wl_display_connect(NULL);
    if (!c->display) {
        fprintf(stderr, "garage3d: wl_display_connect failed - is a Wayland "
                        "compositor running? (WAYLAND_DISPLAY=%s)\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
        return false;
    }
    c->registry = wl_display_get_registry(c->display);
    if (!c->registry) return false;
    wl_registry_add_listener(c->registry, &registry_listener, c);
    /* One roundtrip guarantees every global + seat capability + output scale
     * has been seen before we act on them. */
    if (wl_display_roundtrip(c->display) < 0) return false;
    if (!c->compositor || !c->wm_base) {
        fprintf(stderr, "garage3d: compositor lacks %s (xdg-shell is required)\n",
                !c->compositor ? "wl_compositor" : "xdg_wm_base");
        return false;
    }
    if (!c->seat)
        fprintf(stderr, "garage3d: warning: no wl_seat - running input-less (attract mode)\n");

    /* Role stack: wl_surface is just a rectangle of pixels until an
     * xdg_toplevel role makes it a window. Title/app_id/fullscreen are
     * initial state and must be set before the first commit. */
    c->surface = wl_compositor_create_surface(c->compositor);
    if (!c->surface) return false;
    wl_surface_add_listener(c->surface, &surface_listener, c);
    c->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, c->surface);
    if (!c->xdg_surface) return false;
    xdg_surface_add_listener(c->xdg_surface, &xdg_surface_listener, c);
    c->toplevel = xdg_surface_get_toplevel(c->xdg_surface);
    if (!c->toplevel) return false;
    xdg_toplevel_add_listener(c->toplevel, &toplevel_listener, c);
    xdg_toplevel_set_title(c->toplevel, cfg->title ? cfg->title : "Garage3D");
    xdg_toplevel_set_app_id(c->toplevel, cfg->app_id ? cfg->app_id : "chromedrive.garage3d");
    xdg_toplevel_set_min_size(c->toplevel, 320, 200);
    if (cfg->start_fullscreen) { xdg_toplevel_set_fullscreen(c->toplevel, NULL); c->fullscreen_desired = true; }

    /* The initial commit carries NO buffer. Its only job is to make the
     * compositor send the first configure with a suggested size - attaching
     * a buffer now would be a fatal protocol error. */
    wl_surface_commit(c->surface);
    double deadline = wlclient_now() + 10.0;
    while (!c->configured_once) {
        if (wlclient_dispatch(c, 1000) < 0) {
            fprintf(stderr, "garage3d: display error while awaiting initial configure\n");
            return false;
        }
        if (wlclient_now() > deadline) {
            fprintf(stderr, "garage3d: timed out waiting for xdg_surface.configure\n");
            return false;
        }
    }
    /* Adopt the configured size (0 would have meant "keep our request"). */
    if (c->pending_logical_w > 0) c->logical_w = c->pending_logical_w;
    if (c->pending_logical_h > 0) c->logical_h = c->pending_logical_h;
    if (c->scale_override > 0) c->scale = c->desired_scale = c->scale_override;
    else c->scale = c->desired_scale;
    if (c->surface && wl_surface_get_version(c->surface) < 3) c->scale = c->desired_scale = 1;
    if (c->scale < 1) c->scale = 1;
    if (c->scale > 4) c->scale = 4;
    c->resize_pending = false;

    if (!init_egl(c)) return false;
    c->frame_ready = true; /* nothing outstanding: the first frame may draw */
    return true;
}

void wlclient_shutdown(WlClient *c) {
    /* Reverse creation order. Two dependencies matter:
     *  - the EGL surface must die before the wl_egl_window it renders into,
     *  - the xdg_toplevel must die before its xdg_surface/wl_surface
     *    (destroying a surface with a live role is a protocol error).
     * GL objects themselves are freed by renderer_shutdown() BEFORE this is
     * called, while the context is still current (see main.c). */
    if (c->frame_callback) wl_callback_destroy(c->frame_callback);
    c->frame_callback = NULL;
    if (c->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(c->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (c->egl_surface != EGL_NO_SURFACE) eglDestroySurface(c->egl_display, c->egl_surface);
        if (c->egl_context != EGL_NO_CONTEXT) eglDestroyContext(c->egl_display, c->egl_context);
    }
    if (c->egl_window) wl_egl_window_destroy(c->egl_window);
    if (c->egl_display != EGL_NO_DISPLAY) eglTerminate(c->egl_display);

    if (c->cursor_surface) wl_surface_destroy(c->cursor_surface);
    if (c->cursor_theme) wl_cursor_theme_destroy(c->cursor_theme);
    release_pointer(c);
    release_keyboard(c);
    release_touch(c);
    if (c->seat) {
        if (wl_seat_get_version(c->seat) >= 5) wl_seat_release(c->seat);
        else wl_proxy_destroy((struct wl_proxy *)c->seat);
    }
    for (int i = 0; i < c->output_count; i++) {
        if (c->outputs[i]) {
            wl_proxy_destroy((struct wl_proxy *)c->outputs[i]->output);
            free(c->outputs[i]);
        }
    }
    if (c->toplevel) xdg_toplevel_destroy(c->toplevel);
    if (c->xdg_surface) xdg_surface_destroy(c->xdg_surface);
    if (c->surface) wl_surface_destroy(c->surface);
    if (c->wm_base) xdg_wm_base_destroy(c->wm_base);
    if (c->shm) wl_proxy_destroy((struct wl_proxy *)c->shm);
    if (c->compositor) wl_proxy_destroy((struct wl_proxy *)c->compositor);
    if (c->xkb_state) xkb_state_unref(c->xkb_state);
    if (c->xkb_keymap) xkb_keymap_unref(c->xkb_keymap);
    if (c->xkb_context) xkb_context_unref(c->xkb_context);
    if (c->registry) wl_registry_destroy(c->registry);
    if (c->display) {
        wl_display_flush(c->display);
        wl_display_disconnect(c->display);
    }
    memset(c, 0, sizeof(*c));
    c->egl_display = EGL_NO_DISPLAY;
    c->egl_context = EGL_NO_CONTEXT;
    c->egl_surface = EGL_NO_SURFACE;
}
