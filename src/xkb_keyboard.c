#define _GNU_SOURCE
#include "xkb_keyboard.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-names.h>

static void set_error(char *error, size_t size, const char *fmt, ...)
{
    if (!error || !size) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, size, fmt, args);
    va_end(args);
}

static const char *const led_names[CD_XKB_LED_COUNT] = {
    XKB_LED_NAME_NUM,
    XKB_LED_NAME_CAPS,
    XKB_LED_NAME_SCROLL,
    /* Older xkbcommon-names.h releases omit constants for these two, while
     * the XKB protocol names and wlroots/Weston behavior are stable. */
    "Compose",
    "Kana",
};

static void update_led_indices(CdXkbKeyboard *keyboard)
{
    for (size_t i = 0; i < CD_XKB_LED_COUNT; i++)
        keyboard->led_indices[i] = xkb_keymap_led_get_index(
            keyboard->keymap, led_names[i]);
}

uint32_t cd_xkb_keyboard_led_mask(const CdXkbKeyboard *keyboard)
{
    if (!keyboard || !keyboard->state) return 0;
    uint32_t mask = 0;
    for (size_t i = 0; i < CD_XKB_LED_COUNT; i++) {
        if (keyboard->led_indices[i] != XKB_LED_INVALID &&
            xkb_state_led_index_is_active(keyboard->state,
                                          keyboard->led_indices[i]) > 0)
            mask |= 1u << i;
    }
    return mask;
}

static char *duplicate_optional(const char *value)
{
    return value ? strdup(value) : NULL;
}

void cd_xkb_config_clear(CdXkbConfig *config)
{
    if (!config) return;
    free(config->rules);
    free(config->model);
    free(config->layouts);
    free(config->variants);
    free(config->options);
    memset(config, 0, sizeof(*config));
}

bool cd_xkb_config_copy(CdXkbConfig *out, const CdXkbConfig *source)
{
    if (!out || !source) return false;
    CdXkbConfig next = {0};
    next.rules = duplicate_optional(source->rules);
    next.model = duplicate_optional(source->model);
    next.layouts = duplicate_optional(source->layouts);
    next.variants = duplicate_optional(source->variants);
    next.options = duplicate_optional(source->options);
    if ((source->rules && !next.rules) || (source->model && !next.model) ||
        (source->layouts && !next.layouts) ||
        (source->variants && !next.variants) ||
        (source->options && !next.options)) {
        cd_xkb_config_clear(&next);
        return false;
    }
    cd_xkb_config_clear(out);
    *out = next;
    return true;
}

static bool copy_store_value(char **out, const CdConfig *store,
                             const char *key, char *error, size_t error_size)
{
    if (!cd_config_has(store, "keyboard", key)) {
        *out = NULL;
        return true;
    }
    const char *value = cd_config_get_string(store, "keyboard", key, "");
    *out = strdup(value ? value : "");
    if (!*out) {
        set_error(error, error_size, "out of memory copying keyboard.%s", key);
        return false;
    }
    return true;
}

bool cd_xkb_config_from_store(CdXkbConfig *out, const CdConfig *store,
                              char *error, size_t error_size)
{
    if (!out || !store) {
        set_error(error, error_size, "invalid keyboard configuration store");
        return false;
    }
    CdXkbConfig next = {0};
    if (!copy_store_value(&next.rules, store, "rules", error, error_size) ||
        !copy_store_value(&next.model, store, "model", error, error_size) ||
        !copy_store_value(&next.layouts, store, "layouts", error, error_size) ||
        !copy_store_value(&next.variants, store, "variants", error, error_size) ||
        !copy_store_value(&next.options, store, "options", error, error_size)) {
        cd_xkb_config_clear(&next);
        return false;
    }
    cd_xkb_config_clear(out);
    *out = next;
    return true;
}

static bool optional_equal(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

bool cd_xkb_config_equal(const CdXkbConfig *a, const CdXkbConfig *b)
{
    return a && b && optional_equal(a->rules, b->rules) &&
        optional_equal(a->model, b->model) &&
        optional_equal(a->layouts, b->layouts) &&
        optional_equal(a->variants, b->variants) &&
        optional_equal(a->options, b->options);
}

bool cd_xkb_config_is_absent(const CdXkbConfig *config)
{
    return !config || (!config->rules && !config->model && !config->layouts &&
                       !config->variants && !config->options);
}

void cd_xkb_config_names(const CdXkbConfig *config,
                         struct xkb_rule_names *names)
{
    memset(names, 0, sizeof(*names));
    if (!config) return;
    names->rules = config->rules;
    names->model = config->model;
    names->layout = config->layouts;
    names->variant = config->variants;
    names->options = config->options;
}

static bool write_all(int fd, const char *data, size_t size,
                      char *error, size_t error_size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            set_error(error, error_size, "cannot write XKB keymap memfd: %s",
                      n < 0 ? strerror(errno) : "short write");
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

static int keymap_memfd(const char *string, size_t size,
                        char *error, size_t error_size)
{
    int fd = memfd_create("chromedrive-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        set_error(error, error_size, "cannot create XKB keymap memfd: %s",
                  strerror(errno));
        return -1;
    }
    if (!write_all(fd, string, size, error, error_size)) {
        close(fd);
        return -1;
    }
    /* wl_keyboard.keymap requires a read-only fd.  Sealing the completed
     * memfd prevents both compositor accidents and client-side writes from
     * changing a keymap already sent to another client. */
    if (fcntl(fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0) {
        set_error(error, error_size, "cannot seal XKB keymap memfd read-only: %s",
                  strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int replay_rank(uint32_t key)
{
    /* Standard grp chords are edge-triggered: replaying Space before Meta, or
     * Shift before Alt, is not equivalent to the physical chord. Restore the
     * usual modifier-first order, then Shift, then action/lock/ordinary keys. */
    switch (key) {
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT:
    case KEY_LEFTMETA: case KEY_RIGHTMETA:
        return 0;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
        return 1;
    default:
        return 2;
    }
}

static bool build_keymap(CdXkbKeyboard *keyboard, const CdXkbConfig *config,
                         const bool pressed_keys[256],
                         struct xkb_keymap **new_keymap,
                         struct xkb_state **new_state, int *new_fd,
                         uint32_t *new_size, char *error, size_t error_size)
{
    struct xkb_rule_names names;
    cd_xkb_config_names(config, &names);
    struct xkb_keymap *map = xkb_keymap_new_from_names(
        keyboard->context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) {
        set_error(error, error_size,
                  "could not compile XKB keymap (see xkbcommon diagnostics above)");
        return false;
    }
    struct xkb_state *state = xkb_state_new(map);
    if (!state) {
        set_error(error, error_size, "could not create XKB state");
        xkb_keymap_unref(map);
        return false;
    }
    char *string = xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!string) {
        set_error(error, error_size, "could not serialize XKB keymap");
        xkb_state_unref(state);
        xkb_keymap_unref(map);
        return false;
    }
    size_t length = strlen(string) + 1;
    if (length > UINT32_MAX) {
        set_error(error, error_size, "XKB keymap is too large for wl_keyboard");
        free(string);
        xkb_state_unref(state);
        xkb_keymap_unref(map);
        return false;
    }
    int fd = keymap_memfd(string, length, error, error_size);
    free(string);
    if (fd < 0) {
        xkb_state_unref(state);
        xkb_keymap_unref(map);
        return false;
    }

    /* Keep physical key state across a live keymap replacement.  Key releases
     * retain their evdev numbers, and replaying held modifiers/group-switch
     * keys prevents a stuck client modifier or an invented release. Modifier
     * ranks preserve standard grp chord edges better than numeric keycode
     * order. Locked state intentionally starts from the new map, matching
     * wlroots' normal keymap replacement semantics. */
    if (pressed_keys) {
        for (int rank = 0; rank <= 2; rank++) {
            for (uint32_t key = 0; key < 256; key++) {
                if (pressed_keys[key] && replay_rank(key) == rank)
                    xkb_state_update_key(state, key + 8, XKB_KEY_DOWN);
            }
        }
    }
    *new_keymap = map;
    *new_state = state;
    *new_fd = fd;
    *new_size = (uint32_t)length;
    return true;
}

bool cd_xkb_keyboard_init(CdXkbKeyboard *keyboard,
                          const CdXkbConfig *config,
                          char *error, size_t error_size)
{
    if (!keyboard || keyboard->context || keyboard->keymap || keyboard->state) {
        set_error(error, error_size, "XKB keyboard is already initialized");
        return false;
    }
    keyboard->fd = -1;
    keyboard->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!keyboard->context) {
        set_error(error, error_size, "could not create XKB context");
        return false;
    }
    bool changed = false;
    if (!cd_xkb_keyboard_reconfigure(keyboard, config, NULL, &changed,
                                     error, error_size)) {
        cd_xkb_keyboard_finish(keyboard);
        return false;
    }
    return changed && keyboard->keymap && keyboard->state && keyboard->fd >= 0;
}

bool cd_xkb_keyboard_reconfigure(CdXkbKeyboard *keyboard,
                                 const CdXkbConfig *config,
                                 const bool pressed_keys[256],
                                 bool *changed,
                                 char *error, size_t error_size)
{
    if (changed) *changed = false;
    if (!keyboard || !keyboard->context || !config) {
        set_error(error, error_size, "XKB keyboard is not initialized");
        return false;
    }
    if (keyboard->keymap && cd_xkb_config_equal(&keyboard->active_config, config))
        return true;

    CdXkbConfig next_config = {0};
    if (!cd_xkb_config_copy(&next_config, config)) {
        set_error(error, error_size, "out of memory copying XKB configuration");
        return false;
    }
    struct xkb_keymap *map = NULL;
    struct xkb_state *state = NULL;
    int fd = -1;
    uint32_t size = 0;
    if (!build_keymap(keyboard, config, pressed_keys, &map, &state, &fd, &size,
                      error, error_size)) {
        cd_xkb_config_clear(&next_config);
        return false;
    }

    struct xkb_keymap *old_map = keyboard->keymap;
    struct xkb_state *old_state = keyboard->state;
    int old_fd = keyboard->fd;
    keyboard->keymap = map;
    keyboard->state = state;
    keyboard->fd = fd;
    keyboard->size = size;
    /* LED indices belong to this exact map, never to the rejected or previous
     * configuration.  Resolve only after the transactional commit begins. */
    update_led_indices(keyboard);
    cd_xkb_config_clear(&keyboard->active_config);
    keyboard->active_config = next_config;
    if (old_state) xkb_state_unref(old_state);
    if (old_map) xkb_keymap_unref(old_map);
    if (old_fd >= 0) close(old_fd);
    if (changed) *changed = true;
    return true;
}

void cd_xkb_keyboard_finish(CdXkbKeyboard *keyboard)
{
    if (!keyboard) return;
    if (keyboard->state) xkb_state_unref(keyboard->state);
    if (keyboard->keymap) xkb_keymap_unref(keyboard->keymap);
    if (keyboard->context) xkb_context_unref(keyboard->context);
    if (keyboard->fd >= 0) close(keyboard->fd);
    cd_xkb_config_clear(&keyboard->active_config);
    memset(keyboard, 0, sizeof(*keyboard));
    keyboard->fd = -1;
}
