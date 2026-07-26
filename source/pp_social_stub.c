/* pp_social_stub.c -- native P/Invoke stubs for Papers, Please's Google Play
 * Games integration.
 *
 * This file replaces the Zookeeper base's firebase_stub.c.  Papers, Please
 * ships no Firebase SDK; what it does ship (visible in the il2cpp metadata as
 * `Google.Play.Games.dll`, and in the game's own code as
 * `SocialWrapper.ApiGooglePlay` / `SocialWrapper.ApiSocialService`) is Google
 * Play Games Services: achievements, leaderboards and cloud sign-in.
 *
 * None of that exists on a Switch.  The important thing is not that it fails,
 * but that it fails FAST and SYNCHRONOUSLY.  The failure mode to avoid is the
 * one CloverPit hit with Play Billing: a sign-in call that neither succeeds nor
 * returns, leaving a managed callback pending forever and the game waiting on
 * it.  So every entry point here returns "not available" immediately rather
 * than returning an error code that the managed side might retry, and
 * definitely rather than blocking.
 *
 * How it is reached: libc_shim.c's dlsym_fake() consults
 * pp_native_stub_lookup() before giving up, so a managed [DllImport] that
 * resolves through dlopen/dlsym lands here instead of returning NULL (a NULL
 * would become a null-call crash inside the il2cpp interop thunk).
 *
 * The game degrades gracefully on its own: SocialWrapper.ApiSocialService is
 * written against an interface, and the Haxe-side code treats an unavailable
 * social service as "no achievements this session".  Saves are unaffected --
 * Papers, Please saves through HostUnity.UnityPlatformDisk
 * (getPersistentString / setPersistentString) straight to the filesystem, NOT
 * through Play Games cloud save.
 *
 * MIT, same as the rest of the tree.
 */

#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include "util.h"

/* ---- generic shapes the Play Games native glue expects ------------------- */

/* Most of the plugin's native surface is "start an async op and call back".
 * Returning 0 / NULL immediately is the "unavailable" answer; the managed
 * wrapper checks for it. */
static void     stub_void      (void)          { }
static int32_t  stub_false     (void)          { return 0; }
static int32_t  stub_status    (void)          { return 2; }   /* SignInStatus.Canceled-ish: a terminal, non-retrying state */
static void    *stub_null      (void)          { return NULL; }
static void     stub_void_p    (void *a)       { (void)a; }
static void     stub_void_pp   (void *a, void *b) { (void)a; (void)b; }

/* Counter so the log shows what the game actually asked for.  If the game ever
 * hangs at a social call, debug.log names the exact symbol to add above. */
static int s_stub_hits = 0;

/* Symbols the Play Games Unity plugin looks up natively.  This list is
 * deliberately conservative: it covers the lifecycle and sign-in entry points
 * that can block, and lets anything else fall through to the generic no-op.
 *
 * If you see "[social] unresolved native symbol: X" in debug.log followed by a
 * hang, add X here with the right shape and rebuild. */
static const struct { const char *name; void *fn; } s_tbl[] = {
  /* lifecycle */
  { "GPGS_Initialize",              (void *)&stub_void    },
  { "GPGS_Shutdown",                (void *)&stub_void    },
  { "GPGS_Pause",                   (void *)&stub_void    },
  { "GPGS_Resume",                  (void *)&stub_void    },
  /* sign-in: must return a TERMINAL status, never "pending" */
  { "GPGS_Authenticate",            (void *)&stub_status  },
  { "GPGS_ManualSignIn",            (void *)&stub_status  },
  { "GPGS_IsAuthenticated",         (void *)&stub_false   },
  { "GPGS_GetUserId",               (void *)&stub_null    },
  { "GPGS_GetDisplayName",          (void *)&stub_null    },
  /* achievements / leaderboards: accept and drop */
  { "GPGS_UnlockAchievement",       (void *)&stub_void_p  },
  { "GPGS_IncrementAchievement",    (void *)&stub_void_pp },
  { "GPGS_RevealAchievement",       (void *)&stub_void_p  },
  { "GPGS_ShowAchievementsUI",      (void *)&stub_void    },
  { "GPGS_ShowLeaderboardUI",       (void *)&stub_void    },
  { "GPGS_SubmitScore",             (void *)&stub_void_pp },
  { "GPGS_LoadScores",              (void *)&stub_null    },
};

void *pp_native_stub_lookup(const char *symbol) {
  if (!symbol) return NULL;

  for (unsigned i = 0; i < sizeof(s_tbl) / sizeof(s_tbl[0]); i++) {
    if (!strcmp(symbol, s_tbl[i].name)) {
      if (s_stub_hits < 32) {
        s_stub_hits++;
        debugPrintf("[social] stubbed native call: %s\n", symbol);
      }
      return s_tbl[i].fn;
    }
  }

  /* Anything else that is clearly part of the Play Games surface: answer with a
   * no-op rather than NULL.  A NULL here becomes a jump-to-zero inside the
   * il2cpp interop thunk, which is a hard crash; a no-op is merely a feature
   * that does nothing, which is exactly what we want on this platform. */
  if (strstr(symbol, "GPGS")     || strstr(symbol, "GooglePlay") ||
      strstr(symbol, "PlayGames")|| strstr(symbol, "Nearby")) {
    if (s_stub_hits < 32) {
      s_stub_hits++;
      debugPrintf("[social] generic no-op for unlisted Play Games symbol: %s\n", symbol);
    }
    return (void *)&stub_void;
  }

  debugPrintf("[social] unresolved native symbol: %s\n", symbol);
  return NULL;
}
