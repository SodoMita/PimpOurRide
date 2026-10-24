/*
 * main.c - garage3d: a native Wayland + OpenGL ES 3.1 client that rebuilds
 * ChromeDrive's flat garage.webp backdrop as a real-time 3D scene.
 *
 * The render loop, and how it meshes with the Wayland event loop:
 *
 *     while (running) {
 *         wlclient_dispatch(can_draw ? 0 : -1);   // events; block iff idle
 *         handle edge requests (fullscreen/pause/quit)
 *         if (!wlclient_can_draw()) continue;     // vsync gate (frame cb)
 *         wlclient_apply_pending_resize();        // deferred EGL resize
 *         dt = clamp(now - previous, 0, 0.1);     // frame-rate independence
 *         camera_update(dt);                      // consumes InputState
 *         anim_update(dt);                        // fixed-step physics inside
 *         scene_update_world();                   // resolve the graph
 *         renderer_draw();                        // GL only, no Wayland
 *         input_clear_frame();                    // deltas are per-frame
 *         wlclient_present();                     // frame req + eglSwapBuffers
 *     }
 *
 * There is deliberately no sleep(), no timer, and no "render as fast as
 * possible": when a frame callback is outstanding, can_draw is false and
 * dispatch blocks in poll() on the Wayland fd at 0% CPU until the
 * compositor says the previous frame was presented. That is the correct
 * Wayland shape - the compositor's refresh drives us, targeting 60 fps on
 * a 60 Hz output with exactly one frame in flight.
 */
#define _GNU_SOURCE
#include "animation.h"
#include "renderer.h"
#include "scene.h"
#include "wayland_client.h"
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/encode.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef struct {
    AppConfig app;
    float fov;
    const char *shader_dir;
    const char *screenshot_path;
    uint64_t screenshot_frame; /* 1-based rendered-frame index */
    uint64_t max_frames;       /* 0 = unlimited (smoke tests use this) */
    double exit_after;         /* seconds, 0 = never */
    bool start_paused;
} Options;

static void usage(const char *argv0) {
    printf(
        "garage3d - native Wayland/OpenGL ES 3D garage (the garage.webp scene, in 3D)\n"
        "\nUsage: %s [options]\n"
        "  --width N --height N   Initial logical window size (default 1280x720)\n"
        "  --scale N              Force wl buffer scale 1..4 (default: follow output)\n"
        "  --fov DEGREES          Camera field of view 30..90 (default 62)\n"
        "  --fullscreen           Request fullscreen at startup (F toggles later)\n"
        "  --shader-dir DIR       Directory containing the .vert/.frag files\n"
        "  --paused               Start with animation/physics paused (Space toggles)\n"
        "  --frames N             Exit after N rendered frames (smoke tests)\n"
        "  --exit-after SECONDS   Exit after SECONDS (smoke tests)\n"
        "  --screenshot PATH      Write a WebP of frame --screenshot-frame\n"
        "  --screenshot-frame N   Frame to capture (default 20)\n"
        "  --help                 This help\n"
        "\nControls:\n"
        "  mouse left-drag / 1-finger drag    orbit        wheel / pinch   zoom\n"
        "  right/middle-drag / 2-finger drag  pan          double-tap / R  reset view\n"
        "  WASD + QE (or arrows + PgUp/PgDn)  move         +/-             field of view\n"
        "  Space pause physics   F fullscreen   Esc or Ctrl+Q quit\n"
        "\nConnects to $WAYLAND_DISPLAY: any compositor (weston, sway, GNOME,\n"
        "KDE, phosh) or ChromeDrive itself (WAYLAND_DISPLAY=<its socket>).\n",
        argv0);
}

/* WebP screenshot, mirroring the compositor's scene_save_screenshot():
 * PNG is banned project-wide (see AGENTS.md), so frame captures are encoded
 * with libwebp (WebPEncodeRGBA, quality 90). Automated checks decode the
 * result and assert the frame is non-black and contains both neon colours. */
static bool save_screenshot_webp(const char *path, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    /* Read back as RGBA: it is the one format/type pair GLES guarantees for
     * glReadPixels; GL_RGB readback is implementation-optional and silently
     * fails (leaving the buffer untouched) on several Mesa drivers. */
    unsigned char *pixels = malloc((size_t)w * h * 4);
    if (!pixels) return false;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "garage3d: screenshot glReadPixels failed (GL error 0x%x)\n", err);
        free(pixels);
        return false;
    }
    /* Flip vertically (GL origin is bottom-left) and force opaque alpha:
     * destination-alpha in an EGL window buffer is not meaningful content. */
    unsigned char *flipped = malloc((size_t)w * h * 4);
    if (!flipped) { free(pixels); return false; }
    for (int y = 0; y < h; y++) {
        unsigned char *dst = &flipped[(size_t)y * w * 4];
        memcpy(dst, &pixels[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        for (int x = 0; x < w; x++) dst[x * 4 + 3] = 255;
    }
    free(pixels);
    uint8_t *encoded = NULL;
    size_t encoded_size = WebPEncodeRGBA(flipped, w, h, w * 4, 90.0f, &encoded);
    free(flipped);
    bool ok = false;
    if (encoded_size > 0 && encoded) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(encoded, 1, encoded_size, f) == encoded_size;
            ok = fclose(f) == 0 && ok;
        }
    }
    if (encoded) WebPFree(encoded);
    if (ok) fprintf(stderr, "garage3d: screenshot written to %s (%dx%d)\n", path, w, h);
    else fprintf(stderr, "garage3d: screenshot to %s failed\n", path);
    return ok;
}

int main(int argc, char **argv) {
    Options opt = { 0 };
    opt.app.title = "Garage3D — ChromeDrive garage in 3D";
    opt.app.app_id = "chromedrive.garage3d";
    opt.app.width = 1280;
    opt.app.height = 720;
    opt.fov = 62.0f;
    opt.screenshot_frame = 20;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) opt.app.width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) opt.app.height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) opt.app.scale_override = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fov") == 0 && i + 1 < argc) opt.fov = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--fullscreen") == 0) opt.app.start_fullscreen = true;
        else if (strcmp(argv[i], "--shader-dir") == 0 && i + 1 < argc) opt.shader_dir = argv[++i];
        else if (strcmp(argv[i], "--paused") == 0) opt.start_paused = true;
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) opt.max_frames = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--exit-after") == 0 && i + 1 < argc) opt.exit_after = atof(argv[++i]);
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) opt.screenshot_path = argv[++i];
        else if (strcmp(argv[i], "--screenshot-frame") == 0 && i + 1 < argc) opt.screenshot_frame = strtoull(argv[++i], NULL, 10);
        else {
            fprintf(stderr, "garage3d: unknown or incomplete option '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (opt.app.width < 64 || opt.app.height < 64 || opt.app.width > 16384 || opt.app.height > 16384) {
        fprintf(stderr, "garage3d: invalid --width/--height\n");
        return 2;
    }
    if (opt.app.scale_override < 0 || opt.app.scale_override > 4) {
        fprintf(stderr, "garage3d: --scale must be 0 (auto) or 1..4\n");
        return 2;
    }

    /* sigaction WITHOUT SA_RESTART: Ctrl-C must interrupt the poll() inside
     * wlclient_dispatch, not restart it after the handler runs. */
    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    WlClient client;
    if (!wlclient_init(&client, &opt.app)) {
        wlclient_shutdown(&client);
        return 1;
    }

    Scene scene;
    scene_build(&scene, opt.fov);
    AnimationState anim;
    anim_init(&anim, &scene);
    anim.paused = opt.start_paused;

    Renderer renderer = { 0 };
    if (!renderer_init(&renderer, opt.shader_dir)) {
        fprintf(stderr, "garage3d: renderer initialisation failed\n");
        wlclient_shutdown(&client);
        return 1;
    }
    renderer_log_info();
    fprintf(stderr, "garage3d: %d scene nodes, logical %dx%d (scale %d, buffer %dx%d)\n",
            scene.node_count, client.logical_w, client.logical_h, client.scale,
            wlclient_phys_width(&client), wlclient_phys_height(&client));

    int exit_code = 0;
    uint64_t frames = 0;
    double started = wlclient_now();
    double previous = started;
    double fps_window_start = started;
    uint64_t fps_window_frames = 0;

    while (client.running && !g_stop && !client.input.quit) {
        /* Block only when there is nothing to draw. When a frame *can* be
         * drawn we still dispatch first (timeout 0) so resize/configure and
         * input arriving between frames are never delayed by a render. */
        int timeout = wlclient_can_draw(&client) ? 0 : -1;
        if (wlclient_dispatch(&client, timeout) < 0) {
            fprintf(stderr, "garage3d: fatal Wayland display error, exiting\n");
            exit_code = 1;
            break;
        }

        /* Edge-triggered window/system requests are handled even while no
         * frame can be drawn (e.g. occluded), then cleared here rather than
         * by input_clear_frame so they cannot fire twice. */
        if (client.input.toggle_fullscreen) {
            wlclient_toggle_fullscreen(&client);
            client.input.toggle_fullscreen = false;
        }
        if (client.input.toggle_pause) {
            anim.paused = !anim.paused;
            client.input.toggle_pause = false;
            fprintf(stderr, "garage3d: animation %s\n", anim.paused ? "paused" : "resumed");
        }
        if (client.input.quit || !client.running || g_stop) break;
        if (!wlclient_can_draw(&client)) continue; /* the vsync gate */

        wlclient_apply_pending_resize(&client);

        double now = wlclient_now();
        /* Delta-time, clamped: after an occlusion/suspend the raw gap could
         * be minutes; 0.1 s is the most physics is ever asked to absorb in
         * one frame (the fixed-step accumulator then runs <= 12 ticks).
         * Camera idle uses the same monotonic epoch as input.last_activity. */
        float dt = (float)(now - previous);
        previous = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;

        camera_update(&scene, &client.input, dt, now);
        anim_update(&anim, &scene, dt);
        scene_update_world(&scene);

        /* Shader time = simulated time, so Space truly freezes the world
         * (mist, twinkle and ripples included), not just the physics. */
        renderer_draw(&renderer, &scene, &anim,
                      wlclient_phys_width(&client), wlclient_phys_height(&client),
                      (float)anim.time);

        if (opt.screenshot_path && frames + 1 == opt.screenshot_frame)
            save_screenshot_webp(opt.screenshot_path,
                                wlclient_phys_width(&client), wlclient_phys_height(&client));

        input_clear_frame(&client.input);

        if (!wlclient_present(&client)) {
            exit_code = 1;
            break;
        }
        frames++;
        fps_window_frames++;

        double fps_now = wlclient_now();
        if (fps_now - fps_window_start >= 5.0) {
            fprintf(stderr, "garage3d: %.1f fps (%llu frames, buffer %dx%d)\n",
                    fps_window_frames / (fps_now - fps_window_start),
                    (unsigned long long)frames,
                    wlclient_phys_width(&client), wlclient_phys_height(&client));
            fps_window_start = fps_now;
            fps_window_frames = 0;
        }
        if (opt.max_frames > 0 && frames >= opt.max_frames) break;
        if (opt.exit_after > 0 && wlclient_now() - started >= opt.exit_after) break;
    }

    fprintf(stderr, "garage3d: exiting after %llu frames (%.2f s)\n",
            (unsigned long long)frames, wlclient_now() - started);

    /* GL resources first, while the context is current; then EGL/Wayland
     * teardown in reverse dependency order (see wlclient_shutdown). */
    renderer_shutdown(&renderer);
    wlclient_shutdown(&client);
    return exit_code;
}
