/* main.c -- PAPERS, PLEASE Switch wrapper entry point.
 *
 * Retargeted from the Zookeeper DX loader (same Unity minor version, 2022.3.62f2,
 * and portrait like this game). Engine offsets: nx_patch_pp.h / pp_il2cpp.h.
 *
 * Unity 2022.3 / IL2CPP. Loads libmain + libunity + libil2cpp, then drives the
 * lifecycle the Java UnityPlayer normally runs (initJni -> recreate GFX state ->
 * render loop), calling the native entry points recovered from libunity.so's
 * JNI_OnLoad (see unity_entrypoints.h). The engine owns its own EGL/GLES3 context
 * created from android_native_window(); SDL is audio/HID only.
 *
 * Heap + syscall scaffolding adapted from cr3_nx's main.c (MIT).
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "nx_gameroot.h"
#include "pp_assets.h"
#include "nx_patch_pp.h"
#include "pp_il2cpp.h"
#include <dirent.h>
#include <strings.h>  /* strncasecmp */
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "diag.h"

/* DATA_ROOT is now a FUNCTION, not a macro: the game folder is resolved at
 * runtime from the .nro's own location so the port runs from any folder under
 * sdmc:/switch (papers_please, PapersPlease, ...). See nx_gameroot.c.
 * The old macro spelling is kept as a call so the many use sites below read
 * unchanged -- but note it can no longer be string-concatenated with a literal;
 * every such site was converted to snprintf. */
#define DATA_ROOT  (game_root())
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"
/* (The Zookeeper base defined four libFirebaseCpp*.so names here. Papers,
 * Please ships no Firebase SDK -- its only Google dependency is Play Games
 * Services, handled by pp_social_stub.c -- and the macros were never referenced
 * even in the base. Removed.) */

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena. In overcommit mode (g_overcommit) this is a big *virtual* window in
 * the alias region: Unity's PROT_NONE pool reservations cost only address space
 * and physical pages are committed on demand via svcMapPhysicalMemory. In the
 * fallback path it's a fully heap-backed 256MB-aligned slab (the Switch has no
 * native overcommit). Consumed by mmap_fake/munmap_fake (libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;          /* 1 = alias-region on-demand commit */
u64    g_alias_base = 0, g_alias_size = 0;
/* captured in __libnx_initheap for logging from main() (log file isn't open yet) */
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
/* granular setup diagnostics so a failed gate tells us WHICH step bailed */
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
/* stack-region overcommit arena armer (libc_shim.c) */
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;   /* system resource size (0 => svcMapPhysicalMemory unusable) */

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Replacement icall for UnityEngine.Application::get_internetReachability.
 * Returns NetworkReachability.NotReachable (0). See the frame-0 install site
 * for why (unblocks FirebaseManager.IsGetMessage / the boot coroutine). The
 * il2cpp icall ABI for this static getter is "int32_t func(MethodInfo*)"; we
 * ignore the hidden arg and just report no network. */


/* Unity's native time base is frozen in our environment (it never advances the
 * engine clock -- likely it expects Android Choreographer frame timestamps we
 * don't deliver). Every managed Time.* accessor therefore reads a frozen value:
 * deltaTime==0, time/realtimeSinceStartup constant. That freezes all time-based
 * game logic -- DOTween (the boot logo fade), WaitForSeconds, etc. -- which is
 * what holds the black screen (the fade never completes, so boot never starts).
 * Work around it by driving our own monotonic frame clock and redirecting the
 * managed Time accessors to it. nx_time_tick() runs once per render-loop frame. */
static volatile float  g_unity_dt   = 1.0f / 60.0f;
static volatile double g_unity_time = 0.0;
static volatile uint32_t g_frame_count = 0;   /* Time.frameCount source */
static uint64_t g_time_prev_ns  = 0;
static uint64_t g_time_start_ns = 0;
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void nx_time_tick(void) {
  uint64_t now = nx_now_ns();
  g_frame_count++;                    /* advance Time.frameCount once per frame */
  if (!g_time_start_ns) g_time_start_ns = now;
  if (g_time_prev_ns) {
    double dt = (double)(now - g_time_prev_ns) / 1e9;
    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;            /* clamp, mirrors Unity maximumDeltaTime */
    g_unity_dt = (float)dt;
    g_unity_time += dt;
  }
  g_time_prev_ns = now;
}
static float nx_delta_time(void) { return g_unity_dt; }
/* Retained but currently unhooked: the Zookeeper base bound this to
 * Time.get_time / get_unscaledTime / get_fixedTime. Papers, Please's metadata
 * contains NONE of those three -- managed code stripping removed them -- so
 * there is nothing to hook it to. Kept because a less aggressively stripped
 * build of the game would want it back. */
__attribute__((unused))
static float nx_time_f(void)     { return (float)g_unity_time; }
static int   nx_frame_count(void){ return (int)g_frame_count; }
uint32_t     port_frame_count(void){ return g_frame_count; } /* audio pump warmup gate */
static float nx_realtime_since_startup(void) {
  uint64_t now = nx_now_ns();
  if (!g_time_start_ns) g_time_start_ns = now;
  return (float)((double)(now - g_time_start_ns) / 1e9);
}

/* Papers, Please's metadata exposes realtimeSinceStartupAsDouble, NOT the float
 * realtimeSinceStartup the other ports in this lineage hook.  The managed
 * signature returns a double, so the value must come back in d0 -- returning a
 * float here would leave the engine reading whatever happened to be in the top
 * half of the register.  Same clock, different ABI slot. */
static double nx_realtime_since_startup_d(void) {
  return (double)nx_realtime_since_startup();
}

/* fbstub42: TimeManager::Update entry hook. The Switch port's player loop drives
 * Update with a frozen vsync timestamp as newTime, so deltaTime collapses to the
 * 1e-5 floor and every native time reader (the PreloadManager included) starves,
 * which is what wedges async scene loading (the resident-scene black screen). We
 * redirect Update's entry (libunity 0x446114) here, replay its tiny prologue
 * (frameCount++, aux counter++, pause check), then re-enter its body (0x446138 --
 * frameless, re-reads everything from x0) with newTime = GetTimeSinceStartup(),
 * the engine's own monotonic clock (0x446578) which DOES advance. Update then
 * derives all deltaTime variants and m_Time correctly.
 *
 * *** THESE THREE OFFSETS ARE THE ZOOKEEPER BASE'S, NOT DERIVED FOR THIS GAME. ***
 * They are in range for Papers, Please's libunity, so they will not fault, but
 * they point at whatever happens to live there -- which is NOT TimeManager.
 * That is why PP_HAVE_TIME_FIX ships 0 and this function is never called. If you
 * ever need the re-drive, re-derive all three against your own libunity first
 * (PORTING.md S3 describes the signature-matching method). Do not simply flip
 * the flag. */
static double (*g_unity_get_time)(void *)            = NULL; /* 0x446578 GetTimeSinceStartup(tm) */
static void   (*g_unity_update_body)(void *, double) = NULL; /* 0x446138 Update body             */
static void nx_time_update_hook(void *tm) {
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;            /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;            /* aux counter++           */
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return; /* paused -> early return  */
  double now = g_unity_get_time ? g_unity_get_time(tm) : 0.0;
  if (g_unity_update_body) g_unity_update_body(tm, now);
}
/* Only referenced when PP_HAVE_TIME_FIX is 1 (see nx_patch_pp.h); it ships 0
 * because this game has no async scene-load pipeline needing the re-drive, and
 * the offset it would patch is libunity-build-specific. */
__attribute__((unused))
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  g_unity_get_time    = (double (*)(void *))(ub + 0x446578);
  g_unity_update_body = (void (*)(void *, double))(ub + 0x446138);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  so_patch_code((void *)(ub + 0x446114), stub, sizeof stub);
  debugPrintf("[boot] installed TimeManager::Update hook @libunity+0x446114 "
              "(newTime <- GetTimeSinceStartup)\n");
}

/* --- boot finish-flag probe -------------------------------------------------
 * State 6 (SetFinishFlag) walks A=*(il2cpp+0x21fd0718); B=*A; obj=*(B+0x20);
 * obj2=*(*(obj+0xc0)+0x10); holder=*(obj2+0xb8 then deref); and writes
 * *(holder+0x10)=1 -- but ONLY if holder is non-null (else it stays at state 6
 * forever). CheckInitializeFinish polls the same obj2 (+0xe0 / +0x135). We read
 * the live chain so we can see exactly which link is null / whether the finish
 * flag ever gets set, without any destructive patching. */
static int nx_rd(uintptr_t a) {
  MemoryInfo mi; u32 pi;
  if (a < 0x1000) return 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
  if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) return 0;
  return 1;
}
/* Safe-deref helper. Its only caller in the base was nx_probe_finish(), a
 * Zookeeper-specific il2cpp diagnostic removed for this port. Kept: it is the
 * right tool if you ever need to walk a managed object graph from the loader. */
__attribute__((unused))
static uintptr_t nx_pd(uintptr_t a) { return nx_rd(a) ? *(volatile uintptr_t *)a : 0; }


/* libunity ~17M + libil2cpp ~36M + headroom for relocated segments */
#define SO_REGION_BYTES (160u * 1024 * 1024)

/* Reserve the virtual arena window at the TOP of the alias region (deep in the
 * 64GB region, where libnx never allocates) after verifying it is fully unmapped.
 * No physical backing yet -- pages are committed on demand. */
/* Unused on the current allocation path (the OC window is reserved in
 * libc_shim.c). Inherited from the base; kept for the alias-region experiments
 * documented around MMAP_VIRT_RESERVE in config.h. */
__attribute__((unused))
static void *overcommit_reserve_window(size_t size) {
  size = (size + MMAP_ARENA_ALIGN - 1) & ~(MMAP_ARENA_ALIGN - 1);
  if (!g_alias_base || g_alias_size < size + MMAP_ARENA_ALIGN) return NULL;
  u64 top = g_alias_base + g_alias_size;
  u64 win = (top - size) & ~(MMAP_ARENA_ALIGN - 1);
  u64 a = win;
  while (a < win + size) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return NULL;
    if (mi.type != MemType_Unmapped) return NULL;   /* collision -> bail */
    a = mi.addr + mi.size;
  }
  return (void *)win;
}

/* virtmemFindStack refuses large windows even when the stack region has room, so
 * scan the region directly via svcQueryMemory for the largest 256MB-aligned
 * unmapped hole (and log the whole map for diagnosis). svcMapMemory only aliases
 * into the stack region, so the OC window must live here. */
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;                              /* no-progress guard */
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

/* Try to set up alias-region overcommit, recording each step's outcome into the
 * g_oc_* globals (logged from main). Alias-region overcommit turned out to be
 * impossible on this process: svcMapPhysicalMemory requires a non-zero kernel
 * "system resource" pool (for page-table/block bookkeeping) and our title-override
 * process has none -> it returns InvalidState (0xfa01). The unsafe pool
 * (svcMapPhysicalMemoryUnsafe) is ~44MB and already consumed. So we just record the
 * diagnostics and stay on the fully heap-backed arena. (Confirmed via Atmosphere
 * kern_svc_physical_memory.cpp: `R_UNLESS(GetTotalSystemResourceSize() > 0,
 * ResultInvalidState())`.) */
static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap the engine mallocs from. (Verbatim from cr3_nx.) */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  /* Preferred path: alias-region overcommit. Secures the virtual window and
   * test-commits a page FIRST, then shrinks the heap to [newlib + so_zone] so the
   * freed physical (~2.5GB) is available for on-demand commits. Everything is
   * secured before the shrink so a failure can't strand us with a shrunk heap. */
  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed 256MB-aligned arena (no overcommit). */
  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);   /* clamp to RAM */
    fake_heap_size = size - so_zone - arena_sz - big_align;       /* newlib gets the rest */
  } else {
    /* heap too small for a dedicated arena (e.g. applet mode): skip it; the mmap
     * allocator falls back to a memalign-backed bitmap arena. */
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* Verify the SD layout before we touch the engine.
 *
 * The base checked for "assets/bin/Data/data.unity3d" because ITS game shipped
 * the single-bundle layout. Papers, Please does not ship a data.unity3d at all:
 * it uses the classic non-bundled layout (globalgamemanagers + level0 +
 * sharedassets0 + friends), so demanding that file was a guaranteed false
 * failure -- the loader refusing to start over a file the game never had.
 *
 * So this checks for what is actually there, and accepts EITHER layout:
 * a build with data.unity3d is fine, and so is one without. The only things
 * treated as fatal are the ones nothing can proceed without.
 *
 * Everything else is reported but not fatal. A missing optional file gives a
 * named warning in debug.log, which is far more useful than a hard stop -- the
 * player finds out which file to re-copy instead of just "check your SD card".
 */
static int have_rel(const char *rel) {
  char path[768];
  struct stat st;
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, rel);
  return stat(path, &st) == 0;
}

static void check_data(void) {
  /* --- 1. the engine libraries: nothing works without these --------------- */
  const char *libs[] = { LIB_MAIN, LIB_UNITY, LIB_IL2CPP };
  for (unsigned i = 0; i < sizeof(libs)/sizeof(*libs); i++) {
    if (!have_rel(libs[i]))
      fatal_error("Missing engine library:\n%s\n\n"
                  "Copy it from your APK's lib/arm64-v8a/ folder into\n%s",
                  libs[i], DATA_ROOT);
  }

  /* --- 2. il2cpp metadata: the managed side cannot start without it ------- */
  if (!have_rel("assets/bin/Data/Managed/Metadata/global-metadata.dat"))
    fatal_error("Missing assets/bin/Data/Managed/Metadata/global-metadata.dat\n\n"
                "Copy the whole assets/ folder from your APK into\n%s", DATA_ROOT);

  /* --- 3. the scene data, in EITHER layout -------------------------------- */
  int bundled  = have_rel("assets/bin/Data/data.unity3d");
  int classic  = have_rel("assets/bin/Data/globalgamemanagers");
  if (!bundled && !classic)
    fatal_error("No Unity scene data found under\n%s/assets/bin/Data\n\n"
                "Expected either globalgamemanagers (this game's layout)\n"
                "or data.unity3d. Copy the whole assets/ folder from your APK.",
                DATA_ROOT);
  debugPrintf("[boot] asset layout: %s\n",
              bundled ? "bundled (data.unity3d)" : "classic (globalgamemanagers)");

  /* --- 4. split-file targets: joined, or still in pieces ------------------ */
  if (classic) {
    const char *split_targets[] = {
      "assets/bin/Data/globalgamemanagers.assets",
      "assets/bin/Data/sharedassets0.assets",
    };
    for (unsigned i = 0; i < sizeof(split_targets)/sizeof(*split_targets); i++) {
      char s0[768];
      snprintf(s0, sizeof s0, "%s.split0", split_targets[i]);
      if (!have_rel(split_targets[i]) && !have_rel(s0))
        fatal_error("Missing %s\n(and no .split0 to rebuild it from)\n\n"
                    "Copy the whole assets/ folder from your APK.",
                    split_targets[i]);
    }
  }

  /* --- 5. everything else: warn, never stop ------------------------------ */
  const char *optional[] = {
    "assets/bin/Data/level0",
    "assets/bin/Data/boot.config",
    "assets/bin/Data/unity_app_guid",
    "assets/bin/Data/sharedassets0.resource",
    "assets/bin/Data/unity default resources",
    "assets/bin/Data/Resources/unity_builtin_extra",
    "assets/bin/Data/Managed/Resources/mscorlib.dll-resources.dat",
    "assets/loc/en.zip",
  };
  int missing = 0;
  for (unsigned i = 0; i < sizeof(optional)/sizeof(*optional); i++) {
    if (!have_rel(optional[i])) {
      debugPrintf("[boot] NOTE: %s is missing (continuing)\n", optional[i]);
      missing++;
    }
  }
  debugPrintf("[boot] asset check passed%s\n",
              missing ? " (with notes above)" : "");
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  debugPrintf("[mod] %-14s virtbase=%p size=0x%zx  (resolve: addr - virtbase = vaddr)\n",
              name, mod->load_virtbase, mod->load_size);
  crx_resolve_imports(mod);   /* so_resolve(mod, dynlib_functions, ...) */
  /* NOTE: so_patch_stack_canaries() intentionally NOT called. Per-thread bionic
   * TLS (install_bionic_tls) makes the engine's stack-protector guard consistent,
   * so the canary checks pass on their own. NOPing 2000+ b.ne sites risked a
   * false-positive in non-canary code (e.g. allocator list logic) -> corruption. */
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* ---------------------------------------------------------------------------
 * In-memory libunity patch (ported from VLN's nx_patch_unity_regions): the SD
 * card now ships the STOCK libunity.so and the boot patches it after load,
 * instead of distributing a pre-modified binary. 23 instruction words, from a
 * byte-exact diff of the known-good patched .so vs stock (Unity 2022.3.62f2):
 * 21 sites relax the allocator's memory-region granularity 256MB->64MB so the
 * engine fits the so_loader address space on a 4GB Switch, plus two branch
 * forces (0x5d24cc cond->uncond, 0x5d4e98 ldr->skip) from the same known-good
 * build. Verify-first like VLN: every original word must match before anything
 * is written; a fully pre-patched .so is detected and accepted; any other
 * mismatch leaves the binary untouched (different Unity build) with a loud log.
 * ------------------------------------------------------------------------- */
static int nx_patch_libunity(uintptr_t ub) {
  /* Papers, Please's own libunity.so.  See nx_patch_pp.h for how every offset
   * was recovered and cross-checked -- all 21 granularity sites plus BOTH
   * branch forces, which the PvZ Fusion port was never able to locate. */
  static const struct { uint32_t off, from, to; } P[] = {
    /* --- 21 region-granularity sites (256MB -> 64MB) --- */
    {0x279f70, 0x12be0009, 0x12bf8009}, {0x279f78, 0x92648d36, 0x92669536},
    {0x27a818, 0x52a20009, 0x52a08009}, {0x27e320, 0xd35cfd29, 0xd35afd29},
    {0x27e324, 0x52a2000a, 0x52a0800a}, {0x27e7cc, 0x12be000a, 0x12bf800a},
    {0x27e7d4, 0x92648d36, 0x92669536}, {0x280754, 0xd35cdc33, 0xd35ad433},
    {0x280758, 0xd35cfd15, 0xd35afd15}, {0x2807e8, 0x52a20008, 0x52a08008},
    {0x280b24, 0xd35cfc28, 0xd35afc28}, {0x280b34, 0x92646c28, 0x92667428},
    {0x280b3c, 0xd35c9c2a, 0xd35a942a}, {0x280b50, 0xb25c6feb, 0xb25e77eb},
    {0x280b54, 0xd35cdc29, 0xd35ad429}, {0x280b58, 0xf2a2000b, 0xf2a0800b},
    {0x280b98, 0xcb0a7108, 0xcb0a6908}, {0x280bb0, 0xd368fc28, 0xd366fc28},
    {0x280bc0, 0xd35c9c29, 0xd35a9429}, {0x282874, 0xd368fc28, 0xd366fc28},
    {0x28288c, 0xd35c9e89, 0xd35a9689},
#if PP_HAVE_BRANCH_FORCES
    /* --- 2 branch forces: condition-wait loops that can never be satisfied ---
     * Both are "lock; loop { if (ready) break; cond_wait; } unlock" using the
     * same helper trio (lock 0x9b27a0 / wait 0x9b27b0 / unlock 0x9b27c0).  The
     * forced unconditional branch skips straight to each loop's exit label:
     *   0x4860d0  b.ge 0x4860e4  ->  b 0x4860e4   (target == the exit label)
     *   0x488a9c  ldr x8,[x19,#0x58] -> b 0x488abc (target == the exit label)
     * Both targets were verified against THIS binary's disassembly. */
    {0x4860d0, 0x540000aa, 0x14000005}, {0x488a9c, 0xf9402e68, 0x14000008},
#endif
  };
  const int N = (int)(sizeof P / sizeof P[0]);
  int stock = 0, patched = 0;
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur == P[i].from) stock++;
    else if (cur == P[i].to) patched++;
    else {
      debugPrintf("[patch] libunity word mismatch @+0x%x: have 0x%08x want 0x%08x -> SKIP all (unknown libunity build; game may fail to start)\n",
                  (unsigned)P[i].off, cur, P[i].from);
      return 0;
    }
  }
  if (patched == N) {
    debugPrintf("[patch] libunity already pre-patched (%d sites) -- ok\n", N);
    return 1;
  }
  if (stock != N) {
    debugPrintf("[patch] libunity PARTIALLY patched (%d/%d) -> SKIP (won't mix builds)\n", patched, N);
    return 0;
  }
  for (int i = 0; i < N; i++)
    so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to);
  debugPrintf("[patch] libunity patched in-memory: region granularity 256MB->64MB + branch forces (%d sites)\n", N);
  return 1;
}

int main(int argc, char *argv[]) {
  /* FIRST: work out which folder under sdmc:/switch we are running from, before
   * anything tries to open the log (which lives in it). Resolution order is
   * argv[0] -> scan sdmc:/switch for the folder holding libil2cpp.so ->
   * the compile-time GAME_HOME default. See nx_gameroot.c. */
  game_root_init(argc, argv);

  socketInitializeDefault();
  debugPrintf("[boot] === papersplease_nx start (Unity 2022.3.62f2 / IL2CPP) ===\n");
  debugPrintf("[boot] game root resolved to '%s' (argv0=%s)\n",
              DATA_ROOT, (argc > 0 && argv && argv[0]) ? argv[0] : "(none)");

  /* Load config.txt. Missing -> autogenerate a documented one with the defaults;
   * holding retired options (`language`, and the old landscape `portrait 0`)
   * -> rewrite it without them. Language is no longer a setting here: the game
   * has its own in-game selector, and lang_code() just reports the Switch system
   * language as the initial choice. */
  {
    extern const char *nx_resolved_language(void);   /* jni_fake.c */
    char cfgpath[320];
    game_path(cfgpath, sizeof cfgpath, CONFIG_NAME);
    int crc = read_config(cfgpath);
    if (crc != 0)
      write_config(cfgpath);
    debugPrintf("[boot] config: portrait=%d (%s), dpi=%d, initial language=%s%s\n",
                config.portrait,
                config.portrait == 2 ? "90 CCW" : "90 CW",
                nx_screen_dpi(),
                nx_resolved_language(),
                crc < 0 ? "  [config.txt created]" : crc > 0 ? "  [config.txt rewritten]" : "");
  }

  /* Sweep Unity's case-sensitivity probe files: CASESENSITIVETEST<guid> strays
   * from older builds, plus the single hidden scratch the probe is redirected
   * to now (libc_shim.c casetest_redirect). */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, "%s/%s", DATA_ROOT, de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  /* Papers, Please ships NO data.unity3d. It uses the classic non-bundled Unity
   * layout, and it was built with "Split Application Binary", so the two big
   * serialized files arrive as 1 MB chunks (.split0 .. .splitN). libunity has a
   * native reader for those, but it is bound to the APK/AAssetManager path we
   * do not have -- our files are loose on the SD card. Put them back together
   * once, here, into the names the engine expects in the ordinary case.
   * Idempotent, verified against each file's own header, and a no-op after the
   * first launch. Must happen BEFORE the engine libraries load. */
  pp_assets_prepare();

  /* CWD fix (mirrors MMX enter_data_dir): title-override / hbloader leaves the
   * working dir at the .nro folder or the SD root, NOT the game dir. Unity &
   * il2cpp read many files through *relative* paths ("assets/bin/Data/...") and
   * our basename_fallback stats relative to cwd, so a wrong cwd silently yields
   * empty/missing reads -> NULL il2cpp classes. chdir into DATA_ROOT so every
   * relative read resolves under sdmc:/switch/zookeeper. (Absolute "sdmc:/..."
   * reads are unaffected.) */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    char cwd2[256] = {0};
    getcwd(cwd2, sizeof cwd2);
    struct stat st;
    /* Probe files this game actually has -- the base probed data.unity3d, which
     * Papers, Please does not ship, so it always read 0 and looked alarming. */
    int reach_ggm  = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_guid = stat("assets/bin/Data/unity_app_guid", &st) == 0;
    int reach_lvl0 = stat("assets/bin/Data/level0", &st) == 0;
    debugPrintf("[boot] cwd was '%s' -> chdir(%s)=%d -> '%s'\n", cwd, DATA_ROOT, rc, cwd2);
    debugPrintf("[boot] reachable(rel): globalgamemanagers=%d level0=%d metadata=%d unity_app_guid=%d\n",
                reach_ggm, reach_lvl0, reach_meta, reach_guid);
  }

  /* Force libunity to RE-EXTRACT il2cpp resources every boot. Observed: when
   * extraction is skipped (il2cpp/unity.ver present), il2cpp mmaps the extracted
   * global-metadata.dat and crashes in Class::Init(NULL); when extraction RUNS,
   * il2cpp uses the full source it reads for the copy and gets past that point.
   * The extracted copy is bad because our shim doesn't flush a writable
   * file-backed mmap back to disk, so it lands truncated. Removing the extracted
   * markers makes libunity redo the extraction each boot (uses the good source).
   * Proper fix = flush writable file-backed mmaps on munmap (tracked separately). */
  {
    char p1[320], p2[320], p3[320];
    game_path(p1, sizeof p1, "il2cpp/unity.ver");
    game_path(p2, sizeof p2, "il2cpp/Metadata/global-metadata.dat");
    game_path(p3, sizeof p3, "il2cpp/Resources/mscorlib.dll-resources.dat");
    int a = unlink(p1);
    int b = unlink(p2);
    int c = unlink(p3);
    debugPrintf("[boot] force re-extract: unlink unity.ver=%d metadata=%d resources=%d\n", a, b, c);
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem layout: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    if (g_overcommit)
      debugPrintf("[boot] OVERCOMMIT on: heap shrunk to %u MB, freed %u MB physical; "
                  "arena reserved virtual @ %p (commit on demand)\n",
                  g_oc_heap_mb, g_oc_freed_mb, g_mmap_arena_base);
    else
      debugPrintf("[boot] OVERCOMMIT off (heap-backed): system_resource=%u MB "
                  "(svcMapPhysicalMemory needs >0; unsafe pool exhausted). map_hint=%d alias=%u MB\n",
                  (unsigned)(g_oc_sysres >> 20), g_oc_hint_map, g_oc_alias_mb);
  }

  /* Overcommit feasibility probe. Proper PROT_NONE overcommit on Switch needs a
   * physical-backing primitive (svcMapMemory in the stack region, or
   * svcMapPhysicalMemory in the alias region) plus a region large enough to hold
   * Unity's multi-GB reservations. MMX found svcMapMemory caps at ~2-3 pools (the
   * stack region is ~1GB). Log the region sizes + which mapping svc are granted so
   * we can size/choose the real overcommit (or rule it out) from real numbers. */
  {
    struct { const char *nm; int a, s; } R[] = {
      { "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
      { "heap",  InfoType_HeapRegionAddress,  InfoType_HeapRegionSize  },
      { "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
    };
    for (unsigned i = 0; i < 3; i++) {
      u64 a = 0, s = 0;
      svcGetInfo(&a, R[i].a, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&s, R[i].s, CUR_PROCESS_HANDLE, 0);
      debugPrintf("[probe] region %-5s base=0x%lx size=%u MB\n",
                  R[i].nm, (unsigned long)a, (unsigned)(s >> 20));
    }
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[probe] mem total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20),
                (unsigned)((tot - used) >> 20));
    debugPrintf("[probe] svc hinted: MapPhysicalMemory(0x2c)=%d UnmapPhysical(0x2d)=%d "
                "MapMemory(0x24)=%d UnmapMemory(0x25)=%d\n",
                envIsSyscallHinted(0x2c), envIsSyscallHinted(0x2d),
                envIsSyscallHinted(0x24), envIsSyscallHinted(0x25));
  }

  /* Decisive probe: does svcMapMemory accept a dst in the (unmapped upper) HEAP
   * region? The stack region works but is only ~2GB (~8 regions). The heap region
   * is 8GB; its upper ~5GB sits unmapped above our heap. If svcMapMemory works
   * there too, we can host Unity's 256MB pools in ~7GB of backable address space
   * (~28 regions) without any libunity patching. Pure diagnostic: map 1 page,
   * verify the sentinel reads back, unmap. */
  {
    u64 hbase = 0, hsize = 0;
    svcGetInfo(&hbase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&hsize, InfoType_HeapRegionSize,    CUR_PROCESS_HANDLE, 0);
    u64 probe = 0, a = hbase, end = hbase + hsize;
    while (a < end) {
      MemoryInfo mi; u32 pi;
      if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
      if (mi.addr + mi.size <= a) break;
      if (mi.type == MemType_Unmapped && mi.size >= MMAP_ARENA_ALIGN) {
        u64 al = (mi.addr + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
        if (al + 0x1000 <= mi.addr + mi.size) { probe = al; break; }
      }
      a = mi.addr + mi.size;
    }
    if (probe) {
      void *src = memalign(0x1000, 0x1000);
      if (src) {
        *(volatile u32 *)src = 0xABCD1234;
        Result rc = svcMapMemory((void *)probe, src, 0x1000);
        if (R_SUCCEEDED(rc)) {
          u32 v = *(volatile u32 *)probe;
          Result u = svcUnmapMemory((void *)probe, src, 0x1000);
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx rc=0x%x read=0x%x unmap=0x%x WORKS=%d\n",
                      (unsigned long)probe, rc, v, u, v == 0xABCD1234);
          if (R_SUCCEEDED(u)) free(src);
        } else {
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx FAILED rc=0x%x\n",
                      (unsigned long)probe, rc);
          free(src);
        }
      }
    } else {
      debugPrintf("[heapprobe] no unmapped 256MB-aligned spot found in heap region\n");
    }
  }

  /* Arm the stack-region overcommit arena. The boot probe confirmed svcMapMemory
   * aliases heap pages into the stack region; Unity reserves ~2.8GB of PROT_NONE
   * pools but commits only ~80MB. Reserve a 1280MB stack-region window (cheap
   * address space) + a 256MB heap commit-pool; the OC arena (libc_shim.c) then
   * holds Unity's big reservations there and aliases pool pages in on mprotect.
   * Any failure leaves OC disabled and the engine runs on the heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);   // keep libnx thread stacks out
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES))
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p, heap-backed arena %u MB "
                    "(total reserve %u MB)\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool,
                    (unsigned)(g_mmap_arena_size >> 20),
                    (unsigned)((winsz + g_mmap_arena_size) >> 20));
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole (win=%p sz=%u MB rv=%p) -> heap-backed only\n",
                  win, (unsigned)(winsz >> 20), (void *)rv);
    }
  }

  /* fbstub45 PORTRAIT: ZOOKEEPER is a portrait game. Report a portrait surface
   * (W<H) everywhere Unity reads dimensions so the engine renders upright. The
   * compositor stretches the portrait buffer onto the landscape panel. (Stable;
   * the 640x1137 native-render attempt crashed early in boot -- the game's
   * pipeline appears to depend on its Screen.SetResolution low-res path.) */
  if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1080; screen_height = 1920; }
  else                                                         { screen_width = 720;  screen_height = 1280; }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  check_data();

  /* load the three modules; libil2cpp resolves its engine calls against libunity
   * module-to-module during relocation. */
  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  debugPrintf("[boot] loaded libmain   @ virtbase %p\n", (void *)main_mod.load_virtbase);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  debugPrintf("[boot] loaded libunity  @ virtbase %p\n", (void *)unity_mod.load_virtbase);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  debugPrintf("[boot] loaded libil2cpp @ virtbase %p\n", (void *)il2cpp_mod.load_virtbase);
  /* hand the il2cpp exec base to the GC stop-the-world bridge in libc_shim.c so
   * our pthread_kill can ack the Boehm GC's (undeliverable) suspend/restart
   * signals via its semaphore at il2cpp+0x25936c0. */
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  /* Firebase native libs are intentionally NOT loaded. The first scene's
   * FirebaseManager only advances when the managed dependency check reports
   * DependencyStatus.Available(0); on a Switch there is no Google Play Services,
   * so the real libs could never report that (they return UnavailableMissing)
   * AND libFirebaseCppApp crashes our loader in its JNI_OnLoad logging path. We
   * instead answer the SDK's native P/Invoke lookups with stubs (firebase_stub.c
   * via dlsym_fake) that make the check resolve to Available. The 4 .so files can
   * be deleted from sdmc:/switch/zookeeper/. Firebase is cosmetic here (RemoteConfig
   * banner/news textures), so stubbing it costs only those images. */
  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed (canary-patch disabled)\n");

  /* Patch libunity AFTER finalize/flush -- the same point every other libunity
   * patch below runs at. so_patch_code aliases the target pages via
   * svcMapProcessMemory, but the module's segments must be finalized (mapped
   * with their final RX perms and relocated) first; doing it right after
   * load_module faulted at boot (fbstub94: stock .so on SD, PC in the patch
   * path before finalize). */
  nx_patch_libunity((uintptr_t)unity_mod.load_virtbase);

  /* Force FMOD to use its native OpenSL ES output instead of Unity's Java
   * AudioTrack driver.
   *
   * Mechanism (fully traced in the game binary): Unity's AudioManager FMOD init
   * at libunity+0x6be9c0 calls FMOD::System::setOutput (+0xcda3b4) with a
   * requested FMOD_OUTPUTTYPE in w1, derived at +PP_OFF_FMOD_OUTPUT (mov w1,w21). FMOD's
   * setOutput walks the registered output list (+0xc80e84 loop) and matches the
   * requested type against each output's type field at output+0x78 (copied there
   * from the output description by the registrar +0xc744fc). The type constants,
   * read straight from each getDescriptionEx's desc+0x78:
   *     AudioTrack = 21 (0x15)   <- the default request; needs the JVM run loop
   *     OpenSL ES  = 22 (0x16)   <- callback-driven, self-driving via our shim
   * Default request is 21 (logged previously as "requested output: 21"), so the
   * Java AudioTrack output is selected and, with no JVM consumer, stays silent.
   *
   * fbstub63: rewrite the requested type at the setOutput call site from
   * "mov w1,w21" to "movz w1,#22", so Unity asks FMOD for OPENSL. FMOD finds the
   * registered OpenSL output (type 22), inits it -> dlopen(libOpenSLES.so) ->
   * slCreateEngine (our opensles.c shim) -> the engine drives its own callback
   * buffer queue. No Java handshake, correct lifecycle. The registration path is
   * left untouched (both AudioTrack and OpenSL register normally with their real
   * type fields). 0x2A1503E1 (mov w1,w21) -> 0x528002C1 (movz w1,#0x16). */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    uint32_t req_opensl = 0x528002C1u; /* movz w1, #22 (FMOD_OUTPUTTYPE OPENSL) */
    so_patch_code((void *)(ub + PP_OFF_FMOD_OUTPUT), &req_opensl, sizeof req_opensl);
    debugPrintf("[fmod] output forced to OpenSL(22) @libunity+0x%x = 0x%08x (want 0x528002c1)\n",
                (unsigned)PP_OFF_FMOD_OUTPUT,
                *(volatile uint32_t *)(ub + PP_OFF_FMOD_OUTPUT));

    /* fbstub66: neutralise FMOD's OpenSL buffer-geometry validation.
     *
     * After slCreateEngine succeeds, FMOD's OpenSL init (+0xce67a0 -> continuation
     * +0xce6840) validates the output period against the DSP mixer buffer at
     * +0xce68d4..+0xce6924. It reads {sampleRate, framesPerBuffer} from the
     * AudioManager getProperty values (our jni_fake.c: 48000 / 64) and returns
     * FMOD error 60 ("Error initializing output device") if:
     *     sampleRate == 0, OR framesPerBuffer == 0, OR
     *     framesPerBuffer > (dspNumBuffers-1)*dspBufferLength  even after one halving.
     * dspNumBuffers (w20) / dspBufferLength (w21) come from the game's BAKED
     * AudioSettings (AudioSettings::GetDSPBufferSize, +0x646888) and pass straight
     * through the init wrapper +0xce63a8 unchanged. This title's baked buffer is
     * degenerate enough that even a 64-frame period fails the bound -- consistent
     * with dspNumBuffers == 1, which makes the bound (1-1)*len = 0 so NO positive
     * period can ever satisfy it. Reporting a smaller framesPerBuffer alone cannot
     * fix that case, so we also force the final bound check to pass.
     *
     * Patch the terminal compare-branch at +PP_OFF_FMOD_BUFGEOM from "b.ls 0xce6930"
     * (0x54000089) to an unconditional "b 0xce6930" (0x14000004): the success path
     * at +0xce6930 then always runs and builds the audio player from the sane
     * sampleRate/channels. The buffer count it derives, N = (w20*w21)/period
     * (+0xce697c), stays >= 1 because we report a small 64-frame period. The two
     * zero-guards above (+0xce68ec/+0xce68fc) are left intact and pass (48000/64
     * are both non-zero). */
    uint32_t b_uncond = 0x14000004u; /* b #0xce6930 (was b.ls, 0x54000089) */
    so_patch_code((void *)(ub + PP_OFF_FMOD_BUFGEOM), &b_uncond, sizeof b_uncond);
    debugPrintf("[fmod] OpenSL buffer-geometry check bypassed @libunity+0x%x = 0x%08x (want 0x14000004)\n",
                (unsigned)PP_OFF_FMOD_BUFGEOM,
                *(volatile uint32_t *)(ub + PP_OFF_FMOD_BUFGEOM));

    /* ---- Swappy (Android frame pacing): force-disable ---------------------
     * The Zookeeper base never needed this: its libunity does not enable
     * Swappy.  Papers, Please's DOES -- libunity exports UnitySwappy_version /
     * UnitySwappy_injectTracer and registers three Swappy JNI callbacks
     * (nOnChoreographer, nOnRefreshPeriodChanged, nSetSupportedRefreshPeriods).
     * Swappy's init spins up a Choreographer-driven thread pool and the engine
     * joins it; the Switch has no Android Choreographer to deliver frame
     * callbacks, so the pool never completes and the join hangs at frame 0.
     *
     * PP_OFF_SWAPPY_GETTER is the cached "is frame pacing enabled?" getter --
     * 12 call sites, all `bl <getter> ; tbz w0,#0,<skip>`, all inside the
     * Swappy glue block.  Forcing it to return 0 makes every site take the
     * disabled path: plain eglSwapBuffers, no pacing threads, no join. */
    {
      uint32_t off_pacing[2] = { PP_WORD_SWAPPY_TO0, PP_WORD_SWAPPY_TO1 };
      if (*(volatile uint32_t *)(ub + PP_OFF_SWAPPY_GETTER) == PP_WORD_SWAPPY_FROM) {
        so_patch_code((void *)(ub + PP_OFF_SWAPPY_GETTER), off_pacing, sizeof off_pacing);
        debugPrintf("[pace] frame-pacing (Swappy) force-disabled @libunity+0x%x\n",
                    (unsigned)PP_OFF_SWAPPY_GETTER);
      } else {
        debugPrintf("[pace] SKIP Swappy-disable: +0x%x = 0x%08x, not `stp x30,x19` -- "
                    "libunity differs from the one this port was built against\n",
                    (unsigned)PP_OFF_SWAPPY_GETTER,
                    *(volatile uint32_t *)(ub + PP_OFF_SWAPPY_GETTER));
      }
    }
  }

  /* (A Zookeeper-era diagnostic lived here: it read libunity+0xc44c34 to check
   * whether a stack-canary patch had reached the executed code. It is REMOVED.
   * That offset is inside Zookeeper's larger libunity; Papers, Please's is only
   * 0x9F4928 bytes, so the read landed 2.4 MB past the end of the mapping and
   * took a Data Abort before the engine ever started. The canary patching it
   * was investigating is not done by this port either -- so_patch_stack_canaries()
   * is deliberately not called; per-thread bionic TLS makes the guard consistent
   * instead. Nothing to probe, nothing to keep.) */

  /* The main thread runs init_array + the engine lifecycle; give it its own
   * stable bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  /* fake JNI + our environment, then HID */
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  /* resolve UnityPlayer natives (load_virtbase + recovered offsets) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);
  debugPrintf("[boot] entry points resolved (initJni=%p render=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender);

  /* re-assert the guard right before handing control to the engine, so no
   * intervening libnx/jni setup left tpidr in an unexpected state */
  install_bionic_tls(main_tls);

  /* drive the lifecycle the Java UnityPlayer would */
  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST. It runs jni::Initialize(),
   * which caches the JavaVM into libunity's internal JNI manager; without this
   * ScopedJNI/LocalScope inside initJni get a NULL JNIEnv and crash. It also
   * AttachCurrentThread()s and RegisterNatives() for each subsystem (our fake
   * env handles FindClass/RegisterNatives as safe no-ops). */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    debugPrintf("[boot] calling JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime. il2cpp caches the VM in a
   * global it later checks; without it, il2cpp logs "Java VM not initialized"
   * and every managed AndroidJNI / AndroidJavaObject call (the Twitter SDK +
   * the SWIG-wrapped AppUtil module the first scene initializes) fails, hanging
   * scene load.
   *
   * We do NOT call libil2cpp's JNI_OnLoad: its first action is a log via
   * __android_log_print, whose GOT slot in libil2cpp is mis-bound (resolves to
   * a heap address -> Instruction Abort). Its only *essential* effects are two
   * global stores (verified by disassembling this exact 62f2 libil2cpp's
   * see pp_il2cpp.h. */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    /* Papers, Please's libil2cpp JNI_OnLoad (@0x5f2990) does exactly ONE
     * essential thing: it caches the JavaVM* in a global.  Unlike Zookeeper's
     * and PvZ's builds it does NOT also store a JNI handler function pointer --
     * disassembling it shows one store and two calls, nothing more.  So we
     * replicate the single store and no more; inventing a second write here
     * would scribble on an unrelated global.  (We do not call JNI_OnLoad itself:
     * its first action logs through __android_log_print, whose GOT slot in
     * libil2cpp is mis-bound under our loader -> Instruction Abort.) */
    *(void **)(b + PP_IL2CPP_JAVAVM_OFF) = fake_vm;
    debugPrintf("[boot] il2cpp JavaVM global set @il2cpp+0x%x (vm=%p)\n",
                (unsigned)PP_IL2CPP_JAVAVM_OFF, fake_vm);
  }

  /* (Firebase native libs are not loaded; their JNI_OnLoad is neither needed nor
   * safe to call -- see the boot-time note above. The managed SDK's native calls
   * are answered by firebase_stub.c through dlsym_fake.) */

  debugPrintf("[boot] calling initJni...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] initJni returned; nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  debugPrintf("[boot] gfx state created; sendSurfaceChanged...\n");
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface change sent; resuming + focusing player loop\n");

  /* CRITICAL: on Android the Unity player loop only advances Update/coroutines/
   * animation when the app is RESUMED and FOCUSED. The Java UnityPlayer drives
   * this from onResume()/onWindowFocusChanged(). We had been calling only
   * initJni + gfx + render, so the engine stayed paused: it loaded the boot
   * scene and ran Awake/Start once (hence Firebase init), then rendered a frozen
   * frame forever without ticking a single Update or coroutine -- which is why
   * StartInitializer.InitUpdate was never called. Issue the resume + focus
   * transitions the lifecycle normally would before the render loop. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  debugPrintf("[boot] resumed + focus=true; entering render loop\n");

  { extern void android_native_log_metrics(void); android_native_log_metrics(); }

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");   // the thread that drives nativeRender
  /* Watchdog re-enabled (fbstub88) with the snapshot-ordering fix: stacks are
   * now walked while the target thread is PAUSED (the fbstub86 self-crash came
   * from resuming first and walking a live stack). 6s thresholds. Its job now:
   * catch the first-present hang and dump the thread blocked in eglSwapBuffers. */
  diag_watchdog_start();

  int frame = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);   // heartbeat: lets the watchdog see progress (or its absence)
    nx_time_tick();      // advance our managed-Time clock once per frame
    /* fbstub42: the engine clock is now fixed at its true source by the
     * TimeManager::Update entry hook (installed at boot, see nx_install_time_fix):
     * Update is re-driven each frame with newTime = GetTimeSinceStartup(), so all
     * deltaTime variants advance and the PreloadManager can integrate. No per-frame
     * field poking needed here. */
    android_native_update_mode();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame == 0) {
      /* ---------------------------------------------------------------------
       * Frame-0 il2cpp work.
       *
       * The Zookeeper base did a great deal more here -- forcing
       * Application.internetReachability to NotReachable, stubbing
       * FirebaseManager.IsGetMessage, forcing a FirebaseLoading state gate, and
       * installing a native TimeManager::Update re-drive.  Every one of those
       * was a workaround for Zookeeper's own boot coroutine and its Firebase
       * SDK.  Papers, Please has neither: its managed code is Haxe
       * cross-compiled to C# hosted by a thin HostUnity shim, with no Firebase,
       * no boot state machine, and no async scene-load pipeline to unblock.
       * Carrying those hooks across would mean patching il2cpp offsets that
       * point at unrelated game code.  They are REMOVED, not merely disabled.
       *
       * What remains is the part that is about the engine rather than the game.
       * ------------------------------------------------------------------- */

      /* Boehm GC.  The real fix lives in libc_shim.c: pthread_kill_gc() bridges
       * the stop-the-world handshake, because il2cpp suspends threads with
       * POSIX signals that the Switch never delivers, so the ack semaphore is
       * never posted and the first stop-the-world would hang forever.  Its four
       * globals are re-derived for THIS libil2cpp -- see pp_il2cpp.h.
       *
       * On top of the bridge we also ask the runtime to stand the collector
       * down, by symbol rather than by offset.  set_mode(1) clears the
       * incremental-collector flag (Unity 2022 ships incremental GC on, and the
       * incremental collector keeps doing stop-the-world mark steps regardless
       * of GC_dont_gc), and gc_disable() suppresses ordinary collections.
       *
       * Timing matters here and is the reason this sits at frame 0 rather than
       * earlier: the PvZ port tried calling gc_disable() during il2cpp_init and
       * deadlocked on a GC lock held by a helper thread that had not started
       * running yet.  By frame 0 the runtime is fully up.  If you ever see the
       * boot wedge immediately after "[boot] gc_set_mode", delete these two
       * calls -- the bridge alone is sufficient, and letting the GC run behind
       * it is what the Very Little Nightmares port does. */
      typedef void (*fn_set_mode)(int);
      typedef void (*fn_void)(void);
      fn_set_mode il2cpp_gc_set_mode =
          (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
      fn_void il2cpp_gc_disable =
          (fn_void)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
      if (il2cpp_gc_set_mode) {
        il2cpp_gc_set_mode(1);   /* IL2CPP_GC_MODE_DISABLED */
        debugPrintf("[boot] gc_set_mode(DISABLED) -> incremental GC off\n");
      } else {
        debugPrintf("[boot] WARNING: il2cpp_gc_set_mode not found\n");
      }
      if (il2cpp_gc_disable) { il2cpp_gc_disable(); debugPrintf("[boot] gc_disable() -> GC off\n"); }
      else debugPrintf("[boot] WARNING: il2cpp_gc_disable not found; GC stays on behind the bridge\n");
      /* NOTE: the Zookeeper base also poked its incremental-GC flag directly by
       * offset as a belt-and-braces third step.  That offset is specific to its
       * libil2cpp and there is no safe equivalent to guess here, so the API
       * calls above are the whole story.  They do the same job. */

      /* Redirect the managed UnityEngine.Time accessors to our frame clock so
       * engine-managed time advances (fades, WaitForSeconds, per-frame waits).
       *
       * Papers, Please references only THREE Time members -- managed code
       * stripping removed the rest, and they are genuinely absent from the
       * metadata, so the reference ports' 7- and 8-entry tables do not apply:
       *
       *   public static float  deltaTime                    { get; }
       *   public static int    frameCount                   { get; }
       *   public static double realtimeSinceStartupAsDouble { get; }
       *
       * *** The realtime accessor returns a DOUBLE (in d0), not a float. ***
       * The other ports hook realtimeSinceStartup, which returns a float in s0.
       * Reusing their hook here would hand the engine garbage, so this slot
       * gets nx_realtime_since_startup_d() -- see the wrapper above. */
      {
        struct { uint32_t off; void *fn; const char *nm; } th[] = {
          { PP_RVA_get_deltaTime,                    (void *)&nx_delta_time,                 "deltaTime" },
          { PP_RVA_get_frameCount,                   (void *)&nx_frame_count,                "frameCount" },
          { PP_RVA_get_realtimeSinceStartupAsDouble, (void *)&nx_realtime_since_startup_d,   "realtimeSinceStartupAsDouble" },
        };
        for (unsigned i = 0; i < sizeof(th) / sizeof(th[0]); i++) {
          uint32_t stub[4];
          stub[0] = 0x58000050u;   /* ldr x16, #8 */
          stub[1] = 0xd61f0200u;   /* br  x16     */
          memcpy(&stub[2], &th[i].fn, 8);
          so_patch_code((void *)((uintptr_t)il2cpp_mod.load_virtbase + th[i].off),
                        stub, sizeof stub);
          debugPrintf("[boot] hooked Time.get_%s @il2cpp+0x%x\n", th[i].nm, th[i].off);
        }
        so_flush_caches(&il2cpp_mod);
      }
    }
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
    frame++;
  }

  /* Graceful exit. The engine's render/GC/worker threads are still live here, so
   * running Unity's teardown or libnx service deinit (__libnx_exit) lets those
   * threads touch torn-down gfx/audio state and fault -- that's "closing the game
   * crashes the system". svcExitProcess() terminates the whole process (every
   * thread) atomically in the kernel BEFORE anything is torn down, so nothing can
   * race the shutdown. The system reclaims all resources and returns to HOME. */
  svcExitProcess();
  return 0;
}
