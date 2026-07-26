/* pp_assets.h -- reassemble Unity split asset files at boot. See pp_assets.c. */
#ifndef PP_ASSETS_H
#define PP_ASSETS_H

/* Join assets/bin/Data/<name>.split0..N back into <name>, verified against the
 * Unity serialized-file header. Idempotent: a no-op once the files are joined.
 * Call once at boot, BEFORE the engine libraries are loaded. */
void pp_assets_prepare(void);

#endif /* PP_ASSETS_H */
