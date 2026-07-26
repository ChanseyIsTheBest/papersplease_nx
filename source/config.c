/* config.c -- configuration parser (Papers, Please Switch port)
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "config.h"
#include "util.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(portrait);

Config config;
static int config_needs_rewrite = 0;

// actual screen size that is in use right now
int screen_width = 720;   /* fbstub45 PORTRAIT (stable) */
int screen_height = 1280;

static inline void parse_var(const char *name, const char *value) {
  // retired options -> drop them and rewrite the file without them
  if (!strcmp(name, "screen_dpi") ||
      !strcmp(name, "touchscreen") || !strcmp(name, "controller_cursor") ||
      !strcmp(name, "show_fps") || !strcmp(name, "widescreen") ||
      !strcmp(name, "screen_width") || !strcmp(name, "screen_height") ||
      !strcmp(name, "language")) {
    config_needs_rewrite = 1;
    return;
  }

  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = atof(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { strlcpy(config.var, value, sizeof(config.var)); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  memset(&config, 0, sizeof(Config));
  config_needs_rewrite = 0;
  /* PORTRAIT by default: it is the only layout the phone build of this game
   * ships. Landscape classes exist in the Haxe code (Aspect_LANDSCAPE,
   * BoothLayout_OUTER_LEFT) because that source is shared with the iPad and
   * desktop builds -- the phone build never selects them, so landscape here
   * gives a wrong presentation, not a smaller one.
   *
   * The window itself CANNOT be portrait: once rendering starts the swapchain
   * is fixed at the panel size and nwindowSetDimensions fails (rc=0xf59 on
   * hardware). So the rotation is done in GL instead -- the engine renders into
   * a portrait FBO and pp_tate.c blits it rotated onto the landscape window.
   *
   *   1 = rotate 90 CW (default)   2 = rotate 90 CCW   0 = no rotation
   *
   * These were inverted in the first build of the GL compositor -- 1 gave CCW.
   * Fixed in pp_tate.c; the labels now mean what they say. Touch follows the
   * same value, so the two cannot disagree.
   *
   * If the image comes out upside down or mirrored, try 2. If the compositor
   * path misbehaves entirely, 0 renders straight to the window (landscape
   * layout, which this game does not really support -- diagnostic only). */
  config.portrait = 1;

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;

  // parse lines of the forms
  // <spaces> # <whatever> \n
  // <spaces> NAME <spaces> VALUE <spaces> \n
  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      // trim name
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      // if tmp points to the end of the string, there's no value to parse
      if (*tmp != 0) {
        *tmp = 0;
        // value is next; trim value
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; isspace((int)*tmp); --tmp) *tmp = 0;
        // got key value pair
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  /* Portrait is the only presentation this game supports on phone, so the only
   * question is which way round. Anything that is not 2 becomes 1 -- including
   * the old "0" (landscape), which existed briefly as a diagnostic and would
   * now just give a wrong layout. A file carrying it is rewritten. */
  if (config.portrait != 1 && config.portrait != 2) {
    config.portrait = 1;
    config_needs_rewrite = 1;
  }

  return config_needs_rewrite ? 1 : 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f,
    "# papersplease_nx configuration -- lines are \"name value\"; # starts a comment\n"
    "#\n"
    "# portrait   -- which way the portrait image is rotated onto the panel:\n"
    "#               1 = 90 clockwise         (right Joy-Con up)  [default]\n"
    "#               2 = 90 counter-clockwise (left Joy-Con up)\n"
    "#               Touch follows this automatically. There is no landscape\n"
    "#               option: this game is portrait-only on phone.\n"
    "#\n"
    "\n");

  #define CONFIG_VAR_INT(var) fprintf(f, "%s %d\n", #var, config.var)
  #define CONFIG_VAR_FLOAT(var) fprintf(f, "%s %g\n", #var, config.var)
  #define CONFIG_VAR_STR(var) if (config.var[0]) fprintf(f, "%s %s\n", #var, config.var)
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_FLOAT
  #undef CONFIG_VAR_STR

  fclose(f);

  return 0;
}
