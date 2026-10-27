# AGENTS.md

Guardrails for any human or AI agent committing to this repository. ChromeDrive
is a small, dependency-conscious **native C Wayland compositor**. Most ways to
"improve" it from habit (web-project habits especially) actually degrade it.
Read this before you commit, not after.

## 1. Images — PNG is banned, no exceptions. Use WebP.

**There must be zero `.png` files anywhere in this repository or in anything
the compositor loads/writes at runtime — ever.** This isn't scoped to
screenshots or to "things committed to git"; it's a project-wide rule
covering generated test captures, committed docs assets, and runtime texture
files alike. This was enforced in code, not just convention:

- `src/ui.c`'s `ui_load_webp()` decodes textures with **libwebp**
  (`WebPDecodeBGRA`), not Cairo's PNG reader. There is no PNG-decoding code
  path left in the renderer.
- `src/scene.c`'s `scene_save_screenshot()` encodes captures with
  **libwebp** (`WebPEncodeBGRA`, quality 90), not
  `cairo_surface_write_to_png`. `F12` and `--screenshot PATH` both write
  `.webp` files.
- `assets/garage.webp` is the runtime cockpit background texture (was
  `assets/garage.png`, converted and removed). `docs/hero.webp` is the README
  hero image. Both are WebP. **Do not add a `.png` loader, a `.png` asset, or
  reintroduce `cairo_image_surface_create_from_png` /
  `cairo_surface_write_to_png` anywhere.** If you touch image I/O, it goes
  through libwebp (`-lwebp` / `pkg-config libwebp`, already wired into the
  Makefile's `PKGS`).
- Never `git add` a raw/full-size capture either, even as WebP. `F12` / test
  scripts write frame captures to `screenshots/`, which is `.gitignore`d on
  purpose (`/screenshots/*.webp`) — keep it that way. Don't delete
  `screenshots/.gitkeep`: `scene_save_screenshot` does a plain `fopen` with no
  `mkdir -p`, so the directory must exist on a fresh clone or captures will
  silently fail.
- If an image genuinely needs to live in the repo (e.g. a new README figure),
  it must already be lossy WebP before it's staged:
  ```
  python3 -c "from PIL import Image; Image.open('shot.png').convert('RGB').save('docs/name.webp','WEBP',quality=82,method=6)"
  ```
  (Use that script to *produce* the WebP locally — the intermediate `.png`
  it reads from must never itself be committed or left lying around.)
- Never commit test/debug artifacts regardless of format
  (`cursor-test.webp`, `headless-test.webp`, `*-zoom.webp`, etc.). Those are
  throwaway verification output, not project content.

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

## 2b. User-facing text — go through `tr()`

ChromeDrive speaks one thing at runtime and ships it in several languages.
The rule is the same shape as the PNG ban: there is exactly one right way.

- **Every string the user can see** — HUD, Driver's Manual, Launch Control
  labels, `app_notice()` messages, the settings GUI — is passed through
  `tr()` from `include/i18n.h`. A bare `"Some Label"` handed to `text()`,
  `mono()`, `txt()` or `app_notice()` is a bug.
- **The English text is the key** (gettext model). No key constants, no
  generated header, no extraction tool. Catalog: `assets/locale/<tag>.lang`.
- **Translate at draw time, never in a table.** `Def.sec/key` are config keys
  and, when `enum_is_string`, the stored value is the English label itself —
  translating those in place would corrupt the user's config file and break
  hot reload. Labels, group headings and enum *display* values are translated
  where they are drawn.
- **Editable text fields keep raw bytes.** The settings GUI shows the
  user's stored value in inputs (only read-only summaries are translated), so
  a translated label can never be written back into the config.
- Keys are matched exactly: same case, same spacing (the HUD aligns columns
  with doubled spaces), and **no `=` in a key** — it separates key from value
  in the catalog format.
- **Never truncate translated text with a printf precision.** `%.62s` / `%.*s`
  count bytes and split multi-byte characters (already reachable in Russian,
  guaranteed in CJK). Use `i18n_utf8_truncate()` from `include/i18n.h`.
- **`snprintf()` into a fixed buffer is the same bug with a worse failure
  mode.** A half-written code point makes cairo latch
  `CAIRO_STATUS_INVALID_STRING`, after which *every* draw call on that context
  is silently dropped - a 96-byte note buffer blanked the entire app picker.
  Use `cd_utf8_printf()` from `include/textfit.h`; it also sanitises text that
  an earlier byte-wise truncation already broke.
- **Bound every string that goes into a fixed slot.** Both cairo clients draw
  into a fixed 1000x625 box with neighbouring columns, and translations are
  routinely 20-40% wider than the English. Use the shared
  `text_fit()`/`mono_fit()`/`txt_fit()` wrappers from `src/textfit_cairo.c`
  (policy in `src/textfit.c`): shrink to a legibility floor, then ellipsize -
  never clip, and never draw past the slot into the next column. Compute a
  follow-on element's position from the width the text was **drawn** at, not
  the width it measured at (that is how the RESTART badge ended up on top of a
  slider). `make check-text` renders every catalog and fails on any ellipsis;
  English must still fit at 100%, and is the reference layout.
- **Never translate a format string instead of its argument.** `tr("%s")` with
  the sentence passed as an argument translates nothing; write `"%s"` as a
  literal and wrap the argument in `tr()`. Same for ternaries inside a
  `snprintf`: translate each branch, not the outer `%s`.
- Window titles of **our own** clients (`clients/demo.c`,
  `clients/config.c`) are chrome and go through `tr()` at the point they are
  set. A third-party client's `xdg_toplevel` title belongs to that app — the
  compositor must never translate it.
- **Font coverage is a real constraint, not a detail.** The text path is
  cairo's toy API, which does *no* per-glyph fallback (measured: CJK ink in
  DejaVu is byte-identical to a bogus family name). See "Scripts and font
  coverage" in `docs/configuration.md` before adding a catalog for a new
  script.
- **A non-European catalog names its family** with `@font` / `@monofont` in the
  catalog header, and the family is applied at draw time (`i18n_font()`), so a
  language switch takes effect without a restart. Never hardcode a family name
  in a new drawing path - go through `cd_font()` / `ui_font_family()` or the
  new path will paint tofu in CJK.
- **A catalog that cannot draw its own text must be refused, not left rendered.**
  The probe in `src/fontprobe.c` compares ink against an unassigned code point
  and falls back to English with a loud log line. Standalone Cairo drawing
  programs call it after loading the catalog and *before* baking textures.
  The compositor is the deliberate exception because an in-process
  fontconfig/Cairo stall would also stop input and managed VT servicing: it
  may bake the selected language while `bin/chromedrive-fontprobe` is pending,
  but a verified missing-glyph verdict must immediately re-bake English when
  that catalog is still live, and stale verdicts must be ignored. Never move
  the probe back into the compositor process or wait for a hung helper. Its
  sample comes from the catalog's values (`i18n_sample_glyphs()`), so do not
  add a second hardcoded sample table.
- **Arabic, Hebrew, Indic and Thai are not shippable on this text path.**
  No shaping, no bidi: the result is disconnected letterforms in the wrong
  order. That needs Pango, not a catalog.
- Bake-time, not frame-time: textures with text in them are built once. If
  you add a new baked-with-text texture, add it to `ui_refresh_text()` in
  `src/ui.c` or it will keep the old language until restart.
- **Baked chrome text is fitted, never drawn bare.** In `src/ui.c` and
  `src/city_scene.c`, every translated label goes through `text_fit()`,
  `mono_fit()`, `text_fit_c()` or `oblique_fit()` (shrink to 0.85, then
  ellipsize, never clip) with a real budget: `lane(c, x, margin)` when the label
  ends its line, or an explicit slot width when a column follows. Text drawn
  with a bare `cairo_show_text()` at a fixed size is how "Раскладка" became
  "Движен…" in the hint bar. Sizes are chosen so English fits at 100% - a shrink
  in English means the slot is wrong, not the translation.
- **Never name a font family in a drawing path.** `i18n_font_family(mono)`
  (called by `ui_font_family()` and `cd_font()`) is the single source, so a
  catalog's `@font` is honoured everywhere; a hardcoded `"DejaVu Sans"` is how a
  translated CJK screen renders as tofu.
- **Adding an English label means adding it to every catalog in the same
  commit.** `make test` now fails on both sides of that: *drift* (a catalog key
  that no longer appears verbatim in `src/`/`clients/`, so a reworded label
  cannot silently orphan its translations) and *parity* (a catalog whose key
  set differs from `assets/locale/ru.lang`, the reference set). A language the
  menu offers must not be half English; if a string genuinely stays English,
  list it with the English text as its value.
- **Prove it by drawing, not by grepping.** `tr("...")` call sites cannot see a
  label that arrives from a table (`tr(d->label)` in the settings GUI), and
  that is exactly how a whole column stayed English in four catalogs at once.
  `CHROMEDRIVE_TEXT_AUDIT=1` makes `tr()` report every prose string it could
  not translate; `make check-text` runs it over every language, over the
  clients *and* the compositor, and fails on anything not declared in
  `assets/locale/untranslated.allow`.
- **Never add an unauthenticated network listener on the default path.** The
  embedded RFB server has no password, so whoever connects drives the cockpit;
  it binds loopback by default (`session.vnc_loopback`, `--vnc-listen-all` to
  opt out) and an all-interface bind announces itself at startup. The same rule
  applies to any future remote surface: opt-in, loopback, and loud.
- `make test` runs `tests/test_i18n.c`, which fails if a shipped catalog key
  no longer appears verbatim in the sources. Rewording an English string
  means updating `assets/locale/ru.lang` in the same commit. It also runs
  `tests/test_textfit.c` (the fitting policy against a synthetic metric, so
  the expected cut points are exact) and the overflow check in report mode.
- A missing catalog or key **must** fall back to English silently. Never
  print blanks or key names on screen; a partially translated install is a
  supported state.

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
- **Never run privilege/authentication helpers from the compositor runtime.**
  No `sudo`, `pkexec`, askpass, or prompting shell command on the main/input
  thread. Host device permissions are setup policy; direct syscalls may be
  best-effort, but denial must fail only the optional feature while Wayland
  input and managed VT release continue.
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
   `screenshots/*.webp`, any `.png` file at all, `node_modules/`, or any
   secret is staged.
4. Keep commits scoped: don't mix an unrelated dependency bump, a vendored
   library, or a generated-file dump into a feature commit.

## 7. Multi-agent coordination

- **Read the Mayfly coordination chat at both the start and the end of every turn.**
  Always fetch and read through the latest messages at the very beginning of a
  turn (before starting work, rebasing, or claiming scope, and before posting)
  and read the chat room again at the end of every turn before finishing.
- Use the user-supplied Mayfly room for cross-agent claims, integration status,
  and handoffs. The room URL is explicitly intended for repository agents;
  one-time GitHub PATs and other credentials still follow section 4.

[Mayfly coordination room](https://mayfly.chat/c/evwwU_QqGJycQEPbUwsjfg#KIH9_ejxDUXbgeff3I4Y4GS8uwmRta0jcohz-8FdRL4)
