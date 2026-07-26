/* pp_il2cpp.h -- libil2cpp.so globals and managed RVAs for PAPERS, PLEASE
 *
 * libil2cpp.so is GAME code, so every value here is specific to this exact
 * build. Identity these were recovered from:
 *   libil2cpp.so  16,020,752 bytes
 *   BuildID       e1e5fe2cc16133a1a7bfbb467f830413d77c8915 (sha1)
 *   .text  va 0x522ee4 size 0x15fa08   (il2cpp runtime)
 *   il2cpp va 0x6828ec size 0x7c57b8   (generated game code)
 *   .data  va 0xec3658   .bss va 0xf52b90
 */
#ifndef PP_IL2CPP_H
#define PP_IL2CPP_H

/* ===========================================================================
 * 1. Boehm GC stop-the-world bridge  (libc_shim.c pthread_kill_gc)
 * ===========================================================================
 * THE MOST IMPORTANT BLOCK IN THE PORT. Wrong values here give a black screen
 * with the watchdog reporting "last frame=0".
 *
 * il2cpp's Boehm GC stops the world by pthread_kill()ing every other thread
 * with a suspend signal; each thread's handler sem_post()s an ack, and
 * GC_stop_world / GC_start_world sem_wait() on those acks. The Switch never
 * delivers POSIX signals, so the acks never arrive and the FIRST stop-the-world
 * hangs forever. The bridge makes pthread_kill itself post the ack -- but it
 * needs to know which signal numbers mean suspend/restart and where the ack
 * semaphore lives, and those are per-libil2cpp globals.
 *
 * DERIVATION (all four cross-confirmed against each other):
 *   pthread_kill's PLT stub is 0xe49070; exactly two callers exist in the
 *   runtime .text:
 *       GC_suspend_all @0x6169f8 -> call @0x616bc0, `ldr w1,[x24,#0xa94]`
 *       GC_start_world @0x616d9c -> call @0x616e28, `ldr w1,[x24,#0xa98]`
 *   In both, x24 comes from `adrp x24,#0xf52000`, so the two signal globals are
 *   0xf52a94 and 0xf52a98 -- adjacent, as expected.
 *
 *   The ack semaphore is the shared x0 of sem_init (@0x616f08), sem_wait
 *   (@0x616d50) and both sem_post calls (@0x616aac, @0x616af8): 0x116b9a0.
 *
 *   The restart-ack gate is the word guarding the second sem_post:
 *       0x616ae4  adrp x8,#0xf52000
 *       0x616ae8  ldr  w8,[x8,#0xa90]
 *       0x616aec  cbz  w8,#0x616b08     ; skip the ack if clear
 *   -> 0xf52a90, one word below the suspend signal, matching the reference.
 *
 *   Sanity check: both signal words initialise to -1 (assigned at GC_init) and
 *   the gate initialises to 1. Signals and gate are in .data; the semaphore in
 *   .bss. All consistent with Boehm's layout.
 *
 * Any Unity/IL2CPP so-loader port has to re-derive these per libil2cpp. The
 * method above is mechanical: find the two pthread_kill callers, take the w1
 * source globals (they are one apart), then the shared sem_wait/sem_post arg,
 * then the int one word below the suspend signal.
 * ------------------------------------------------------------------------- */
#define PP_GC_START_ACK_OFF   0xf52a90   /* int      : restart-ack gate        */
#define PP_GC_SUSPEND_SIG_OFF 0xf52a94   /* int      : GC_suspend_all kill arg */
#define PP_GC_RESTART_SIG_OFF 0xf52a98   /* int      : GC_start_world kill arg */
#define PP_GC_ACK_SEM_OFF     0x116b9a0  /* FakeSem* : ack semaphore storage   */

/* ===========================================================================
 * 2. JavaVM global
 * ===========================================================================
 * We do not call libil2cpp's own JNI_OnLoad (@0x5f2990): its first action logs
 * through __android_log_print, whose GOT slot in libil2cpp is mis-bound under
 * our loader -> Instruction Abort. Its one essential effect is a single global
 * store, which main.c replicates:
 *
 *   0x5f2990  stp  x30,x19,[sp,#-0x10]!
 *   0x5f299c  mov  x19, x0              ; x0 = JavaVM*
 *   0x5f29ac  bl   #0xe48e70
 *   0x5f29b4  adrp x8, #0xf58000
 *   0x5f29bc  str  x19, [x8, #0x410]    <-- the VM global
 *   0x5f29c0  bl   #0x584ecc
 *
 * NOTE vs the Zookeeper and PvZ ports: both of those additionally stored a JNI
 * handler function pointer into a SECOND global. Papers, Please's JNI_OnLoad
 * has no such store -- it is strictly simpler (one store, two calls). Do not
 * invent a second write here; there is nothing to write, and guessing would
 * scribble on an unrelated global.
 * ------------------------------------------------------------------------- */
#define PP_IL2CPP_JAVAVM_OFF  0xf58410

/* ===========================================================================
 * 3. Managed UnityEngine.Time accessors
 * ===========================================================================
 * Papers, Please references only THREE Time members; managed code stripping
 * removed the rest and they are genuinely absent from the metadata, so the
 * reference ports' 7- and 8-entry hook tables do not apply:
 *
 *   public class Time // TypeDefIndex: 2994
 *     public static float  deltaTime                    { get; }
 *     public static int    frameCount                   { get; }
 *     [NativeProperty("Realtime")]
 *     public static double realtimeSinceStartupAsDouble { get; }
 *
 * *** WATCH THE RETURN TYPE ***
 * The realtime accessor here returns a DOUBLE in d0. The other ports in this
 * lineage hook realtimeSinceStartup, which returns a FLOAT in s0. Reusing their
 * hook function verbatim hands the engine garbage; main.c uses
 * nx_realtime_since_startup_d() for this slot.
 * ------------------------------------------------------------------------- */
#define PP_RVA_get_deltaTime                     0xE2ABF4  /* float  */
#define PP_RVA_get_frameCount                    0xE2AC1C  /* int    */
#define PP_RVA_get_realtimeSinceStartupAsDouble  0xE2AC44  /* double */

#endif /* PP_IL2CPP_H */
