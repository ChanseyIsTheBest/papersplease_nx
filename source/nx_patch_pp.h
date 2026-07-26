/* nx_patch_pp.h -- engine patch constants for PAPERS, PLEASE
 * (Unity 2022.3.62f2, arm64, IL2CPP).
 *
 * Engine identity these were recovered from:
 *   libunity.so   10,438,952 bytes
 *   .note.unity   "2022.3.62f2_7670c08855a9"
 *   BuildID       e834a80f301c2437 (xxHash)
 *   .text         va 0x24e810 size 0x763d00
 *
 * IF YOUR libunity.so DIFFERS, EVERY OFFSET HERE IS WRONG. The patcher is
 * verify-first and will refuse to write anything, loudly, rather than corrupt
 * the binary -- but you will need to re-derive. See PORTING.md.
 *
 * The 21-entry region-granularity table itself lives in main.c's
 * nx_patch_libunity() (that is where the Zookeeper base keeps it); this header
 * holds the phase flags and the standalone single-word patches.
 *
 * A NOTE ON PROVENANCE. Papers, Please is the same Unity minor version as the
 * Zookeeper DX base, but Unity strips unused engine modules out of libunity.so
 * per game, so the layouts differ -- all 21 Zookeeper offsets were tested
 * against this binary and NONE matched. They were re-derived by searching this
 * .text for Zookeeper's known-good stock words: 14 of 21 occur exactly once,
 * and the other 7 were pinned by deltas to an already-unique neighbour, with
 * every local delta matching the reference layout exactly. All five clusters
 * were then disassembled and confirmed to be the allocator (round-up mask pair,
 * the size>>28 region-round with its 256MB constant, and two page-table walkers
 * keyed on addr>>28 and addr>>40).
 */
#ifndef NX_PATCH_PP_H
#define NX_PATCH_PP_H

/* ---- Branch forces -------------------------------------------------------
 * Zookeeper's known-good 62f2 build patches two extra sites beyond the 21
 * granularity words. The PvZ Fusion port was never able to locate them (their
 * stock encodings are far too common to word-search: 80 and 117 hits here).
 * They ARE located for Papers, Please, and the evidence is strong:
 *
 *   0x4860d0   ldr x21,[x22,#0xa8] ; cmp x21,x19 ; b.ge 0x4860e4
 *              ... bl <cond_wait> ; b <recheck>
 *   0x488a9c   ldr x8,[x19,#0x58] ; cbz/cbnz ... ; bl <cond_wait> ; b <recheck>
 *
 * Both are "lock; loop { if (ready) break; wait; } unlock" using the same
 * helper trio (lock 0x9b27a0 / wait 0x9b27b0 / unlock 0x9b27c0) -- waits that
 * can never be satisfied here. Zookeeper's forced encodings are b +5 and b +8
 * instructions, and in THIS binary those land on 0x4860e4 and 0x488abc, which
 * are exactly the two loops' exit labels. Two independent forced branch
 * distances both landing on semantically correct labels is not coincidence.
 *
 * Set to 0 if you suspect them; the port should still boot without (the PvZ
 * port shipped without these and reached gameplay). Turn off first if you see
 * threading weirdness that the granularity patch alone would not explain. */
#define PP_HAVE_BRANCH_FORCES 1

/* ---- Phase flags ---------------------------------------------------------
 * PP_HAVE_TIME_FIX -- the native TimeManager::Update re-drive. PvZ needed it
 *   because its first nativeRender blocked on a huge async scene load whose
 *   PreloadManager never integrated without a live engine clock. Papers,
 *   Please is a small 2D Haxe-hosted game with no comparable load, and the
 *   offset is libunity-build-specific, so it ships OFF. */
#define PP_HAVE_TIME_FIX 0

/* ===========================================================================
 * Single-word engine patches (each applied verify-first in main.c)
 * ======================================================================== */

/* --- FMOD: force the OpenSL ES output ------------------------------------
 * Unity's FMOD defaults to the Java AudioTrack output, which needs a JVM run
 * loop we do not have, so audio would stay silent or stall at init. The
 * output-type select matched its signature exactly once:
 *
 *   0x515d30  cmp   w0, #3
 *   0x515d34  mov   w8, #0x15      ; FMOD_OUTPUTTYPE_AUDIOTRACK (21)
 *   0x515d38  mov   w9, #0x17      ; FMOD_OUTPUTTYPE_AAUDIO     (23)
 *   0x515d3c  csel  w8, w9, w8, eq
 *   0x515d44  mov   w9, #0x16      ; FMOD_OUTPUTTYPE_OPENSL     (22)
 *   0x515d48  csel  w21, w9, w8, eq
 *   0x515d4c  ldr   x0, [x19, #0x158]
 *   0x515d50  mov   w1, w21        <-- PATCH to movz w1,#22
 *   0x515d58  bl    #0x90c1e0      ; setOutput()
 */
#define PP_OFF_FMOD_OUTPUT   0x515d50

/* --- FMOD: bypass the OpenSL buffer-geometry validation -------------------
 * The OpenSL init halves its buffer until it fits, then rejects the geometry if
 * it still does not; the Switch's geometry trips that check. The terminal b.ls
 * of the 8-word halving loop, matched exactly once:
 *
 *   0x918730  sub w10,w20,#1 ; mul w10,w10,w21 ; cmp w9,w10 ; b.ls 0x918748
 *   0x918740  lsr w9,w9,#1   ; str w9,[x19,#0x3f8]
 *   0x918748  cmp w9,w10
 *   0x91874c  b.ls 0x91875c   <-- PATCH to unconditional b
 */
#define PP_OFF_FMOD_BUFGEOM  0x91874c

/* --- Swappy (frame pacing): force-disable --------------------------------
 * The Zookeeper base never needed this; its libunity does not enable Swappy.
 * Papers, Please's DOES: libunity exports UnitySwappy_version /
 * UnitySwappy_injectTracer and registers three Swappy JNI callbacks. Swappy's
 * init spins up a Choreographer-driven thread pool and the engine joins it, but
 * the Switch has no Android Choreographer, so the pool never completes and the
 * join hangs at frame 0.
 *
 * 0x46b0bc is the cached "is frame pacing enabled?" getter -- 12 call sites,
 * every one `bl 0x46b0bc ; tbz w0,#0,<skip>`, all inside the Swappy glue block
 * that ends at the exported UnitySwappy_version (0x46baa8):
 *
 *   0x46b0bc  stp  x30,x19,[sp,#-0x10]!   <-- PATCH to mov w0,#0 ; ret
 *   0x46b0c4  ldrb w8,[x19,#0x931]        ; cached?
 *   0x46b0dc  bl   #0x7e8170              ; GetPlayerSettings()
 *   0x46b0e4  ldrb w8,[x0,#0x49a]         ; androidUseSwappy
 *   0x46b0fc  ldrb w9,[x9,#0x934]         ; force-disabled flag
 *   0x46b110  and  w0,w8,w9
 */
#define PP_OFF_SWAPPY_GETTER 0x46b0bc
#define PP_WORD_SWAPPY_FROM  0xA9BF4FFEu   /* stp x30,x19,[sp,#-0x10]! */
#define PP_WORD_SWAPPY_TO0   0x52800000u   /* mov w0, #0 */
#define PP_WORD_SWAPPY_TO1   0xD65F03C0u   /* ret        */

#endif /* NX_PATCH_PP_H */
