/*
 * cdconfig.h - tiny INI-style configuration store (compositor + config GUI).
 *
 * One deliberately small format serves the whole project:
 *
 *     # comment                (';' also starts a comment)
 *     [section]
 *     key = value
 *
 * Values are stored as strings; typed getters interpret them (ints, doubles,
 * bools as true/false/1/0/yes/no/on/off, colours as #RRGGBB). Two roles use
 * this module:
 *
 *   - readers (the compositor in src/main.c): load once at startup, then
 *     poll cd_config_mtime() and re-load when the file changes - that is what
 *     makes `chromedrive-config` edits apply live.
 *   - the one writer (clients/config.c, the GUI): loads the store, edits
 *     values, saves. Unknown sections/keys are preserved verbatim through a
 *     load/save round-trip, so hand-added keys the GUI does not know about
 *     (e.g. compositor `socket`) survive. Comments and blank lines do NOT
 *     survive a GUI save - the store is a data model, not a text model.
 *
 * Saves are atomic (write to "<path>.tmp", fsync best-effort, rename), so a
 * reader polling mtime can never observe a half-written file.
 *
 * The Garage3D client carries its own typed loader for the same format (it
 * must stay buildable/installable standalone); it never writes config, so it
 * needs no store. This header is standalone on purpose: no chrome.h, so the
 * GUI client can link src/cdconfig.c without dragging in the compositor.
 */
#ifndef CDCONFIG_H
#define CDCONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct CdConfig CdConfig;

/* Loads `path`. A missing/unreadable file is not an error: it yields an
 * empty store (every getter then returns its default). NULL only on OOM. */
CdConfig *cd_config_load(const char *path);
void cd_config_free(CdConfig *cfg);

/* Serialises the store to `path` atomically, creating parent directories
 * (mode 0700 components under the config home) as needed. */
bool cd_config_save(const CdConfig *cfg, const char *path);

/* Resolves "$XDG_CONFIG_HOME/<app>/<file>" (or "~/.config/<app>/<file>").
 * Returns false (and leaves an empty string) if neither variable is set. */
bool cd_config_path(char *buf, size_t size, const char *app, const char *file);

/* Modification time of `path`, 0 if it does not exist. Poll this to detect
 * GUI edits; comparing against the value from your last load is the whole
 * hot-reload mechanism - no inotify, no threads. */
time_t cd_config_mtime(const char *path);

/* Typed getters. Missing key or unparseable value -> the default. */
const char *cd_config_get_string(const CdConfig *cfg, const char *section,
                                 const char *key, const char *fallback);
long cd_config_get_int(const CdConfig *cfg, const char *section,
                       const char *key, long fallback);
double cd_config_get_double(const CdConfig *cfg, const char *section,
                            const char *key, double fallback);
bool cd_config_get_bool(const CdConfig *cfg, const char *section,
                        const char *key, bool fallback);
/* #RRGGBB (a leading '#' is optional) -> rgb[3] in 0..1. */
bool cd_config_get_color(const CdConfig *cfg, const char *section,
                         const char *key, float rgb[3]);

/* Setters (the GUI's side). Setting an existing key updates it in place so
 * file order is stable across edits. */
void cd_config_set_string(CdConfig *cfg, const char *section, const char *key,
                          const char *value);
void cd_config_set_int(CdConfig *cfg, const char *section, const char *key, long value);
void cd_config_set_double(CdConfig *cfg, const char *section, const char *key, double value);
void cd_config_set_bool(CdConfig *cfg, const char *section, const char *key, bool value);
void cd_config_set_color(CdConfig *cfg, const char *section, const char *key,
                         const float rgb[3]);

/* True if the key exists at all (distinguishes "absent" from "empty"). */
bool cd_config_has(const CdConfig *cfg, const char *section, const char *key);

#endif /* CDCONFIG_H */
