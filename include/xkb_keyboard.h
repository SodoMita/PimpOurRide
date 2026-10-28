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

#endif /* CHROMEDRIVE_XKB_KEYBOARD_H */
