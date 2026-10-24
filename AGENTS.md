# AGENTS.md

Guardrails for any human or AI agent committing to this repository. ChromeDrive
is a small, dependency-conscious **native C Wayland compositor**. Most ways to
"improve" it from habit (web-project habits especially) actually degrade it.
Read this before you commit, not after.

## 1. Screenshots / images — never commit raw captures

- **Never `git add` a raw PNG screenshot or frame capture.** `F12` / test
  scripts write full uncompressed RGBA PNGs (commonly 500 KB–1 MB+ each) to
  `screenshots/`. That directory is `.gitignore`d on purpose
  (`/screenshots/*.png`, `/screenshots/*.jpg`) — keep it that way.
- If a screenshot genuinely needs to live in the repo (e.g. a README hero
  image), **convert it to lossy WebP first** and put it under `docs/`:
  ```
  python3 -c "from PIL import Image; Image.open('shot.png').convert('RGB').save('docs/hero.webp','WEBP',quality=82,method=6)"
  ```
  `docs/hero.webp` is the one deliberate exception — it replaced an 821 KB PNG
  at ~98 KB. Match that pattern; don't add a second multi-hundred-KB image
  next to it without compressing it the same way.
- Never commit test/debug artifacts (`cursor-test.png`, `headless-test.png`,
  `*-zoom.png`, etc.). Those are throwaway verification output, not project
  content — delete them locally or let `.gitignore` do it.
- Don't delete `screenshots/.gitkeep`. `src/main.c`'s F12 handler
  (`scene_save_screenshot`) does a plain `fopen`/cairo write with no
  `mkdir -p` — if the directory doesn't exist on a fresh clone, screenshotting
  silently fails.
- `assets/garage.png` is **not** a screenshot — it's a runtime texture loaded
  by `scene.c` via `ui_load_png`. It must stay PNG (the loader only reads
  PNG) and must stay tracked. Don't "clean it up" by converting or removing
  it.

## 2. JavaScript / TypeScript — do not add a frontend stack

This is a **C project**. The only JavaScript in the repo is the ~15-line
inline `<script type="module">` in `tools/viewer/index.html`, which opens a
WebSocket to the compositor's own embedded VNC server and wires a few hotkey
buttons. That's it, by design.

- **Do not add `package.json`, `node_modules/`, a bundler (webpack/vite/esbuild),
  or a TypeScript build step.** There is nothing here that needs compiling —
  the viewer is one static HTML file, intentionally.
- **Do not rewrite `tools/viewer/index.html` in React/Vue/Svelte/TS/whatever.**
  It is a thin noVNC client shell, not an app. More framework = more attack
  surface and build fragility for zero functional gain here.
- **Do not vendor or commit `tools/viewer/core/` or `tools/viewer/vendor/`.**
  Those are symlinks that `tools/prepare-viewer.sh` creates, pointing at the
  system `novnc` package (`/usr/share/novnc`). They're `.gitignore`d
  deliberately — committing real copies of noVNC bloats the repo with
  third-party code that's trivially `apt install novnc` instead.
- If the viewer genuinely needs a new feature, extend the existing inline
  script in place. Don't introduce a second JS entry point, a `src/` JS tree,
  or any `import` that resolves outside of noVNC's `rfb.js`.
- Never add JS/TS anywhere else in the tree (`src/`, `tests/`, `clients/`) —
  those are compositor internals, protocol tests, and demo Wayland clients,
  all plain C. A `.js`/`.ts` file appearing under any of those paths is a
  sign something went wrong.

## 3. Build artifacts & generated files

- Never commit `bin/`, `.generated/`, `*.o`, or any other `make`-produced
  file. The Makefile regenerates all of it (`wayland-scanner` output, object
  files, binaries) from a clean tree every time.
- Don't commit anything with no code reference to it. (`assets/garage.jpg`
  was removed for exactly this reason — unused, undocumented, pure bloat.)
  Before adding a binary asset, `grep` the source tree to confirm it's
  actually loaded.

## 4. Secrets & credentials

- **Never commit tokens, API keys, or passwords** — not in source, not in
  scripts, not in README examples, not even "temporarily." If a credential is
  ever needed for a one-off push/deploy, use it directly in the shell/remote
  URL and never write it to a tracked file or `.git/config` that gets pushed.
- If a secret is ever accidentally staged, don't just delete it in a new
  commit — the old blob still exists in history. Stop and rewrite history
  (`git filter-repo`/`BFG`) before pushing, or rotate the credential.

## 5. Project architecture — don't quietly change it

These were explicit, deliberate product decisions. Don't "simplify" them away
in an unrelated change:

- **Headless-by-default, no X server anywhere.** `backend_headless.c`
  (surfaceless EGL + an embedded VNC server) is the default path; `--windowed`
  (SDL2/GLX) exists only as a local dev/debug fallback. Don't make
  `--windowed` the default, and don't reintroduce `Xvfb`/`x11vnc` as a
  required dependency — the compositor *is* its own VNC server now.
- **Real 3D raycast picking for dashboard buttons**, not 2D/HTML hit-testing.
  Buttons are picked against actual cockpit geometry from the first-person
  camera. Don't flatten this into a 2D overlay-click shortcut.
- **One spatial virtual monitor per app**, positioned in the 3D scene — not
  OS-level virtual desktops, not physical multi-monitor output. Don't change
  "launch app" to open a regular overlapping window instead of mounting it on
  its own in-scene screen.
- First-person camera from the driver's seat is required, not optional framing.

## 6. Before every commit

1. `make clean && make all` — must build **with zero warnings**.
2. `make test` — `protocol-probe` and `test-math` must both pass.
3. `git status --short` — confirm nothing from `bin/`, `.generated/`,
   `screenshots/*.png`, `node_modules/`, or any secret is staged.
4. Keep commits scoped: don't mix an unrelated dependency bump, a vendored
   library, or a generated-file dump into a feature commit.
