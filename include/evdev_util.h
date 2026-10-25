#ifndef EVDEV_UTIL_H
#define EVDEV_UTIL_H

#include <stdbool.h>
#include <stdint.h>

/* Per-device tracking for the compositor-owned Ctrl+Alt+F1..F12 VT chord.
 * A consumed key release is still consumed after the modifiers are let go,
 * and auto-repeat cannot retrigger a switch. */
typedef struct {
    uint16_t consumed_mask; /* one bit per F1..F12, independent of modifier release order */
    uint8_t modifiers;     /* physical Ctrl/Alt state for this evdev device only */
} EvdevVtShortcut;

/* Track modifiers in the evdev device state, never in Server.keys: SDL/VNC
 * also update Server.keys and must not be able to complete a host VT chord. */
void evdev_vt_shortcut_update_modifiers(EvdevVtShortcut *state,
                                        uint16_t code, int value);
bool evdev_vt_shortcut_ctrl(const EvdevVtShortcut *state);
bool evdev_vt_shortcut_alt(const EvdevVtShortcut *state);

/* Returns 1..12 for a newly consumed F-key press (the target VT), -1 for a
 * consumed release/repeat, and 0 when the event is not a shortcut and should
 * follow the normal key path. */
int evdev_vt_shortcut(EvdevVtShortcut *state, uint16_t code, int value,
                      bool ctrl_down, bool alt_down);
void evdev_vt_shortcut_reset(EvdevVtShortcut *state);

/* Scale an absolute axis value from its device range [min, max] into
 * output pixel coordinates [0, out_size - 1]. Degenerate ranges
 * (max <= min or out_size <= 0) return 0. Clamped to bounds. */
int evdev_util_scale_abs(int value, int min, int max, int out_size);

/* Touch-id pool for up to 10 simultaneous contacts (CD_MAX_TOUCH_POINTS).
 * Allocates the lowest free id on success, returns -1 on exhaustion.
 * Double-release is safe (idempotent). */
int evdev_util_touch_id_alloc(void);
void evdev_util_touch_id_release(int id);
void evdev_util_touch_id_reset(void);

#endif