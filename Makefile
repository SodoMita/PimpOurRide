CC ?= cc
PKGS = wayland-server sdl2 egl gl cairo xkbcommon libvncserver
CPPFLAGS += -Iinclude -I.generated $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm
PROTO_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
GEN = .generated
BIN = bin
SOURCES = src/main.c src/wayland.c src/scene.c src/ui.c src/math3d.c src/dmabuf.c src/backend_sdl.c src/backend_headless.c
PROTOCOLS = $(GEN)/xdg-shell-protocol.c $(GEN)/xdg-decoration-protocol.c $(GEN)/linux-dmabuf-protocol.c
HEADERS = $(GEN)/xdg-shell-server.h $(GEN)/xdg-decoration-server.h $(GEN)/linux-dmabuf-server.h
.PHONY: all clean test sanitize
all: $(BIN)/chromedrive $(BIN)/chromedrive-demo
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
$(BIN)/chromedrive: $(SOURCES) include/chrome.h include/math3d.h $(HEADERS) $(PROTOCOLS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(PROTOCOLS) -o $@ $(LDLIBS)
$(BIN)/chromedrive-demo: clients/demo.c $(GEN)/xdg-shell-client.h $(GEN)/xdg-shell-protocol.c | $(BIN)
	$(CC) -I$(GEN) $(CFLAGS) $(shell pkg-config --cflags wayland-client cairo xkbcommon) clients/demo.c $(GEN)/xdg-shell-protocol.c -o $@ $(shell pkg-config --libs wayland-client cairo xkbcommon) -lm
$(BIN)/test-math: tests/test_math.c src/math3d.c include/math3d.h | $(BIN)
	$(CC) -Iinclude $(CFLAGS) tests/test_math.c src/math3d.c -o $@ -lm
$(BIN)/protocol-probe: tests/protocol_probe.c $(GEN)/xdg-shell-client.h $(GEN)/xdg-shell-protocol.c $(GEN)/linux-dmabuf-protocol.c $(GEN)/linux-dmabuf-client.h | $(BIN)
	$(CC) -I$(GEN) $(CFLAGS) $(shell pkg-config --cflags wayland-client) tests/protocol_probe.c $(GEN)/xdg-shell-protocol.c $(GEN)/linux-dmabuf-protocol.c -o $@ $(shell pkg-config --libs wayland-client)
test: $(BIN)/test-math
	./$(BIN)/test-math
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -fsanitize=address,undefined -fno-omit-frame-pointer'
clean:
	rm -rf $(BIN) $(GEN)

