#ifndef CHROMEDRIVE_XKB_KEYBOARD_H
#define CHROMEDRIVE_XKB_KEYBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>
#include "cdconfig.h"

/* NULL means that the setting is absent, so xkbcommon retains its normal
 * XKB_DEFAULT_* / system-default behavior. Empty strings are preserved rather
 * than collapsed to NULL: xkbcommon defaults empty non-options fields, while
 * options="" deliberately disables inherited options. */
typedef struct {
    char *rules;
    char *model;
    char *layouts;
    char *variants;
    char *options;
} CdXkbConfig;

enum {
    CD_XKB_LED_NUM_LOCK = 1u << 0,
    CD_XKB_LED_CAPS_LOCK = 1u << 1,
    CD_XKB_LED_SCROLL_LOCK = 1u << 2,
    CD_XKB_LED_COMPOSE = 1u << 3,
    CD_XKB_LED_KANA = 1u << 4,
};
#define CD_XKB_LED_COUNT 5

/* The fd is a sealed, read-only memfd containing the NUL-terminated XKB V1
 * keymap.  active_config is the successfully compiled configuration, never a
 * rejected reload.  The structure must be zero-initialized with fd == -1. */
typedef struct {
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    int fd;
    uint32_t size;
    CdXkbConfig active_config;
    /* Resolved again for every replacement keymap.  XKB_LED_INVALID entries
     * are normal for maps without Compose/Kana or optional lock LEDs. */
    xkb_led_index_t led_indices[CD_XKB_LED_COUNT];
} CdXkbKeyboard;

void cd_xkb_config_clear(CdXkbConfig *config);
bool cd_xkb_config_copy(CdXkbConfig *out, const CdXkbConfig *source);
bool cd_xkb_config_from_store(CdXkbConfig *out, const CdConfig *store,
                              char *error, size_t error_size);
bool cd_xkb_config_equal(const CdXkbConfig *a, const CdXkbConfig *b);
/* True only when every field is absent (NULL), not merely an empty string. */
bool cd_xkb_config_is_absent(const CdXkbConfig *config);
void cd_xkb_config_names(const CdXkbConfig *config,
                         struct xkb_rule_names *names);

bool cd_xkb_keyboard_init(CdXkbKeyboard *keyboard,
                          const CdXkbConfig *config,
                          char *error, size_t error_size);
/* Compile and allocate everything before changing the active keyboard.
 * pressed_keys is the compositor's physical evdev bitmap; currently held
 * keys are replayed into the new state just as wlroots does.  On any failure
 * the old keymap/state/fd/configuration remain untouched and *changed is
 * false.  An identical active configuration is a successful no-op. */
bool cd_xkb_keyboard_reconfigure(CdXkbKeyboard *keyboard,
                                 const CdXkbConfig *config,
                                 const bool pressed_keys[256],
                                 bool *changed,
                                 char *error, size_t error_size);
void cd_xkb_keyboard_finish(CdXkbKeyboard *keyboard);
/* Seat-wide XKB indicator state, using the CD_XKB_LED_* bit assignments.
 * Group indicators (grp_led:*) are already reflected by xkbcommon; no layout
 * policy is duplicated here.  Missing LEDs and an uninitialized state return
 * no active bits rather than treating xkbcommon's -1 error as true. */
uint32_t cd_xkb_keyboard_led_mask(const CdXkbKeyboard *keyboard);

#endif /* CHROMEDRIVE_XKB_KEYBOARD_H */
