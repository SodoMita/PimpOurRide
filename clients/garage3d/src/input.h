/*
 * input.h - the contract between the Wayland platform module and the
 * scene/camera code.
 *
 * Why this indirection exists: scene.c and animation.c must never include
 * <wayland-client.h>. The platform module (wayland_client.c) translates
 * wl_pointer / wl_keyboard / wl_touch events into this plain struct once
 * per dispatch; the camera consumes accumulated *deltas* once per rendered
 * frame and then calls input_clear_frame(). That keeps camera motion
 * frame-rate independent (deltas are integrated with dt in scene.c, not
 * applied per-event, where 1000 Hz mice would otherwise spin faster than
 * 60 Hz touchpads).
 */
#ifndef GARAGE3D_INPUT_H
#define GARAGE3D_INPUT_H

#include <stdbool.h>

typedef struct {
    /* Accumulated since the last rendered frame (logical pixels).
     * Pointer left-drag and one-finger touch feed orbit; right/middle-drag
     * and two-finger centroid motion feed pan. */
    float orbit_dx, orbit_dy;
    float pan_dx, pan_dy;
    /* Wheel steps (discrete) merged with smooth axis deltas, plus pinch:
     * positive = zoom out (increase orbit distance). Pinch contributes
     * log(prev_dist / dist) so equal finger ratios give equal zooms. */
    float zoom_delta;
    float fov_delta; /* from +/- keys, degrees per frame-scaled in camera */

    /* Held-state movement axes in [-1, 1], recomputed by the platform module
     * on every keyboard event from its held-key bitmap. State-based (rather
     * than event-based) movement is what makes WASD speed independent of
     * the compositor's key-repeat rate (wl_keyboard.repeat_info). */
    float move_forward; /* W/S, Up/Down */
    float move_strafe;  /* D/A, Right/Left */
    float move_vertical;/* E/Q, PageUp/PageDown */

    /* Edge-triggered requests: set by an event, consumed+cleared by main. */
    bool reset_camera;      /* R key or touch double-tap */
    bool toggle_pause;      /* Space */
    bool toggle_fullscreen; /* F */
    bool quit;              /* Esc / Ctrl+Q */

    /* Monotonic seconds of the last user input; drives the idle attract
     * camera drift in scene.c (stops the moment the user touches anything). */
    double last_activity;
    bool any_activity; /* any event since last frame (for the idle timer) */
} InputState;

static inline void input_clear_frame(InputState *in) {
    in->orbit_dx = in->orbit_dy = 0.0f;
    in->pan_dx = in->pan_dy = 0.0f;
    in->zoom_delta = 0.0f;
    in->fov_delta = 0.0f;
    in->reset_camera = false;
    in->toggle_pause = false;
    in->toggle_fullscreen = false;
    in->any_activity = false;
    /* move_*, fov_delta and quit deliberately survive: held state, not pulses. */
}

#endif
