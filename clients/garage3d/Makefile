# garage3d - native Wayland + OpenGL ES 3.1 client.
#
# Deliberately mirrors the repo-root Makefile's style (pkg-config modules,
# wayland-scanner outputs in .generated/, binary in bin/, one-shot compile)
# so the whole repository builds with one toolchain and no new build system.
#
#   make            -> bin/garage3d
#   make test       -> headless math/physics/scene-graph unit tests (no GL)
#   make smoke      -> end-to-end render under weston/headless + pixel checks
#   make sanitize   -> ASan+UBSan build
#   make install    -> binary + shaders into $(PREFIX)

CC ?= cc
PREFIX ?= /usr/local
PKGS = wayland-client wayland-egl wayland-cursor egl glesv2 xkbcommon libwebp

CPPFLAGS += -Isrc -I.generated $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

PROTO_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
GEN = .generated
BIN = bin

SOURCES = src/main.c src/wayland_client.c src/renderer.c src/scene.c \
          src/animation.c src/mesh.c src/gmath.c
HEADERS = src/wayland_client.h src/renderer.h src/scene.h src/animation.h \
          src/mesh.h src/gmath.h src/input.h
SHADERS = shaders/lit.vert shaders/lit.frag shaders/neon.vert shaders/neon.frag \
          shaders/city.vert shaders/city.frag shaders/mist.vert shaders/mist.frag \
          shaders/particle.vert shaders/particle.frag
PURE_SOURCES = src/gmath.c src/mesh.c src/scene.c src/animation.c

.PHONY: all clean test smoke sanitize install
all: $(BIN)/garage3d

$(GEN) $(BIN):
	mkdir -p $@

# xdg-shell client protocol code is generated, never checked in: the XML is
# the single source of truth and every distro ships it in wayland-protocols.
$(GEN)/xdg-shell-client.h: | $(GEN)
	wayland-scanner client-header $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@
$(GEN)/xdg-shell-protocol.c: | $(GEN)
	wayland-scanner private-code $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml $@

$(BIN)/garage3d: $(SOURCES) $(HEADERS) $(SHADERS) $(GEN)/xdg-shell-client.h $(GEN)/xdg-shell-protocol.c | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(GEN)/xdg-shell-protocol.c -o $@ $(LDLIBS)

# The test binary links only the pure (GL/Wayland-free) modules + libm.
$(BIN)/test-garage3d: tests/test_gmath_physics.c $(PURE_SOURCES) $(HEADERS) | $(BIN)
	$(CC) -Isrc $(CFLAGS) tests/test_gmath_physics.c $(PURE_SOURCES) -o $@ -lm

test: $(BIN)/test-garage3d
	./$(BIN)/test-garage3d

smoke: $(BIN)/garage3d
	sh tools/smoke-test.sh

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-omit-frame-pointer'

install: $(BIN)/garage3d
	install -Dm755 $(BIN)/garage3d $(DESTDIR)$(PREFIX)/bin/garage3d
	install -dm755 $(DESTDIR)$(PREFIX)/share/garage3d/shaders
	install -Dm644 $(SHADERS) -t $(DESTDIR)$(PREFIX)/share/garage3d/shaders/

clean:
	rm -rf $(BIN) $(GEN)
