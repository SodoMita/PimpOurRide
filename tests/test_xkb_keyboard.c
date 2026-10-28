#define _GNU_SOURCE
#include "xkb_keyboard.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
        failures++; \
    } \
} while (0)

static void set_config(CdXkbConfig *out, const char *rules, const char *model,
                       const char *layouts, const char *variants, const char *options)
{
    CdXkbConfig literal = {
        .rules = (char *)rules, .model = (char *)model, .layouts = (char *)layouts,
        .variants = (char *)variants, .options = (char *)options,
    };
    CHECK(cd_xkb_config_copy(out, &literal), "copy test configuration");
}

static xkb_layout_index_t toggle_group(CdXkbKeyboard *keyboard)
{
    xkb_state_update_key(keyboard->state, KEY_LEFTALT + 8, XKB_KEY_DOWN);
    xkb_state_update_key(keyboard->state, KEY_LEFTSHIFT + 8, XKB_KEY_DOWN);
    xkb_layout_index_t group = xkb_state_serialize_layout(
        keyboard->state, XKB_STATE_LAYOUT_EFFECTIVE);
    xkb_state_update_key(keyboard->state, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
    xkb_state_update_key(keyboard->state, KEY_LEFTALT + 8, XKB_KEY_UP);
    return group;
}

static void lock_key(CdXkbKeyboard *keyboard, uint32_t key)
{
    xkb_state_update_key(keyboard->state, key + 8, XKB_KEY_DOWN);
    xkb_state_update_key(keyboard->state, key + 8, XKB_KEY_UP);
}

static void test_led_mask(void)
{
    /* Explicit RMLVO keeps this independent of the environment-default tests
     * in main. grp_led:caps must come from XKB's effective group, not from a
     * second compositor-side layout counter. */
    CdXkbConfig config = {0};
    set_config(&config, "evdev", "pc105", "us,ru", ",",
               "grp:alt_shift_toggle,grp_led:caps,scrolllock:mod3");
    CdXkbKeyboard keyboard = {.fd = -1};
    char error[512] = "";
    CHECK(cd_xkb_keyboard_init(&keyboard, &config, error, sizeof(error)), error);
    if (!keyboard.state) {
        cd_xkb_config_clear(&config);
        return;
    }
    CHECK(cd_xkb_keyboard_led_mask(&keyboard) == 0,
          "new XKB state starts with all supported LEDs off");
    CHECK(cd_xkb_keyboard_led_mask(NULL) == 0,
          "uninitialized LED query is safely off");

    /* grp_led:caps deliberately owns the Caps indicator, so ordinary Caps
     * Lock semantics are tested on a separate map below. Num is independent. */
    lock_key(&keyboard, KEY_NUMLOCK);
    CHECK(cd_xkb_keyboard_led_mask(&keyboard) == CD_XKB_LED_NUM_LOCK,
          "Num Lock activates only the XKB Num LED");
    lock_key(&keyboard, KEY_NUMLOCK);
    lock_key(&keyboard, KEY_SCROLLLOCK);
    CHECK(cd_xkb_keyboard_led_mask(&keyboard) == CD_XKB_LED_SCROLL_LOCK,
          "scrolllock:mod3 binds the physical key to its XKB LED");
    lock_key(&keyboard, KEY_SCROLLLOCK);

    CHECK(toggle_group(&keyboard) == 1, "grp_led test selects second group");
    CHECK(cd_xkb_keyboard_led_mask(&keyboard) == CD_XKB_LED_CAPS_LOCK,
          "grp_led:caps follows effective layout without Caps Lock");
    CHECK(toggle_group(&keyboard) == 0, "grp_led test returns to first group");
    CHECK(cd_xkb_keyboard_led_mask(&keyboard) == 0,
          "group LED turns off with alternative layout");

    CdXkbConfig next = {0};
    set_config(&next, "evdev", "pc105", "us,ru", ",",
               "grp:alt_shift_toggle,grp_led:num");
    bool changed = false;
    CHECK(cd_xkb_keyboard_reconfigure(&keyboard, &next, NULL, &changed,
                                      error, sizeof(error)), error);
    CHECK(changed && cd_xkb_keyboard_led_mask(&keyboard) == 0,
          "replacement map resolves a fresh LED index set");
    CHECK(toggle_group(&keyboard) == 1 &&
          cd_xkb_keyboard_led_mask(&keyboard) == CD_XKB_LED_NUM_LOCK,
          "live reload moves grp_led from Caps to Num");

    xkb_led_index_t old_indices[CD_XKB_LED_COUNT];
    memcpy(old_indices, keyboard.led_indices, sizeof(old_indices));
    CdXkbConfig bad = {0};
    set_config(&bad, "evdev", "pc105", "chromedrive_no_such_led_layout", ",",
               "grp_led:scroll");
    CHECK(!cd_xkb_keyboard_reconfigure(&keyboard, &bad, NULL, &changed,
                                       error, sizeof(error)),
          "invalid LED reload is rejected");
    CHECK(!changed && cd_xkb_keyboard_led_mask(&keyboard) == CD_XKB_LED_NUM_LOCK &&
          memcmp(old_indices, keyboard.led_indices, sizeof(old_indices)) == 0,
          "invalid reload retains active LED state and indices");

    CdXkbConfig lock_config = {0};
    set_config(&lock_config, "evdev", "pc105", "us", NULL, "");
    CdXkbKeyboard locks = {.fd = -1};
    CHECK(cd_xkb_keyboard_init(&locks, &lock_config, error, sizeof(error)), error);
    if (locks.state) {
        lock_key(&locks, KEY_CAPSLOCK);
        CHECK(cd_xkb_keyboard_led_mask(&locks) == CD_XKB_LED_CAPS_LOCK,
              "without grp_led, Caps Lock activates only the Caps LED");
        lock_key(&locks, KEY_CAPSLOCK);
        CHECK(cd_xkb_keyboard_led_mask(&locks) == 0,
              "second Caps Lock deactivates the ordinary Caps LED");
    }
    cd_xkb_keyboard_finish(&locks);
    cd_xkb_config_clear(&lock_config);
    cd_xkb_keyboard_finish(&keyboard);
    cd_xkb_config_clear(&bad);
    cd_xkb_config_clear(&next);
    cd_xkb_config_clear(&config);
}

static void clear_default_env(void)
{
    unsetenv("XKB_DEFAULT_RULES");
    unsetenv("XKB_DEFAULT_MODEL");
    unsetenv("XKB_DEFAULT_LAYOUT");
    unsetenv("XKB_DEFAULT_VARIANT");
    unsetenv("XKB_DEFAULT_OPTIONS");
}

int main(void)
{
    test_led_mask();
    clear_default_env();
    setenv("XKB_DEFAULT_LAYOUT", "us,ru", 1);
    setenv("XKB_DEFAULT_OPTIONS", "grp:alt_shift_toggle", 1);

    /* A missing [keyboard] section preserves all five xkbcommon defaults,
     * including the rules/model variables that the old compositor ignored. */
    CdXkbConfig empty = {0};
    CdXkbKeyboard keyboard = {.fd = -1};
    char error[512] = "";
    CHECK(cd_xkb_keyboard_init(&keyboard, &empty, error, sizeof(error)), error);
    if (!keyboard.keymap) return 1;
    CHECK(xkb_keymap_num_layouts(keyboard.keymap) == 2,
          "XKB_DEFAULT_LAYOUT supplies multiple layouts");
    CHECK(xkb_keymap_num_layouts(keyboard.keymap) > 1, "group test needs two layouts");
    CHECK(toggle_group(&keyboard) == 1, "grp:alt_shift_toggle selects second layout");
    CHECK(xkb_state_serialize_layout(keyboard.state, XKB_STATE_LAYOUT_EFFECTIVE) == 1,
          "toggle remains effective after chord release");
    CHECK(toggle_group(&keyboard) == 0, "second grp chord returns to first layout");

    int seals = fcntl(keyboard.fd, F_GET_SEALS);
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    CHECK(seals >= 0 && (seals & required) == required,
          "keymap fd is sealed against writes and resize");
    errno = 0;
    CHECK(pwrite(keyboard.fd, "X", 1, 0) < 0 && errno == EPERM,
          "keymap fd rejects writes");
    char *text = mmap(NULL, keyboard.size, PROT_READ, MAP_PRIVATE, keyboard.fd, 0);
    CHECK(text != MAP_FAILED, "keymap fd is readable by clients");
    if (text != MAP_FAILED) {
        CHECK(text[keyboard.size - 1] == '\0' && strstr(text, "xkb_keymap") != NULL,
              "keymap is a NUL-terminated XKB V1 map");
        munmap(text, keyboard.size);
    }

    int old_fd = keyboard.fd;
    struct xkb_keymap *old_map = keyboard.keymap;
    struct xkb_state *old_state = keyboard.state;
    bool changed = true;
    CHECK(cd_xkb_keyboard_reconfigure(&keyboard, &empty, NULL, &changed,
                                      error, sizeof(error)), "identical config is successful");
    CHECK(!changed && keyboard.fd == old_fd && keyboard.keymap == old_map &&
          keyboard.state == old_state, "identical config does not rebroadcast");

    CdXkbConfig next = {0};
    set_config(&next, NULL, NULL, "de,us", NULL, "grp:alt_shift_toggle");
    bool pressed[256] = {0};
    pressed[KEY_LEFTSHIFT] = true;
    pressed[KEY_W] = true;
    changed = false;
    CHECK(cd_xkb_keyboard_reconfigure(&keyboard, &next, pressed, &changed,
                                      error, sizeof(error)), error);
    CHECK(changed && keyboard.fd != old_fd && keyboard.keymap != old_map,
          "valid live config replaces the map and fd");
    CHECK(xkb_state_mod_name_is_active(keyboard.state, XKB_MOD_NAME_SHIFT,
                                       XKB_STATE_MODS_DEPRESSED) > 0,
          "held Shift is replayed into replacement state");
    CHECK(xkb_state_key_get_one_sym(keyboard.state, KEY_W + 8) == XKB_KEY_W,
          "held ordinary key remains translatable after replacement");
    xkb_state_update_key(keyboard.state, KEY_LEFTSHIFT + 8, XKB_KEY_UP);
    xkb_state_update_key(keyboard.state, KEY_W + 8, XKB_KEY_UP);
    CHECK(!(xkb_state_serialize_mods(keyboard.state, XKB_STATE_MODS_DEPRESSED) &
            (1u << xkb_keymap_mod_get_index(keyboard.keymap, XKB_MOD_NAME_SHIFT))),
          "post-reload release clears replayed Shift");

    /* Numeric keycode order would replay Space (57) before Meta (125), which
     * is not the physical win_space chord. Modifier-first replay must retain
     * its group edge when a reload happens with both keys held. */
    CdXkbConfig chord_initial = {0}, chord_next = {0};
    set_config(&chord_initial, NULL, NULL, "us,ru", NULL, "");
    set_config(&chord_next, NULL, NULL, "de,us", NULL, "grp:win_space_toggle");
    CdXkbKeyboard chord = {.fd = -1};
    error[0] = '\0';
    CHECK(cd_xkb_keyboard_init(&chord, &chord_initial, error, sizeof(error)), error);
    bool chord_pressed[256] = {0};
    chord_pressed[KEY_LEFTMETA] = true;
    chord_pressed[KEY_SPACE] = true;
    changed = false;
    CHECK(cd_xkb_keyboard_reconfigure(&chord, &chord_next, chord_pressed, &changed,
                                      error, sizeof(error)), error);
    CHECK(changed && xkb_state_serialize_layout(
              chord.state, XKB_STATE_LAYOUT_EFFECTIVE) == 1,
          "held Meta+Space survives replacement in chord order");

    old_fd = keyboard.fd;
    old_map = keyboard.keymap;
    old_state = keyboard.state;
    CdXkbConfig bad = {0};
    set_config(&bad, NULL, NULL, "chromedrive_no_such_layout", NULL,
               "grp:alt_shift_toggle");
    changed = true;
    error[0] = '\0';
    CHECK(!cd_xkb_keyboard_reconfigure(&keyboard, &bad, pressed, &changed,
                                       error, sizeof(error)),
          "unknown layout must fail compilation");
    CHECK(!changed && keyboard.fd == old_fd && keyboard.keymap == old_map &&
          keyboard.state == old_state, "invalid reload retains every active object");
    CHECK(error[0] != '\0', "invalid reload has a useful diagnostic");
    CHECK(cd_xkb_config_equal(&keyboard.active_config, &next),
          "invalid reload does not become the active configuration");
    CHECK(fcntl(old_fd, F_GETFD) >= 0, "old keymap fd remains open after rejection");

    CdXkbConfig no_options = {0};
    set_config(&no_options, NULL, NULL, "us,ru", NULL, "");
    CHECK(!cd_xkb_config_equal(&empty, &no_options),
          "absent and explicitly empty options are distinct");
    CdXkbKeyboard without_options = {.fd = -1};
    error[0] = '\0';
    CHECK(cd_xkb_keyboard_init(&without_options, &no_options, error, sizeof(error)), error);
    if (without_options.state) {
        CHECK(toggle_group(&without_options) == 0,
              "explicit empty options overrides inherited XKB_DEFAULT_OPTIONS");
    }

    /* Empty non-options strings are passed through too: xkbcommon treats them
     * like defaults, including XKB_DEFAULT_VARIANT. This differs from empty
     * positions within a non-empty variants list and from options="". */
    setenv("XKB_DEFAULT_VARIANT", "intl,", 1);
    CdXkbConfig blank_non_options = {0};
    set_config(&blank_non_options, "", "", "", "", NULL);
    CdXkbKeyboard blank_defaults = {.fd = -1};
    error[0] = '\0';
    CHECK(cd_xkb_keyboard_init(&blank_defaults, &blank_non_options,
                               error, sizeof(error)), error);
    if (blank_defaults.keymap) {
        const char *first = xkb_keymap_layout_get_name(blank_defaults.keymap, 0);
        CHECK(first && strstr(first, "intl"),
              "empty variant retains XKB_DEFAULT_VARIANT");
        CHECK(xkb_keymap_num_layouts(blank_defaults.keymap) == 2 &&
              toggle_group(&blank_defaults) == 1,
              "empty non-options fields retain layout/options defaults");
    }
    unsetenv("XKB_DEFAULT_VARIANT");

    CdConfig *store = cd_config_load("/chromedrive-test-no-such-file");
    CHECK(store != NULL, "empty config store");
    if (store) {
        cd_config_set_string(store, "keyboard", "rules", "evdev");
        cd_config_set_string(store, "keyboard", "model", "pc105");
        cd_config_set_string(store, "keyboard", "layouts", "us,ru");
        cd_config_set_string(store, "keyboard", "variants", ",");
        cd_config_set_string(store, "keyboard", "options", "");
        CdXkbConfig parsed = {0};
        CHECK(cd_xkb_config_from_store(&parsed, store, error, sizeof(error)), error);
        CHECK(parsed.rules && parsed.model && parsed.layouts && parsed.variants && parsed.options,
              "present keys remain present even when empty");
        CHECK(parsed.options && parsed.options[0] == '\0' && parsed.variants &&
              strcmp(parsed.variants, ",") == 0 && strcmp(parsed.layouts, "us,ru") == 0,
              "RMLVO strings round-trip without a fixed preset");
        struct xkb_rule_names names;
        cd_xkb_config_names(&parsed, &names);
        CHECK(names.rules == parsed.rules && names.model == parsed.model &&
              names.layout == parsed.layouts && names.variant == parsed.variants &&
              names.options == parsed.options, "all five RMLVO fields reach xkbcommon");
        cd_xkb_config_clear(&parsed);
        cd_config_free(store);
    }

    cd_xkb_keyboard_finish(&chord);
    cd_xkb_keyboard_finish(&blank_defaults);
    cd_xkb_keyboard_finish(&without_options);
    cd_xkb_keyboard_finish(&keyboard);
    cd_xkb_config_clear(&chord_initial);
    cd_xkb_config_clear(&chord_next);
    cd_xkb_config_clear(&blank_non_options);
    cd_xkb_config_clear(&no_options);
    cd_xkb_config_clear(&bad);
    cd_xkb_config_clear(&next);
    if (failures) return 1;
    puts("PASS: XKB RMLVO/env defaults, multiple groups, grp switching/LEDs, lock LEDs, held-key replay, sealed fd and invalid-reload rollback");
    return 0;
}
