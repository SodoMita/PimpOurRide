#include "evdev_util.h"
#include <assert.h>
#include <linux/input-event-codes.h>

enum {
    EVDEV_VT_MOD_LCTRL = 1u << 0,
    EVDEV_VT_MOD_RCTRL = 1u << 1,
    EVDEV_VT_MOD_LALT  = 1u << 2,
    EVDEV_VT_MOD_RALT  = 1u << 3
};

void evdev_vt_shortcut_update_modifiers(EvdevVtShortcut *state,
                                        uint16_t code, int value) {
    if (!state) return;
    uint8_t bit = 0;
    switch (code) {
    case KEY_LEFTCTRL:  bit = EVDEV_VT_MOD_LCTRL; break;
    case KEY_RIGHTCTRL: bit = EVDEV_VT_MOD_RCTRL; break;
    case KEY_LEFTALT:   bit = EVDEV_VT_MOD_LALT;  break;
    case KEY_RIGHTALT:  bit = EVDEV_VT_MOD_RALT;  break;
    default: return;
    }
    if (value) state->modifiers |= bit;
    else state->modifiers &= (uint8_t)~bit;
}

bool evdev_vt_shortcut_ctrl(const EvdevVtShortcut *state) {
    return state && (state->modifiers & (EVDEV_VT_MOD_LCTRL | EVDEV_VT_MOD_RCTRL));
}

bool evdev_vt_shortcut_alt(const EvdevVtShortcut *state) {
    return state && (state->modifiers & (EVDEV_VT_MOD_LALT | EVDEV_VT_MOD_RALT));
}

static int evdev_vt_fkey_index(uint16_t code) {
    /* Linux KEY_F11/KEY_F12 are 87/88, not contiguous with KEY_F1..F10. */
    if (code >= KEY_F1 && code <= KEY_F10) return (int)(code - KEY_F1);
    if (code == KEY_F11) return 10;
    if (code == KEY_F12) return 11;
    return -1;
}

int evdev_vt_shortcut(EvdevVtShortcut *state, uint16_t code, int value,
                      bool ctrl_down, bool alt_down) {
    int index = evdev_vt_fkey_index(code);
    if (!state || index < 0) return 0;
    uint16_t bit = (uint16_t)(1u << index);
    if (value == 2) return (state->consumed_mask & bit) ? -1 : 0;
    if (value == 0) {
        if (!(state->consumed_mask & bit)) return 0;
        state->consumed_mask &= (uint16_t)~bit;
        return -1;
    }
    if (value != 1 || (state->consumed_mask & bit)) return (state->consumed_mask & bit) ? -1 : 0;
    if (!ctrl_down || !alt_down) return 0;
    state->consumed_mask |= bit;
    return index + 1;
}

void evdev_vt_shortcut_reset(EvdevVtShortcut *state) {
    if (state) *state = (EvdevVtShortcut){0};
}

static bool touch_id_used[10] = {0};

int evdev_util_scale_abs(int value, int min, int max, int out_size) {
    if (max <= min || out_size <= 0) return 0;
    if (value <= min) return 0;
    if (value >= max) return out_size - 1;
    return (int)(((long long)(value - min) * (out_size - 1)) / (max - min));
}

int evdev_util_touch_id_alloc(void) {
    for (int i = 0; i < 10; i++) {
        if (!touch_id_used[i]) {
            touch_id_used[i] = true;
            return i;
        }
    }
    return -1;
}

void evdev_util_touch_id_release(int id) {
    if (id >= 0 && id < 10) {
        touch_id_used[id] = false;
    }
}

void evdev_util_touch_id_reset(void) {
    for (int i = 0; i < 10; i++) {
        touch_id_used[i] = false;
    }
}

_Static_assert(10 == 10, "evdev_util touch pool size must match CD_MAX_TOUCH_POINTS");