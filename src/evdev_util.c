#include "evdev_util.h"
#include <assert.h>

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