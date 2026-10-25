/* Raw evdev input for the fbdev backend. No libinput/libevdev dependency.
 * Uses <linux/input.h> ioctls + read() directly. Mirrors backend_sdl.c and
 * backend_vnc.c conventions for input dispatch through the compositor core. */
#include "chrome.h"
#include "evdev_util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static EvdevState *evdev_state(Server *s) {
    return s ? (EvdevState *)s->evdev_state : NULL;
}

static void evdev_device_close(EvdevDevice *d) {
    if (d->fd >= 0) {
        if (d->grabbed) {
            ioctl(d->fd, EVIOCGRAB, (int)0);
            d->grabbed = false;
        }
        close(d->fd);
        d->fd = -1;
    }
}

static void evdev_release_finger(Server *s, EvdevFinger *f, uint32_t time, bool is_chrome_owner) {
    if (!f->active) return;
    switch (f->route) {
        case CD_TOUCH_ROUTE_APP:
            server_touch_up(s, f->touch_id, time);
            evdev_util_touch_id_release(f->touch_id);
            break;
        case CD_TOUCH_ROUTE_POINTER:
            s->mouse_x = f->last_x;
            s->mouse_y = f->last_y;
            Hit hit = scene_pick(s, s->mouse_x, s->mouse_y, s->pointer_grab != NULL);
            server_pointer_motion(s, hit, time);
            server_pointer_button(s, BTN_LEFT, false, time);
            break;
        case CD_TOUCH_ROUTE_WHEEL:
            s->mouse_x = f->last_x;
            s->mouse_y = f->last_y;
            scene_wheel_button(s, (Hit){0}, f->last_x, f->last_y, false, time);
            break;
        case CD_TOUCH_ROUTE_CHROME:
            s->mouse_x = f->last_x;
            s->mouse_y = f->last_y;
            hit = scene_pick(s, f->last_x, f->last_y, false);
            if (s->arrange_mode) {
                app_arrange_pointer_button(s, hit, BTN_LEFT, false);
            } else if (s->frame_dragging) {
                app_frame_pointer_button(s, hit, BTN_LEFT, false);
            }
            break;
        case CD_TOUCH_ROUTE_LOOK:
        case CD_TOUCH_ROUTE_NONE:
            break;
    }
    if (is_chrome_owner) {
        s->evdev_chrome_touch_id = -1;
    }
    f->active = false;
    f->route = CD_TOUCH_ROUTE_NONE;
    f->touch_id = -1;
    f->tracking_id = -1;
}

static void evdev_release_device_state(Server *s, EvdevDevice *d, uint32_t time) {
    uint32_t released_mask = d->sent_button_mask & d->held_button_mask;
    if (released_mask) {
        if (released_mask & (1u << 0)) server_pointer_button(s, BTN_LEFT, false, time);
        if (released_mask & (1u << 1)) server_pointer_button(s, BTN_RIGHT, false, time);
        if (released_mask & (1u << 2)) server_pointer_button(s, BTN_MIDDLE, false, time);
        if (released_mask & (1u << 3)) server_pointer_button(s, BTN_SIDE, false, time);
        if (released_mask & (1u << 4)) server_pointer_button(s, BTN_EXTRA, false, time);
    }
    d->held_button_mask = 0;
    d->sent_button_mask = 0;
    d->btn_left = d->btn_right = d->btn_middle = d->btn_side = d->btn_extra = false;

    for (int i = 0; i < EVDEV_MAX_SLOTS; i++) {
        evdev_release_finger(s, &d->mt_fingers[i], time, d->mt_fingers[i].is_chrome_owner);
    }
    evdev_release_finger(s, &d->st_finger, time, d->st_finger.is_chrome_owner);

    if (d->middle_look_held) {
        s->middle_look = false;
        d->middle_look_held = false;
    }
}

static bool evdev_open_device(EvdevDevice *d, const char *path, bool grab) {
    d->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (d->fd < 0) return false;
    snprintf(d->path, sizeof(d->path), "%s", path);

    uint8_t evbit[EV_MAX / 8 + 1] = {0};
    uint8_t keybit[KEY_MAX / 8 + 1] = {0};
    uint8_t relbit[REL_MAX / 8 + 1] = {0};
    uint8_t absbit[ABS_MAX / 8 + 1] = {0};
    uint8_t propbit[INPUT_PROP_MAX / 8 + 1] = {0};

    if (ioctl(d->fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
        evdev_device_close(d);
        return false;
    }
    if (ioctl(d->fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) {
        evdev_device_close(d);
        return false;
    }
    if (ioctl(d->fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit) < 0) {
        evdev_device_close(d);
        return false;
    }
    if (ioctl(d->fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) < 0) {
        evdev_device_close(d);
        return false;
    }
    ioctl(d->fd, EVIOCGPROP(sizeof(propbit)), propbit);
    d->is_direct = (propbit[INPUT_PROP_DIRECT / 8] & (1 << (INPUT_PROP_DIRECT % 8))) != 0;

    if (ioctl(d->fd, EVIOCGNAME(sizeof(d->name)), d->name) < 0) {
        snprintf(d->name, sizeof(d->name), "unknown");
    }

    bool has_keys = false;
    for (int k = KEY_ESC; k <= KEY_MAX; k++) {
        if (keybit[k / 8] & (1 << (k % 8))) { has_keys = true; break; }
    }

    d->has_rel_x = (relbit[REL_X / 8] & (1 << (REL_X % 8))) != 0;
    d->has_rel_y = (relbit[REL_Y / 8] & (1 << (REL_Y % 8))) != 0;
    d->has_abs_x = (absbit[ABS_X / 8] & (1 << (ABS_X % 8))) != 0;
    d->has_abs_y = (absbit[ABS_Y / 8] & (1 << (ABS_Y % 8))) != 0;

    bool has_mt_x = (absbit[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8))) != 0;
    bool has_mt_y = (absbit[ABS_MT_POSITION_Y / 8] & (1 << (ABS_MT_POSITION_Y % 8))) != 0;
    bool has_mt_tracking = (absbit[ABS_MT_TRACKING_ID / 8] & (1 << (ABS_MT_TRACKING_ID % 8))) != 0;
    bool has_btn_touch = (keybit[BTN_TOUCH / 8] & (1 << (BTN_TOUCH % 8))) != 0;

    if (has_mt_x && has_mt_y && has_mt_tracking && d->is_direct) {
        d->type = EVDEV_DEV_MULTITOUCH;
        if (ioctl(d->fd, EVIOCGABS(ABS_MT_POSITION_X), &d->abs_mt_x) < 0) {
            evdev_device_close(d);
            return false;
        }
        if (ioctl(d->fd, EVIOCGABS(ABS_MT_POSITION_Y), &d->abs_mt_y) < 0) {
            evdev_device_close(d);
            return false;
        }
        for (int i = 0; i < EVDEV_MAX_SLOTS; i++) {
            d->mt_fingers[i].tracking_id = -1;
            d->mt_fingers[i].active = false;
            d->mt_fingers[i].touch_id = -1;
            d->mt_fingers[i].route = CD_TOUCH_ROUTE_NONE;
            d->mt_fingers[i].last_x = d->mt_fingers[i].last_y = 0;
            d->mt_fingers[i].is_chrome_owner = false;
        }
    } else if (has_btn_touch && d->has_abs_x && d->has_abs_y) {
        d->type = EVDEV_DEV_SINGLE_TOUCH;
        if (ioctl(d->fd, EVIOCGABS(ABS_X), &d->abs_x) < 0) {
            evdev_device_close(d);
            return false;
        }
        if (ioctl(d->fd, EVIOCGABS(ABS_Y), &d->abs_y) < 0) {
            evdev_device_close(d);
            return false;
        }
        d->st_finger.tracking_id = -1;
        d->st_finger.active = false;
        d->st_finger.touch_id = -1;
        d->st_finger.route = CD_TOUCH_ROUTE_NONE;
        d->st_finger.last_x = d->st_finger.last_y = 0;
        d->st_finger.is_chrome_owner = false;
    } else if (d->has_rel_x && d->has_rel_y) {
        d->type = EVDEV_DEV_REL_POINTER;
    } else if (d->has_abs_x && d->has_abs_y && !d->is_direct) {
        d->type = EVDEV_DEV_ABS_POINTER;
        if (ioctl(d->fd, EVIOCGABS(ABS_X), &d->abs_x) < 0) {
            evdev_device_close(d);
            return false;
        }
        if (ioctl(d->fd, EVIOCGABS(ABS_Y), &d->abs_y) < 0) {
            evdev_device_close(d);
            return false;
        }
    } else if (has_keys) {
        d->type = EVDEV_DEV_KEYBOARD;
    } else {
        evdev_device_close(d);
        return false;
    }

    if (d->type != EVDEV_DEV_KEYBOARD && (d->has_rel_x || d->has_abs_x) && has_keys) {
        d->has_keys = true;
    }

    if (grab) {
        if (ioctl(d->fd, EVIOCGRAB, (int)1) == 0) {
            d->grabbed = true;
        } else {
            fprintf(stderr, "[evdev] EVIOCGRAB failed on %s: %s (continuing ungrabbed)\n",
                    d->path, strerror(errno));
        }
    }

    fprintf(stderr, "[evdev] opened %s: %s (type %d)\n", d->path, d->name, d->type);
    return true;
}

static void evdev_process_key(Server *s, EvdevDevice *d, uint16_t code, int value, uint32_t time) {
    if (value == 2) return;

    if (code < 0x100) {
        if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
            bool down = value == 1;
            if (down) {
                s->keys[KEY_LEFTSHIFT] = true;
                s->keys[KEY_RIGHTSHIFT] = true;
            } else {
                s->keys[KEY_LEFTSHIFT] = false;
                s->keys[KEY_RIGHTSHIFT] = false;
            }
        }
        if (!app_global_key(s, code, value == 1)) {
            server_key(s, code, value == 1, time);
        }
    } else {
        bool down = value == 1;
        bool is_mouse_button = false;
        int btn_index = -1;

        switch (code) {
            case BTN_LEFT:   btn_index = 0; is_mouse_button = true; d->btn_left = down; break;
            case BTN_RIGHT:  btn_index = 1; is_mouse_button = true; d->btn_right = down; break;
            case BTN_MIDDLE: btn_index = 2; is_mouse_button = true; d->btn_middle = down; break;
            case BTN_SIDE:   btn_index = 3; is_mouse_button = true; d->btn_side = down; break;
            case BTN_EXTRA:  btn_index = 4; is_mouse_button = true; d->btn_extra = down; break;
            case BTN_TOUCH:
                if (d->type == EVDEV_DEV_SINGLE_TOUCH) {
                    d->st_finger.tracking_id = down ? 0 : -1;
                }
                return;
        }

        if (is_mouse_button && btn_index >= 0) {
            if (down) {
                d->held_button_mask |= (1u << btn_index);
            } else {
                d->held_button_mask &= ~(1u << btn_index);
            }
        }
    }
}

static void evdev_process_rel(Server *s, EvdevDevice *d, uint16_t code, int value) {
    switch (code) {
        case REL_X: d->rel_accum_x += value; break;
        case REL_Y: d->rel_accum_y += value; break;
        case REL_WHEEL: d->wheel_accum += value; break;
        case REL_HWHEEL: d->hwheel_accum += value; break;
    }
}

static void evdev_process_abs(Server *s, EvdevDevice *d, uint16_t code, int value) {
    switch (code) {
        case ABS_X: d->abs_x_val = value; break;
        case ABS_Y: d->abs_y_val = value; break;
        case ABS_MT_SLOT:
            if (value >= 0 && value < EVDEV_MAX_SLOTS) d->current_slot = value;
            break;
        case ABS_MT_TRACKING_ID:
            if (d->type == EVDEV_DEV_MULTITOUCH && d->current_slot >= 0 && d->current_slot < EVDEV_MAX_SLOTS) {
                d->mt_fingers[d->current_slot].tracking_id = value;
            }
            break;
        case ABS_MT_POSITION_X:
            if (d->type == EVDEV_DEV_MULTITOUCH && d->current_slot >= 0 && d->current_slot < EVDEV_MAX_SLOTS) {
                d->mt_fingers[d->current_slot].x = value;
            }
            break;
        case ABS_MT_POSITION_Y:
            if (d->type == EVDEV_DEV_MULTITOUCH && d->current_slot >= 0 && d->current_slot < EVDEV_MAX_SLOTS) {
                d->mt_fingers[d->current_slot].y = value;
            }
            break;
    }
}

static void evdev_chrome_down(Server *s, EvdevDevice *d, EvdevFinger *f, Hit hit, int x, int y, uint32_t time) {
    f->route = CD_TOUCH_ROUTE_CHROME;
    f->is_chrome_owner = true;
    s->evdev_chrome_touch_id = f - d->mt_fingers;
    if (d->type == EVDEV_DEV_SINGLE_TOUCH) {
        s->evdev_chrome_touch_id = EVDEV_MAX_SLOTS;
    }
    s->mouse_x = x;
    s->mouse_y = y;

    if (scene_wheel_button(s, hit, x, y, true, time)) {
        f->route = CD_TOUCH_ROUTE_WHEEL;
        return;
    }
    if (s->arrange_mode) {
        app_arrange_pointer_button(s, hit, BTN_LEFT, true);
        return;
    }
    if (hit.kind == HIT_FRAME || hit.kind == HIT_HANDLES_BUTTON) {
        app_frame_pointer_button(s, hit, BTN_LEFT, true);
        return;
    }
    if (hit.kind == HIT_BUTTON) {
        bool shift_held = s->keys[KEY_LEFTSHIFT] || s->keys[KEY_RIGHTSHIFT];
        app_launch(s, hit.button, shift_held);
        return;
    }
    if (hit.kind == HIT_CLOSE) {
        server_close_surface(hit.surface);
        return;
    }
    if (hit.kind == HIT_COG) {
        app_open_settings(s);
        return;
    }
    if (hit.kind == HIT_QUIT) {
        app_request_quit(s, false);
        return;
    }
    f->route = CD_TOUCH_ROUTE_LOOK;
    server_pointer_motion(s, (Hit){0}, time);
}

static void evdev_finger_down(Server *s, EvdevDevice *d, EvdevFinger *f, int x, int y, uint32_t time) {
    f->last_x = x;
    f->last_y = y;
    Hit hit = scene_pick(s, x, y, false);

    bool any_app_finger = false;
    for (int i = 0; i < EVDEV_MAX_SLOTS; i++) {
        if (d->mt_fingers[i].active && d->mt_fingers[i].route == CD_TOUCH_ROUTE_APP) {
            any_app_finger = true;
            break;
        }
    }
    if (d->st_finger.active && d->st_finger.route == CD_TOUCH_ROUTE_APP) {
        any_app_finger = true;
    }

    if (any_app_finger) {
        if (!s->arrange_mode && hit.kind == HIT_APP && server_touch_down(s, hit, f->touch_id, time)) {
            f->route = CD_TOUCH_ROUTE_APP;
        } else {
            f->route = CD_TOUCH_ROUTE_NONE;
        }
        return;
    }

    if (s->evdev_chrome_touch_id >= 0) {
        f->route = CD_TOUCH_ROUTE_NONE;
        return;
    }

    if (s->arrange_mode) {
        if (!s->arrange_dragging) {
            evdev_chrome_down(s, d, f, hit, x, y, time);
        }
        return;
    }

    if (hit.kind == HIT_APP) {
        if (server_touch_down(s, hit, f->touch_id, time)) {
            f->route = CD_TOUCH_ROUTE_APP;
            return;
        }
        if (s->pointer_grab || s->held_buttons) {
            f->route = CD_TOUCH_ROUTE_NONE;
            return;
        }
        f->route = CD_TOUCH_ROUTE_POINTER;
        s->evdev_chrome_touch_id = f - d->mt_fingers;
        if (d->type == EVDEV_DEV_SINGLE_TOUCH) {
            s->evdev_chrome_touch_id = EVDEV_MAX_SLOTS;
        }
        s->mouse_x = x;
        s->mouse_y = y;
        server_pointer_motion(s, hit, time);
        server_pointer_button(s, BTN_LEFT, true, time);
        return;
    }

    if (s->wheel_drag || s->frame_dragging) {
        f->route = CD_TOUCH_ROUTE_NONE;
        return;
    }

    evdev_chrome_down(s, d, f, hit, x, y, time);
}

static void evdev_finger_motion(Server *s, EvdevDevice *d, EvdevFinger *f, int x, int y, uint32_t time) {
    if (f->last_x == x && f->last_y == y) return;
    int dx = x - f->last_x;
    int dy = y - f->last_y;
    f->last_x = x;
    f->last_y = y;

    switch (f->route) {
        case CD_TOUCH_ROUTE_APP:
            server_touch_motion(s, f->touch_id, (float)x, (float)y, time);
            break;
        case CD_TOUCH_ROUTE_POINTER:
            s->mouse_x = x;
            s->mouse_y = y;
            Hit hit = scene_pick(s, x, y, s->pointer_grab != NULL);
            server_pointer_motion(s, hit, time);
            break;
        case CD_TOUCH_ROUTE_WHEEL:
            s->mouse_x = x;
            s->mouse_y = y;
            scene_wheel_motion(s, x, y, time);
            break;
        case CD_TOUCH_ROUTE_LOOK:
            s->mouse_x = x;
            s->mouse_y = y;
            s->target_yaw -= dx * 0.16f;
            s->target_pitch = clampf(s->target_pitch - dy * 0.13f, -35.0f, 32.0f);
            s->focus_view = false;
            s->arrange_last_x = x;
            s->arrange_last_y = y;
            s->frame_last_x = x;
            s->frame_last_y = y;
            break;
        case CD_TOUCH_ROUTE_CHROME:
            s->mouse_x = x;
            s->mouse_y = y;
            if (s->frame_dragging) {
                app_frame_pointer_motion(s, x, y);
            } else if (s->arrange_mode) {
                app_arrange_pointer_motion(s, x, y);
            }
            break;
        case CD_TOUCH_ROUTE_NONE:
            break;
    }
}

static void evdev_finger_up(Server *s, EvdevDevice *d, EvdevFinger *f, int x, int y, uint32_t time) {
    bool was_chrome_owner = f->is_chrome_owner;
    switch (f->route) {
        case CD_TOUCH_ROUTE_APP:
            server_touch_up(s, f->touch_id, time);
            evdev_util_touch_id_release(f->touch_id);
            break;
        case CD_TOUCH_ROUTE_POINTER:
            s->mouse_x = x;
            s->mouse_y = y;
            Hit hit = scene_pick(s, x, y, s->pointer_grab != NULL);
            server_pointer_motion(s, hit, time);
            server_pointer_button(s, BTN_LEFT, false, time);
            break;
        case CD_TOUCH_ROUTE_WHEEL:
            s->mouse_x = x;
            s->mouse_y = y;
            scene_wheel_button(s, (Hit){0}, x, y, false, time);
            break;
        case CD_TOUCH_ROUTE_CHROME:
            s->mouse_x = x;
            s->mouse_y = y;
            hit = scene_pick(s, x, y, false);
            if (s->arrange_mode) {
                app_arrange_pointer_button(s, hit, BTN_LEFT, false);
            } else if (s->frame_dragging) {
                app_frame_pointer_button(s, hit, BTN_LEFT, false);
            }
            break;
        case CD_TOUCH_ROUTE_LOOK:
        case CD_TOUCH_ROUTE_NONE:
            break;
    }
    if (was_chrome_owner) {
        s->evdev_chrome_touch_id = -1;
    }
    f->active = false;
    f->route = CD_TOUCH_ROUTE_NONE;
    f->touch_id = -1;
    f->tracking_id = -1;
    f->is_chrome_owner = false;
}

static void evdev_dispatch_pointer_motion(Server *s, EvdevDevice *d, uint32_t time) {
    int new_x = s->mouse_x;
    int new_y = s->mouse_y;
    bool moved = false;
    int dx = 0, dy = 0;

    if (d->type == EVDEV_DEV_REL_POINTER) {
        if (d->rel_accum_x != 0 || d->rel_accum_y != 0) {
            dx = d->rel_accum_x;
            dy = d->rel_accum_y;
            new_x += dx;
            new_y += dy;
            moved = true;
        }
    } else if (d->type == EVDEV_DEV_ABS_POINTER) {
        int scaled_x = evdev_util_scale_abs(d->abs_x_val, d->abs_x.minimum, d->abs_x.maximum, s->width);
        int scaled_y = evdev_util_scale_abs(d->abs_y_val, d->abs_y.minimum, d->abs_y.maximum, s->height);
        dx = scaled_x - s->mouse_x;
        dy = scaled_y - s->mouse_y;
        if (dx != 0 || dy != 0) {
            new_x = scaled_x;
            new_y = scaled_y;
            moved = true;
        }
    }

    if (new_x < 0) new_x = 0;
    if (new_x >= s->width) new_x = s->width - 1;
    if (new_y < 0) new_y = 0;
    if (new_y >= s->height) new_y = s->height - 1;

    if (moved) {
        s->mouse_x = new_x;
        s->mouse_y = new_y;
        if (s->wheel_drag) {
            scene_wheel_motion(s, s->mouse_x, s->mouse_y, time);
        } else if (s->middle_look) {
            s->target_yaw -= dx * 0.16f;
            s->target_pitch = clampf(s->target_pitch - dy * 0.13f, -35.0f, 32.0f);
            s->focus_view = false;
            s->arrange_last_x = s->mouse_x;
            s->arrange_last_y = s->mouse_y;
            s->frame_last_x = s->mouse_x;
            s->frame_last_y = s->mouse_y;
        } else if (s->frame_dragging) {
            app_frame_pointer_motion(s, s->mouse_x, s->mouse_y);
        } else if (s->arrange_mode) {
            app_arrange_pointer_motion(s, s->mouse_x, s->mouse_y);
        } else {
            Hit hit = scene_pick(s, s->mouse_x, s->mouse_y, s->pointer_grab != NULL);
            server_pointer_motion(s, hit, time);
        }
    }

    d->rel_accum_x = 0;
    d->rel_accum_y = 0;
}

static void evdev_dispatch_pointer_buttons(Server *s, EvdevDevice *d, uint32_t time) {
    uint32_t pressed = d->held_button_mask & ~d->sent_button_mask;
    uint32_t released = d->sent_button_mask & ~d->held_button_mask;
    uint32_t all_edges = pressed | released;

    for (int btn_idx = 0; btn_idx < 5; btn_idx++) {
        uint32_t bit = 1u << btn_idx;
        if (!(all_edges & bit)) continue;

        uint32_t button;
        switch (btn_idx) {
            case 0: button = BTN_LEFT; break;
            case 1: button = BTN_RIGHT; break;
            case 2: button = BTN_MIDDLE; break;
            case 3: button = BTN_SIDE; break;
            case 4: button = BTN_EXTRA; break;
            default: continue;
        }

        bool down = (pressed & bit) != 0;

        if (button == BTN_MIDDLE) {
            s->middle_look = down;
            d->middle_look_held = down;
            if (down) {
                server_pointer_motion(s, (Hit){0}, time);
            }
            continue;
        }

        Hit hit = scene_pick(s, s->mouse_x, s->mouse_y, s->pointer_grab != NULL);

        if (button == BTN_LEFT && scene_wheel_button(s, hit, s->mouse_x, s->mouse_y, down, time)) {
            continue;
        }

        if (s->arrange_mode) {
            app_arrange_pointer_button(s, hit, button, down);
            continue;
        }

        if (s->frame_dragging || hit.kind == HIT_FRAME || hit.kind == HIT_HANDLES_BUTTON) {
            app_frame_pointer_button(s, hit, button, down);
            continue;
        }

        server_pointer_motion(s, hit, time);

        if (down && button == BTN_LEFT) {
            if (hit.kind == HIT_BUTTON) {
                bool shift_held = s->keys[KEY_LEFTSHIFT] || s->keys[KEY_RIGHTSHIFT];
                app_launch(s, hit.button, shift_held);
                continue;
            }
            if (hit.kind == HIT_CLOSE) {
                server_close_surface(hit.surface);
                continue;
            }
            if (hit.kind == HIT_COG) {
                app_open_settings(s);
                continue;
            }
            if (hit.kind == HIT_QUIT) {
                app_request_quit(s, false);
                continue;
            }
        }

        if (down && button == BTN_RIGHT && hit.kind == HIT_BUTTON) {
            app_open_settings_slot(s, hit.button, true);
            continue;
        }

        if (hit.kind == HIT_APP || s->pointer_grab) {
            server_pointer_button(s, button, down, time);
        }
    }

    d->sent_button_mask = d->held_button_mask;
}

static void evdev_dispatch_wheel(Server *s, EvdevDevice *d, uint32_t time) {
    if (d->wheel_accum == 0 && d->hwheel_accum == 0) return;

    if (s->arrange_mode) {
        app_arrange_pointer_wheel(s, (float)d->wheel_accum);
    } else {
        Hit hit = scene_pick(s, s->mouse_x, s->mouse_y, false);
        if (hit.kind == HIT_APP) {
            server_pointer_axis(s, (double)d->hwheel_accum, (double)d->wheel_accum, time);
        } else {
            s->target_fov = clampf(s->target_fov - (float)d->wheel_accum * 2.0f,
                                   CD_DRIVER_FOV_MIN, CD_DRIVER_SCROLL_FOV_MAX);
        }
    }
    d->wheel_accum = 0;
    d->hwheel_accum = 0;
}

static void evdev_dispatch_multitouch(Server *s, EvdevDevice *d, uint32_t time) {
    for (int i = 0; i < EVDEV_MAX_SLOTS; i++) {
        EvdevFinger *f = &d->mt_fingers[i];
        int tx = evdev_util_scale_abs(f->x, d->abs_mt_x.minimum, d->abs_mt_x.maximum, s->width);
        int ty = evdev_util_scale_abs(f->y, d->abs_mt_y.minimum, d->abs_mt_y.maximum, s->height);
        if (tx < 0) tx = 0;
        if (tx >= s->width) tx = s->width - 1;
        if (ty < 0) ty = 0;
        if (ty >= s->height) ty = s->height - 1;

        if (f->tracking_id == -1 && f->active) {
            evdev_finger_up(s, d, f, tx, ty, time);
        } else if (f->tracking_id != -1 && !f->active) {
            int tid = evdev_util_touch_id_alloc();
            if (tid >= 0) {
                f->touch_id = tid;
                f->active = true;
                evdev_finger_down(s, d, f, tx, ty, time);
            } else {
                f->route = CD_TOUCH_ROUTE_NONE;
                f->touch_id = -1;
                f->active = true;
            }
        } else if (f->tracking_id != -1 && f->active) {
            evdev_finger_motion(s, d, f, tx, ty, time);
        }
    }
}

static void evdev_dispatch_single_touch(Server *s, EvdevDevice *d, uint32_t time) {
    EvdevFinger *f = &d->st_finger;
    int tx = evdev_util_scale_abs(d->abs_x_val, d->abs_x.minimum, d->abs_x.maximum, s->width);
    int ty = evdev_util_scale_abs(d->abs_y_val, d->abs_y.minimum, d->abs_y.maximum, s->height);
    if (tx < 0) tx = 0;
    if (tx >= s->width) tx = s->width - 1;
    if (ty < 0) ty = 0;
    if (ty >= s->height) ty = s->height - 1;

    if (f->tracking_id == -1 && f->active) {
        evdev_finger_up(s, d, f, tx, ty, time);
    } else if (f->tracking_id != -1 && !f->active) {
        int tid = evdev_util_touch_id_alloc();
        if (tid >= 0) {
            f->touch_id = tid;
            f->active = true;
            evdev_finger_down(s, d, f, tx, ty, time);
        } else {
            f->route = CD_TOUCH_ROUTE_NONE;
            f->touch_id = -1;
            f->active = true;
        }
    } else if (f->tracking_id != -1 && f->active) {
        evdev_finger_motion(s, d, f, tx, ty, time);
    }
}

static void evdev_dispatch_syn(Server *s, EvdevDevice *d, uint32_t time) {
    if (d->type == EVDEV_DEV_REL_POINTER || d->type == EVDEV_DEV_ABS_POINTER) {
        evdev_dispatch_pointer_motion(s, d, time);
        evdev_dispatch_pointer_buttons(s, d, time);
        evdev_dispatch_wheel(s, d, time);
    } else if (d->type == EVDEV_DEV_MULTITOUCH) {
        evdev_dispatch_multitouch(s, d, time);
    } else if (d->type == EVDEV_DEV_SINGLE_TOUCH) {
        evdev_dispatch_single_touch(s, d, time);
    }
    if (d->has_keys) {
        /* keys already dispatched in process_key */
    }
}

static void evdev_handle_syn_dropped(Server *s, EvdevDevice *d, uint32_t time) {
    static bool logged_dropped[EVDEV_MAX_DEVICES] = {0};
    int idx = -1;
    EvdevState *st = evdev_state(s);
    if (st) {
        for (int i = 0; i < st->device_count; i++) {
            if (&st->devices[i] == d) { idx = i; break; }
        }
    }
    if (idx >= 0 && !logged_dropped[idx]) {
        fprintf(stderr, "[evdev] SYN_DROPPED on %s, resetting device state\n", d->path);
        logged_dropped[idx] = true;
    } else if (idx < 0) {
        fprintf(stderr, "[evdev] SYN_DROPPED on unknown device, resetting state\n");
    }

    uint32_t released_mask = d->sent_button_mask & d->held_button_mask;
    if (released_mask) {
        if (released_mask & (1u << 0)) server_pointer_button(s, BTN_LEFT, false, time);
        if (released_mask & (1u << 1)) server_pointer_button(s, BTN_RIGHT, false, time);
        if (released_mask & (1u << 2)) server_pointer_button(s, BTN_MIDDLE, false, time);
        if (released_mask & (1u << 3)) server_pointer_button(s, BTN_SIDE, false, time);
        if (released_mask & (1u << 4)) server_pointer_button(s, BTN_EXTRA, false, time);
    }
    d->held_button_mask = 0;
    d->sent_button_mask = 0;
    d->btn_left = d->btn_right = d->btn_middle = d->btn_side = d->btn_extra = false;

    for (int i = 0; i < EVDEV_MAX_SLOTS; i++) {
        evdev_release_finger(s, &d->mt_fingers[i], time, d->mt_fingers[i].is_chrome_owner);
    }
    evdev_release_finger(s, &d->st_finger, time, d->st_finger.is_chrome_owner);

    if (d->middle_look_held) {
        s->middle_look = false;
        d->middle_look_held = false;
    }

    d->rel_accum_x = d->rel_accum_y = 0;
    d->wheel_accum = d->hwheel_accum = 0;
    d->current_slot = 0;
    for (int i = 0; i < EVDEV_MAX_SLOTS; i++) {
        d->mt_fingers[i].tracking_id = -1;
    }
    d->st_finger.tracking_id = -1;

    if (d->has_keys) {
        server_release_keys(s, time);
    }
}

bool backend_evdev_init(Server *s) {
    EvdevState *st = calloc(1, sizeof(*st));
    if (!st) {
        fprintf(stderr, "[evdev] out of memory\n");
        return false;
    }
    st->evdev_enabled = true;
    st->grab_input = s->fbdev_grab_input;
    st->explicit_count = s->fbdev_input_device_count;
    for (int i = 0; i < st->explicit_count; i++) {
        snprintf(st->explicit_paths[i], sizeof(st->explicit_paths[i]), "%s", s->fbdev_input_devices[i]);
    }
    s->evdev_state = st;
    s->evdev_chrome_touch_id = -1;

    const char *paths[EVDEV_MAX_DEVICES];
    int path_count = 0;

    if (st->explicit_count > 0) {
        for (int i = 0; i < st->explicit_count && path_count < EVDEV_MAX_DEVICES; i++) {
            paths[path_count++] = st->explicit_paths[i];
        }
    } else {
        DIR *dir = opendir("/dev/input");
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) && path_count < EVDEV_MAX_DEVICES) {
                if (strncmp(ent->d_name, "event", 5) == 0) {
                    char full_path[512];
                    snprintf(full_path, sizeof(full_path), "/dev/input/%s", ent->d_name);
                    paths[path_count++] = strdup(full_path);
                }
            }
            closedir(dir);
        }
    }

    int opened = 0;
    for (int i = 0; i < path_count; i++) {
        const char *path = paths[i];
        if (st->device_count >= EVDEV_MAX_DEVICES) break;
        EvdevDevice *d = &st->devices[st->device_count];
        memset(d, 0, sizeof(*d));
        d->fd = -1;
        d->current_slot = 0;
        for (int j = 0; j < EVDEV_MAX_SLOTS; j++) {
            d->mt_fingers[j].tracking_id = -1;
            d->mt_fingers[j].route = CD_TOUCH_ROUTE_NONE;
        }
        d->st_finger.tracking_id = -1;
        d->st_finger.route = CD_TOUCH_ROUTE_NONE;
        if (evdev_open_device(d, path, st->grab_input)) {
            st->device_count++;
            opened++;
            if (d->type == EVDEV_DEV_MULTITOUCH || d->type == EVDEV_DEV_SINGLE_TOUCH) {
                s->touch_available = true;
            }
        }
        if (path != st->explicit_paths[0]) free((void *)path);
    }

    if (opened == 0) {
        fprintf(stderr, "[evdev] no input devices opened (need read access to /dev/input/event*, usually the `input` group); output continues, mirror still available\n");
    }

    evdev_util_touch_id_reset();
    return true;
}

void backend_evdev_poll(Server *s) {
    EvdevState *st = evdev_state(s);
    if (!st || !st->evdev_enabled || st->device_count == 0) return;

    uint32_t time = (uint32_t)(s->time * 1000.0);

    for (int i = 0; i < st->device_count; i++) {
        EvdevDevice *d = &st->devices[i];
        if (d->fd < 0) continue;

        struct input_event ev[64];
        while (true) {
            ssize_t n = read(d->fd, ev, sizeof(ev));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == ENODEV || errno == EIO) {
                    fprintf(stderr, "[evdev] device %s removed: %s\n", d->path, strerror(errno));
                    evdev_release_device_state(s, d, time);
                    evdev_device_close(d);
                    if (i < st->device_count - 1) {
                        memmove(&st->devices[i], &st->devices[i + 1],
                                (st->device_count - i - 1) * sizeof(EvdevDevice));
                    }
                    st->device_count--;
                    i--;
                    break;
                }
                break;
            }
            int count = n / sizeof(struct input_event);
            for (int j = 0; j < count; j++) {
                struct input_event *e = &ev[j];
                switch (e->type) {
                    case EV_KEY:
                        evdev_process_key(s, d, e->code, e->value, time);
                        break;
                    case EV_REL:
                        evdev_process_rel(s, d, e->code, e->value);
                        break;
                    case EV_ABS:
                        evdev_process_abs(s, d, e->code, e->value);
                        break;
                    case EV_SYN:
                        if (e->code == SYN_REPORT) {
                            evdev_dispatch_syn(s, d, time);
                        } else if (e->code == SYN_DROPPED) {
                            evdev_handle_syn_dropped(s, d, time);
                        }
                        break;
                }
            }
        }
    }
}

void backend_evdev_shutdown(Server *s) {
    EvdevState *st = evdev_state(s);
    if (!st) return;

    uint32_t time = (uint32_t)(s->time * 1000.0);

    for (int i = 0; i < st->device_count; i++) {
        EvdevDevice *d = &st->devices[i];
        evdev_release_device_state(s, d, time);
        evdev_device_close(d);
    }

    free(st);
    s->evdev_state = NULL;
    s->evdev_chrome_touch_id = -1;
}