#ifndef EVDEV_UTIL_H
#define EVDEV_UTIL_H

#include <stdbool.h>
#include <stdint.h>

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