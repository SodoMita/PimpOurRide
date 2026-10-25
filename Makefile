CC ?= cc
PKGS = wayland-server sdl2 egl gl cairo xkbcommon libvncserver libwebp
CPPFLAGS += -Iinclude -I.generated $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm
PROTO_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
GEN = .generated
BIN = bin
SOURCES = src/main.c src/wayland.c src/scene.c src/city_scene.c src/vehicle.c src/city.c src/vehicle_scene.c src/vehicle_seat.c src/keymap.c src/wheel_input.c src/ui.c src/math3d.c src/telemetry.c src/dmabuf.c src/cdconfig.c src/i18n.c src/backend_sdl.c src/backend_headless.c src/backend_egl.c src/backend_vnc.c src/backend_fbdev.c src/backend_evdev.c src/evdev_util.c src/fbdev_pixfmt.c
PROTOCOLS = $(GEN)/xdg-shell-protocol.c $(GEN)/xdg-decoration-protocol.c $(GEN)/linux-dmabuf-protocol.c
HEADERS = $(GEN)/xdg-shell-server.h $(GEN)/xdg-decoration-server.h $(GEN)/linux-dmabuf-server.h
.PHONY: all clean test sanitize garage3d garage3d-test garage3d-clean
all: $(BIN)/chromedrive $(BIN)/chromedrive-demo $(BIN)/chromedrive-config
$(GEN) $(BIN):
	mkdir -p $@
$(GEN)/xdg-shell-server.h: | $(GEN)
	wayland-scanner server-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@
$(GEN)/xdg-shell-client.h: | $(GEN)
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@
$(GEN)/xdg-shell-protocol.c: | $(GEN)
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@
$(GEN)/xdg-decoration-server.h: | $(GEN)
	wayland-scanner server-header $(PROTO_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@
$(GEN)/xdg-decoration-protocol.c: | $(GEN)
	wayland-scanner private-code $(PROTO_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml $@
$(GEN)/linux-dmabuf-server.h: | $(GEN)
	wayland-scanner server-header $(PROTO_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml $@
$(GEN)/linux-dmabuf-protocol.c: | $(GEN)
	wayland-scanner private-code $(PROTO_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml $@
$(GEN)/linux-dmabuf-client.h: | $(GEN)
	wayland-scanner client-header $(PROTO_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml $@
$(BIN)/chromedrive: $(SOURCES) include/chrome.h include/math3d.h include/vehicle.h include/city.h include/vehicle_seat.h include/wheel_input.h include/telemetry.h include/cdconfig.h include/i18n.h include/fbdev_pixfmt.h $(HEADERS) $(PROTOCOLS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(PROTOCOLS) -o $@ $(LDLIBS)
$(BIN)/chromedrive-demo: clients/demo.c src/wheel_input.c include/telemetry.h include/wheel_input.h $(GEN)/xdg-shell-client.h $(GEN)/xdg-shell-protocol.c | $(BIN)
	$(CC) -Iinclude -I$(GEN) $(CFLAGS) $(shell pkg-config --cflags wayland-client cairo xkbcommon) clients/demo.c src/wheel_input.c $(GEN)/xdg-shell-protocol.c -o $@ $(shell pkg-config --libs wayland-client cairo xkbcommon) -lm
# Settings GUI (launch-menu editor + all config): wl_shm/cairo client like
# the demo, linking the same INI store the compositor reads. No new deps
# beyond what the compositor already needs (libwebp included), so it is
# part of `all`.
$(BIN)/chromedrive-config: clients/config.c src/cdconfig.c src/i18n.c include/cdconfig.h include/i18n.h $(GEN)/xdg-shell-client.h $(GEN)/xdg-shell-protocol.c | $(BIN)
	$(CC) -Iinclude -I$(GEN) $(CFLAGS) $(shell pkg-config --cflags wayland-client cairo xkbcommon libwebp) clients/config.c src/cdconfig.c src/i18n.c $(GEN)/xdg-shell-protocol.c -o $@ $(shell pkg-config --libs wayland-client cairo xkbcommon libwebp) -lm
$(BIN)/test-math: tests/test_math.c src/math3d.c include/math3d.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_math.c src/math3d.c -o $@ -lm
$(BIN)/protocol-probe: tests/protocol_probe.c $(GEN)/xdg-shell-client.h $(GEN)/xdg-shell-protocol.c $(GEN)/linux-dmabuf-protocol.c $(GEN)/linux-dmabuf-client.h | $(BIN)
	$(CC) -I$(GEN) $(CFLAGS) $(shell pkg-config --cflags wayland-client) tests/protocol_probe.c $(GEN)/xdg-shell-protocol.c $(GEN)/linux-dmabuf-protocol.c -o $@ $(shell pkg-config --libs wayland-client)
# garage3d (clients/garage3d) is a standalone native Wayland/GLES client with
# its own Makefile and extra deps (wayland-cursor, GLES3). It is deliberately
# NOT part of `all`, so building the compositor never requires them.
garage3d:
	$(MAKE) -C clients/garage3d
garage3d-test:
	$(MAKE) -C clients/garage3d test
garage3d-clean:
	$(MAKE) -C clients/garage3d clean
$(BIN)/test-config: tests/test_config.c src/cdconfig.c include/cdconfig.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_config.c src/cdconfig.c -o $@ -lm
$(BIN)/test-vehicle: tests/test_vehicle.c src/vehicle.c src/city.c src/math3d.c include/vehicle.h include/city.h include/math3d.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_vehicle.c src/vehicle.c src/city.c src/math3d.c -o $@ -lm
$(BIN)/test-telemetry: tests/test_telemetry.c src/telemetry.c src/vehicle.c src/city.c src/math3d.c include/chrome.h include/telemetry.h | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_telemetry.c src/telemetry.c src/vehicle.c src/city.c src/math3d.c -o $@ -lm
$(BIN)/test-keymap: tests/test_keymap.c src/keymap.c include/keymap.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_keymap.c src/keymap.c -o $@
$(BIN)/test-wheel-input: tests/test_wheel_input.c src/wheel_input.c include/wheel_input.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_wheel_input.c src/wheel_input.c -o $@ -lm
$(BIN)/test-fbdev: tests/test_fbdev.c src/fbdev_pixfmt.c src/evdev_util.c include/fbdev_pixfmt.h include/evdev_util.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_fbdev.c src/fbdev_pixfmt.c src/evdev_util.c -o $@
$(BIN)/test-i18n: tests/test_i18n.c src/i18n.c include/i18n.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_i18n.c src/i18n.c -o $@
test: $(BIN)/test-math $(BIN)/test-config $(BIN)/test-vehicle $(BIN)/test-telemetry $(BIN)/test-keymap $(BIN)/test-wheel-input $(BIN)/test-fbdev $(BIN)/test-i18n $(BIN)/chromedrive-config
	./$(BIN)/test-math
	./$(BIN)/test-config
	./$(BIN)/test-vehicle
	./$(BIN)/test-telemetry
	./$(BIN)/test-keymap
	./$(BIN)/test-wheel-input
	./$(BIN)/test-fbdev
	./$(BIN)/test-i18n
	./$(BIN)/chromedrive-config --selftest
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -fsanitize=address,undefined -fno-omit-frame-pointer'
clean:
	rm -rf $(BIN) $(GEN)

