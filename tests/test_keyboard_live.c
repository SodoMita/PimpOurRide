#define _GNU_SOURCE
/* Include the production config glue as well as the production Wayland server.
 * The entry point is renamed and discarded by --gc-sections; the test calls
 * apply_keyboard_config() for the real INI -> RMLVO startup/reload paths. */
#define main chromedrive_program_main
#include "../src/main.c"
#undef main

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
        failures++; \
    } \
} while (0)

/* A real libwayland client connected to the production server over socketpair.
 * The nonblocking pump is the usual prepare_read/read_events contract; no
 * proxy of protocol behavior and no mocked wl_keyboard events are involved. */
typedef struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct wl_client *server_side;
    struct xkb_context *context;
    struct xkb_keymap *map;
    struct xkb_state *state;
    int syncs, keymaps, enters, leaves, keys, key_events, modifiers;
    int mods_at_keymap;
    uint32_t depressed, latched, locked, group;
    uint32_t last_key, last_state;
    xkb_keysym_t last_sym;
    bool seals_ok;
} Client;

static void sync_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    Client *client = data;
    client->syncs++;
    wl_callback_destroy(callback);
}
static const struct wl_callback_listener sync_listener = {.done = sync_done};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    Client *client = data;
    (void)keyboard;
    CHECK(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 100,
          "server sends a nonempty XKB V1 keymap");
    int seals = fcntl(fd, F_GET_SEALS);
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (seals >= 0 && (seals & required) == required) client->seals_ok = true;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !size) {
        close(fd);
        return;
    }
    const char *bytes = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (bytes == MAP_FAILED) return;
    size_t length = size;
    if (bytes[length - 1] == '\0') length--;
    struct xkb_keymap *map = xkb_keymap_new_from_buffer(
        client->context, bytes, length, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap((void *)bytes, size);
    if (!map) return;
    struct xkb_state *state = xkb_state_new(map);
    if (!state) {
        xkb_keymap_unref(map);
        return;
    }
    if (client->state) xkb_state_unref(client->state);
    if (client->map) xkb_keymap_unref(client->map);
    client->map = map;
    client->state = state;
    client->keymaps++;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    Client *client = data;
    (void)keyboard; (void)serial; (void)surface;
    client->enters++;
    client->keys += (int)(keys ? keys->size / sizeof(uint32_t) : 0);
}
static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    Client *client = data;
    (void)keyboard; (void)serial; (void)surface;
    client->leaves++;
}
static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    Client *client = data;
    (void)keyboard; (void)serial; (void)time;
    client->last_key = key;
    client->last_state = state;
    client->key_events++;
    client->last_sym = client->state
        ? xkb_state_key_get_one_sym(client->state, key + 8) : XKB_KEY_NoSymbol;
}
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    Client *client = data;
    (void)keyboard; (void)serial;
    client->depressed = depressed;
    client->latched = latched;
    client->locked = locked;
    client->group = group;
    client->modifiers++;
    client->mods_at_keymap = client->keymaps;
    if (client->state)
        xkb_state_update_mask(client->state, depressed, latched, locked,
                              0, 0, group);
}
static void keyboard_repeat(void *data, struct wl_keyboard *keyboard,
                            int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    Client *client = data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !client->keyboard) {
        client->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    Client *client = data;
    if (!strcmp(interface, "wl_compositor"))
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_seat")) {
        client->seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
        wl_seat_add_listener(client->seat, &seat_listener, client);
    }
}
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_remove,
};

static bool pump_once(Server *server, Client *client)
{
    if (wl_display_flush(client->display) < 0 && errno != EAGAIN) return false;
    server_dispatch(server);
    while (wl_display_prepare_read(client->display) != 0) {
        if (wl_display_dispatch_pending(client->display) < 0) return false;
    }
    if (wl_display_flush(client->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(client->display);
        return false;
    }
    struct pollfd fd = {.fd = wl_display_get_fd(client->display), .events = POLLIN};
    int ready = poll(&fd, 1, 1);
    if (ready < 0 && errno != EINTR) {
        wl_display_cancel_read(client->display);
        return false;
    }
    if (ready > 0 && (fd.revents & POLLIN)) {
        if (wl_display_read_events(client->display) < 0) return false;
    } else {
        wl_display_cancel_read(client->display);
    }
    if (wl_display_dispatch_pending(client->display) < 0) return false;
    server_dispatch(server);
    return true;
}

static bool pump_until(Server *server, Client *client, int *value, int target)
{
    for (int i = 0; i < 5000 && *value < target; i++) {
        if (!pump_once(server, client)) return false;
    }
    return *value >= target;
}

static bool client_sync(Server *server, Client *client)
{
    int target = client->syncs + 1;
    struct wl_callback *callback = wl_display_sync(client->display);
    if (!callback) return false;
    wl_callback_add_listener(callback, &sync_listener, client);
    return pump_until(server, client, &client->syncs, target);
}

static bool client_connect(Server *server, Client *client)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) return false;
    client->server_side = wl_client_create(server->display, fds[0]);
    if (!client->server_side) {
        close(fds[0]); close(fds[1]);
        return false;
    }
    client->display = wl_display_connect_to_fd(fds[1]);
    if (!client->display) return false;
    client->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!client->context) return false;
    client->registry = wl_display_get_registry(client->display);
    wl_registry_add_listener(client->registry, &registry_listener, client);
    if (!client_sync(server, client)) return false;
    if (!client->seat || !client->compositor) return false;
    return pump_until(server, client, &client->keymaps, 1);
}

static void client_finish(Client *client)
{
    if (client->surface) wl_surface_destroy(client->surface);
    if (client->keyboard) wl_keyboard_release(client->keyboard);
    if (client->seat) wl_seat_release(client->seat);
    if (client->compositor) wl_compositor_destroy(client->compositor);
    if (client->registry) wl_registry_destroy(client->registry);
    if (client->display) {
        wl_display_flush(client->display);
        wl_display_disconnect(client->display);
    }
    if (client->state) xkb_state_unref(client->state);
    if (client->map) xkb_keymap_unref(client->map);
    if (client->context) xkb_context_unref(client->context);
    memset(client, 0, sizeof(*client));
}

static void set_config(CdXkbConfig *out, const char *layouts, const char *options)
{
    CdXkbConfig literal = {.layouts = (char *)layouts, .options = (char *)options};
    CHECK(cd_xkb_config_copy(out, &literal), "copy live-test configuration");
}

static CdConfig *store_with_layouts(const char *layouts, const char *options)
{
    CdConfig *store = cd_config_load("/chromedrive-live-no-such-file");
    if (!store) return NULL;
    cd_config_set_string(store, "keyboard", "rules", "evdev");
    cd_config_set_string(store, "keyboard", "model", "pc105");
    cd_config_set_string(store, "keyboard", "layouts", layouts);
    cd_config_set_string(store, "keyboard", "variants", ",");
    cd_config_set_string(store, "keyboard", "options", options);
    return store;
}

static bool client_shift_down(Client *client)
{
    return client->state && xkb_state_mod_name_is_active(
        client->state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_DEPRESSED) > 0;
}

int main(void)
{
    unsetenv("XKB_DEFAULT_RULES");
    unsetenv("XKB_DEFAULT_MODEL");
    unsetenv("XKB_DEFAULT_LAYOUT");
    unsetenv("XKB_DEFAULT_VARIANT");
    unsetenv("XKB_DEFAULT_OPTIONS");
    char runtime[] = "/tmp/chromedrive-xkb-live-XXXXXX";
    CHECK(mkdtemp(runtime) != NULL, "temporary runtime directory");
    if (failures) return 1;
    setenv("XDG_RUNTIME_DIR", runtime, 1);

    Server server = {.xkb = {.fd = -1}};
    CdConfig *startup = store_with_layouts("us,ru", "grp:alt_shift_toggle");
    CHECK(startup != NULL, "startup INI store");
    if (startup) {
        apply_keyboard_config(&server, startup, true);
        cd_config_free(startup);
    }
    CHECK(server_init(&server, "chromedrive-xkb-live-test"), "production server_init");
    if (!server.display) return 1;
    Client client = {0};
    CHECK(client_connect(&server, &client), "real Wayland client connection and first keymap");
    if (!client.display) {
        server_finish(&server);
        return 1;
    }
    CHECK(client.map && xkb_keymap_num_layouts(client.map) == 2,
          "client compiled initial two-layout keymap");
    CHECK(client.seals_ok, "client received a sealed read-only keymap fd");

    /* Group state belongs to the seat, not to keyboard focus. Toggle twice
     * with no focused surface, then start the focused checks from group 0. */
    server_key(&server, KEY_LEFTALT, true, 1);
    server_key(&server, KEY_LEFTSHIFT, true, 2);
    server_key(&server, KEY_LEFTSHIFT, false, 3);
    server_key(&server, KEY_LEFTALT, false, 4);
    CHECK(xkb_state_serialize_layout(server.xkb.state, XKB_STATE_LAYOUT_EFFECTIVE) == 1,
          "grp chord updates XKB state without keyboard focus");
    CHECK(client_sync(&server, &client) && client.modifiers == 0,
          "no-focus group switch does not invent focused modifiers");
    server_key(&server, KEY_LEFTALT, true, 5);
    server_key(&server, KEY_LEFTSHIFT, true, 6);
    server_key(&server, KEY_LEFTSHIFT, false, 7);
    server_key(&server, KEY_LEFTALT, false, 8);
    CHECK(xkb_state_serialize_layout(server.xkb.state, XKB_STATE_LAYOUT_EFFECTIVE) == 0,
          "second no-focus chord restores the initial group");

    client.surface = wl_compositor_create_surface(client.compositor);
    CHECK(client_sync(&server, &client), "surface creation roundtrip");
    Surface *surface = NULL, *candidate;
    wl_list_for_each(candidate, &server.surfaces, link) {
        if (wl_resource_get_client(candidate->resource) == client.server_side) {
            surface = candidate;
            break;
        }
    }
    CHECK(surface != NULL, "server has the client's real wl_surface");
    if (surface) server_focus(&server, surface);
    CHECK(pump_until(&server, &client, &client.enters, 1), "focused client enter");

    /* Standard XKB group switching is processed even before any keysym is
     * interpreted by a client; effective layout then reaches wl_keyboard. */
    server_key(&server, KEY_LEFTALT, true, 10);
    server_key(&server, KEY_LEFTSHIFT, true, 11);
    CHECK(client_sync(&server, &client), "group-switch events");
    CHECK(client.group == 1, "Alt+Shift broadcasts effective second layout");
    server_key(&server, KEY_LEFTSHIFT, false, 12);
    server_key(&server, KEY_LEFTALT, false, 13);
    server_key(&server, KEY_A, true, 14);
    CHECK(client_sync(&server, &client), "Russian key event");
    CHECK(client.last_key == KEY_A && client.last_state == WL_KEYBOARD_KEY_STATE_PRESSED &&
          client.last_sym == XKB_KEY_Cyrillic_ef,
          "physical KEY_A is Cyrillic ef in the second group");
    server_key(&server, KEY_A, false, 15);

    /* The compositor and vehicle never translate these controls through XKB.
     * Exercise the production handler while the seat is still in group 1. */
    CHECK(xkb_state_serialize_layout(server.xkb.state, XKB_STATE_LAYOUT_EFFECTIVE) == 1,
          "physical-shortcut checks run in the non-US group");
    server.steering_mode = VEHICLE_STEER_DRIVE;
    CHECK(app_global_key(&server, KEY_W, true) && server.drive_forward,
          "physical W drives in the Russian group");
    CHECK(app_global_key(&server, KEY_W, false) && !server.drive_forward,
          "physical W release drives in the Russian group");
    VehicleCameraMode old_camera = server.camera_mode;
    CHECK(app_global_key(&server, KEY_F8, true) &&
          server.camera_mode == (VehicleCameraMode)((old_camera + 1) % 3),
          "physical F8 remains a compositor shortcut in the Russian group");
    CHECK(app_global_key(&server, KEY_F8, false), "F8 release remains consumed");
    CHECK(!app_global_key(&server, KEY_F5, true), "F5 remains unbound");
    server_key(&server, KEY_LEFTMETA, true, 16);
    CHECK(app_global_key(&server, KEY_O, true) && server.door_shortcut_down,
          "physical Meta+O remains a compositor chord in the Russian group");
    CHECK(app_global_key(&server, KEY_O, false) && !server.door_shortcut_down,
          "physical Meta+O release remains consumed");
    server_key(&server, KEY_LEFTMETA, false, 17);
    server.steering_mode = VEHICLE_STEER_CLIENT;

    /* Replace the map while Shift is physically down. The client must see the
     * keymap before the matching modifier event and then a normal release. */
    server_key(&server, KEY_LEFTSHIFT, true, 20);
    CHECK(client_sync(&server, &client), "Shift down before live replacement");
    CHECK(client_shift_down(&client), "client has depressed Shift before replacement");
    CdXkbConfig next = {0};
    set_config(&next, "de,us", "grp:win_space_toggle");
    bool changed = false;
    char error[512] = "";
    CHECK(server_keyboard_reconfigure(&server, &next, &changed, error, sizeof(error)), error);
    CHECK(changed, "valid live replacement reports a change");
    CHECK(pump_until(&server, &client, &client.keymaps, 2),
          "already connected focused client receives replacement keymap");
    CHECK(client_sync(&server, &client), "replacement modifiers");
    CHECK(client.mods_at_keymap == 2, "new modifier state follows the new keymap");
    CHECK(client_shift_down(&client), "held Shift survives keymap replacement without sticking");
    server_key(&server, KEY_LEFTSHIFT, false, 21);
    CHECK(client_sync(&server, &client), "post-replacement Shift release");
    CHECK(!client_shift_down(&client) && client.last_key == KEY_LEFTSHIFT &&
          client.last_state == WL_KEYBOARD_KEY_STATE_RELEASED,
          "held Shift releases normally after replacement");

    /* Space is the DRIVE brake and is deliberately consumed before Wayland
     * delivery. It must still reach XKB: Meta+Space switches group, emits only
     * modifiers, and replays in chord order if the map changes while held. */
    server.steering_mode = VEHICLE_STEER_DRIVE;
    if (!app_global_key(&server, KEY_LEFTMETA, true))
        server_key(&server, KEY_LEFTMETA, true, 22);
    CHECK(client_sync(&server, &client), "Meta down before win_space");
    int local_key_events = client.key_events;
    client.last_key = UINT32_MAX;
    client.last_state = UINT32_MAX;
    CHECK(app_global_key(&server, KEY_SPACE, true) && server.drive_brake &&
          server.xkb_pressed[KEY_SPACE] && !server.keys[KEY_SPACE],
          "DRIVE consumes Space as brake without adding a client-held key");
    CHECK(xkb_state_serialize_layout(server.xkb.state, XKB_STATE_LAYOUT_EFFECTIVE) == 1,
          "consumed Meta+Space switches server XKB group");
    CHECK(client_sync(&server, &client) && client.group == 1 &&
          client.key_events == local_key_events && client.last_key == UINT32_MAX &&
          client.last_state == UINT32_MAX,
          "consumed grp chord sends modifiers but no Space key event");
    set_config(&next, "de,fr", "grp:win_space_toggle");
    changed = false;
    CHECK(server_keyboard_reconfigure(&server, &next, &changed, error, sizeof(error)), error);
    CHECK(changed, "replacement during consumed grp chord applies");
    CHECK(pump_until(&server, &client, &client.keymaps, 3) &&
          client_sync(&server, &client),
          "client receives replacement during consumed grp chord");
    CHECK(client.mods_at_keymap == 3 && client.group == 1 &&
          client.key_events == local_key_events && client.last_key == UINT32_MAX &&
          client.last_state == UINT32_MAX,
          "held consumed Meta+Space replays its group without a key event");
    CHECK(app_global_key(&server, KEY_SPACE, false) && !server.drive_brake &&
          !server.xkb_pressed[KEY_SPACE],
          "consumed Space release clears brake and physical XKB bitmap");
    CHECK(app_global_key(&server, KEY_SPACE, true) &&
          app_global_key(&server, KEY_SPACE, false),
          "second consumed win_space chord remains local");
    CHECK(client_sync(&server, &client) && client.group == 0 &&
          client.key_events == local_key_events && client.last_key == UINT32_MAX &&
          client.last_state == UINT32_MAX,
          "second win_space chord returns to group zero without key leakage");
    if (!app_global_key(&server, KEY_LEFTMETA, false))
        server_key(&server, KEY_LEFTMETA, false, 23);
    server.steering_mode = VEHICLE_STEER_CLIENT;

    struct xkb_keymap *active_map = server.xkb.keymap;
    int active_fd = server.xkb.fd;
    int client_maps = client.keymaps;
    CdConfig *bad = store_with_layouts("chromedrive_no_such_layout", "grp:alt_shift_toggle");
    CHECK(bad != NULL, "invalid INI store");
    if (bad) {
        apply_keyboard_config(&server, bad, false);
        cd_config_free(bad);
    }
    CHECK(server.xkb.keymap == active_map && server.xkb.fd == active_fd,
          "invalid INI hot reload leaves server map and fd untouched");
    CHECK(client_sync(&server, &client) && client.keymaps == client_maps,
          "invalid live layout emits no client keymap");

    /* No-focus reload still broadcasts, but modifiers wait for the next enter. */
    server_focus(&server, NULL);
    CHECK(client_sync(&server, &client), "keyboard leave");
    CdConfig *reload = store_with_layouts("fr,ru", "grp:alt_shift_toggle");
    CHECK(reload != NULL, "no-focus INI reload store");
    if (reload) {
        apply_keyboard_config(&server, reload, false);
        cd_config_free(reload);
    }
    CHECK(pump_until(&server, &client, &client.keymaps, 4),
          "unfocused existing client still receives live keymap from INI reload");
    CHECK(client_sync(&server, &client), "no-focus reload flush");
    CHECK(client.mods_at_keymap == 3, "no focused modifier event is invented during reload");
    if (surface) server_focus(&server, surface);
    CHECK(client_sync(&server, &client), "focus after no-focus reload");
    CHECK(client.mods_at_keymap == 4 && client.enters >= 2,
          "next enter supplies modifiers for the latest keymap");

    /* A newly bound keyboard after all replacements gets the current map, not
     * an older generation. This also exercises the ordinary bind path. */
    Client late = {0};
    CHECK(client_connect(&server, &late), "late client connection");
    if (late.display) {
        CHECK(late.map && xkb_keymap_num_layouts(late.map) == 2,
              "late client gets current two-layout map");
        CHECK(late.seals_ok, "late client keymap is sealed too");
    }

    /* The optional changed out-parameter must never gate protocol delivery.
     * Both already connected clients receive this generation. */
    set_config(&next, "de,fr", "grp:alt_shift_toggle");
    CHECK(server_keyboard_reconfigure(&server, &next, NULL, error, sizeof(error)), error);
    CHECK(pump_until(&server, &client, &client.keymaps, 5),
          "NULL changed still broadcasts to the focused client");
    if (late.display) {
        CHECK(client_sync(&server, &late) && late.keymaps == 2,
              "one live replacement reaches every connected client");
    }

    client_finish(&late);
    client_finish(&client);
    server_finish(&server);

    /* A bad explicit startup map has no previous user map to retain, so the
     * production startup path falls back to environment/system defaults. */
    Server fallback = {.xkb = {.fd = -1}};
    CdConfig *bad_startup = store_with_layouts("chromedrive_no_such_startup_layout", "");
    CHECK(bad_startup != NULL, "bad startup INI store");
    if (bad_startup) {
        apply_keyboard_config(&fallback, bad_startup, true);
        cd_config_free(bad_startup);
    }
    CHECK(server_init(&fallback, "chromedrive-xkb-fallback-test"),
          "invalid explicit startup layout falls back to a working default");
    if (fallback.display) {
        CHECK(fallback.xkb.keymap && cd_xkb_config_is_absent(&fallback.xkb.active_config),
              "startup fallback activates the default RMLVO configuration");
        server_finish(&fallback);
    }
    cd_xkb_config_clear(&next);
    char socket_path[4096];
    snprintf(socket_path, sizeof(socket_path), "%s/chromedrive-xkb-live-test", runtime);
    unlink(socket_path);
    snprintf(socket_path, sizeof(socket_path), "%s/chromedrive-xkb-fallback-test", runtime);
    unlink(socket_path);
    rmdir(runtime);
    if (failures) return 1;
    puts("PASS: real Wayland live keymaps, grp with/without focus, consumed grp chords, physical shortcuts, held-key release, rollback, late bind and startup fallback");
    return 0;
}
