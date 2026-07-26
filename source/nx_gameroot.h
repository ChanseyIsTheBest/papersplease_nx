/* nx_gameroot.h -- runtime game-folder resolution.
 *
 * Lets the .nro live in ANY folder under sdmc:/switch (papers_please,
 * PapersPlease, papersplease, whatever) instead of one hardcoded name.
 * See nx_gameroot.c for the resolution order.
 *
 * game_root_init() must be the first thing main() calls -- before any
 * debugPrintf, because the log file lives under the root we are resolving.
 */
#ifndef NX_GAMEROOT_H
#define NX_GAMEROOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve once. Safe to call again (subsequent calls are no-ops). */
void game_root_init(int argc, char **argv);

/* "sdmc:/switch/<folder>" -- device-qualified, never a trailing slash. */
const char *game_root(void);

/* "/switch/<folder>" -- Unix-rooted. Hand THIS to the engine/managed side:
 * Path.Combine treats "sdmc:/x" as drive-relative and mangles it. */
const char *game_root_unix(void);

/* "sdmc:/switch/<folder>/debug.log" */
const char *game_log_path(void);

/* snprintf "<root>/<rel>" into buf. A leading '/' on rel is tolerated.
 * Returns what snprintf returns. */
int game_path(char *buf, size_t sz, const char *rel);

#ifdef __cplusplus
}
#endif

#endif /* NX_GAMEROOT_H */
