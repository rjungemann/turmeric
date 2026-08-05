/* jit_engine.c -- the in-process MIR JIT engine behind `tur jit`.
 *
 * Phase J1 of docs/upcoming/jit-engine-plan.md.  Consumes the same emitted C
 * that `tur build` hands to cc -- after the same in-process post-passes
 * (hoist_tur_include_directives, scan_autolink_markers) -- and executes it
 * with no cc subprocess and no disk artifacts:
 *
 *   c2mir (C11 front end) -> MIR_link (symbols resolved against THIS process
 *   by dlsym(RTLD_DEFAULT)) -> MIR_gen (lazy, serialized -- see the generation
 *   mode note below) -> call main on a sized-stack thread.
 *
 * Everything in here is a straight port of the J0 spike harness
 * (tools/jit-spike/tur-jit-spike.c), which the findings doc validated against
 * the full 1,680-fixture corpus at 98.0%.  Where the two diverge it is because
 * `tur` is the host process: the runtime is already linked in (no
 * whole-archive question), tur_collect_symbols is the real one, and the C text
 * arrives as an in-memory buffer rather than a file.
 *
 * Compiled into `tur` only under -DTUR_JIT=ON (which vendors MIR via
 * cmake/mir.cmake and sets ENABLE_EXPORTS so dlsym can see the runtime).
 * Without it, cmd_jit in main.c reports the missing capability.
 */

#include "jit_engine.h"

#ifdef _WIN32
/* MinGW ships no <dlfcn.h>.  platform_dl.h is the same LoadLibrary shim the
 * rest of the tree already uses for this (src/turi/spice_loader.c, and the
 * Godot shim's AOT image loader), so host-symbol resolution goes through one
 * implementation rather than a second, subtly-different one. */
#  include "platform_dl.h"
#else
#  include <dlfcn.h>
#endif
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"

/* ------------------------------------------------------------------ */
/* I0 SPIKE INSTRUMENTATION (mir-interp-tier-plan phase I0)            */
/* Temporary: measures the tier so I0's go/no-go gate has numbers.     */
/* Remove wholesale if I0 comes back negative and the plan is shelved. */
/* ------------------------------------------------------------------ */
#ifndef _WIN32
/* getrlimit/setrlimit live here; MinGW has neither the header nor the calls.
 * Nothing in this file actually calls them today -- the include rides along
 * with the temporary instrumentation block above -- so guarding it costs no
 * functionality on Windows. If a real rlimit use lands, it needs a Win32
 * equivalent (Job Objects), not just an include. */
#  include <sys/resource.h>
#endif
#include <time.h>

static int g_jit_interp_mode;  /* TUR_JIT_GEN=interp */
static int g_jit_gen_inited;   /* was MIR_gen_init called? */

static double jit_now_ms (void) {
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1.0e6;
}

static double g_jit_t0, g_jit_t_prev;
static int g_jit_timing;   /* TUR_JIT_TIMING=1 */

static void jit_timing_begin (void) {
  const char *t = getenv ("TUR_JIT_TIMING");
  g_jit_timing = (t != NULL && strcmp (t, "1") == 0);
  g_jit_t0 = g_jit_t_prev = jit_now_ms ();
}

/* Phase delta since the previous mark, on stderr so fixture stdout stays
 * byte-comparable under the parity harness. */
static void jit_timing_mark (const char *phase) {
  if (!g_jit_timing) return;
  double now = jit_now_ms ();
  fprintf (stderr, "TUR_JIT_TIMING\t%s\t%s\t%.3f\n",
           g_jit_interp_mode ? "interp" : "gen", phase, now - g_jit_t_prev);
  g_jit_t_prev = now;
}

static void jit_timing_rss (void) {
  if (!g_jit_timing) return;
  struct rusage ru;
  getrusage (RUSAGE_SELF, &ru);
  /* Linux reports KB, macOS bytes; normalize to KB. */
#if defined(__APPLE__)
  double kb = (double) ru.ru_maxrss / 1024.0;
#else
  double kb = (double) ru.ru_maxrss;
#endif
  fprintf (stderr, "TUR_JIT_TIMING\t%s\tmaxrss_kb\t%.0f\n",
           g_jit_interp_mode ? "interp" : "gen", kb);
  fprintf (stderr, "TUR_JIT_TIMING\t%s\ttotal\t%.3f\n",
           g_jit_interp_mode ? "interp" : "gen", jit_now_ms () - g_jit_t0);
}

/* ------------------------------------------------------------------ */
/* input: the emitted C, in memory                                     */
/* ------------------------------------------------------------------ */
static const char *g_src;
static size_t g_src_len, g_src_pos;

static int jit_getc (void *data) {
  (void) data;
  return g_src_pos >= g_src_len ? EOF : (unsigned char) g_src[g_src_pos++];
}

/* Builtin prototypes, prepended ahead of the TU.  c2mir does not know the
 * __builtin_* family; an undeclared __builtin_sqrt is implicitly declared as
 * int-returning and the call reads the integer return register while the shim
 * delivers the value in xmm0 -- floor(sqrt(25.0)) came out as 1 before the
 * spike learned this (findings 11.7).  These reach the emitted C from INLINE C
 * (stdlib/math.tur and friends), never from codegen.
 *
 * Deliberately absent: the spike shim's `#define __thread` and fake
 * non-atomic __atomic_* lowerings.  Those exist to squeeze fixture coverage
 * out of inline-C the emitter does not own, and shipping them would trade a
 * clean compile error for silent corruption under spawn.  Inline C that uses
 * them fails c2mir loudly and takes the step-6 fallback to cc instead. */
static const char JIT_PRELUDE[] =
  /* MIR's Apple/aarch64 prelude spells `#define __arm64__` with NO
   * replacement list (c2mir/aarch64/mirc_aarch64_linux.h:135), where Apple
   * clang defines it as 1.  Every SDK `#if __arm64__` therefore expands to a
   * bare `#if` -- "empty preprocessor expression" -- and the guarded block is
   * lost.  `mach/port.h:100` is the one the corpus hits; it guards
   * `xnu_static_assert_struct_size`, so losing it silently DISABLES the SDK's
   * own struct-size assertions.  Restoring the value re-arms them.
   *
   * That mattered: it exposed that c2mir was ignoring the
   * `#pragma pack(push, 4)` mach/message.h:291 wraps those structs in, laying
   * them out at natural alignment (16 bytes where clang gives 12, on a pack(4)
   * probe).  `#pragma pack` is implemented as of MIR fork commit d7e19e8d, so
   * the assertions now PASS and those programs JIT; they stay armed as a live
   * guard against the layout drifting again.  See
   * docs/archive/jit-c2mir-ignores-pragma-pack.md.
   *
   * This runs before any SDK header: c2mir's stream stack reads its own
   * prelude first, so a redefine here lands after MIR's and ahead of the
   * emitted TU's #includes.  Scoped to Apple/aarch64 -- the only target whose
   * prelude carries the empty spelling. */
  "#if defined(__APPLE__) && defined(__aarch64__)\n"
  "#undef __arm64__\n"
  "#define __arm64__ 1\n"
  /* <libkern/OSByteOrder.h>:80 selects on __LITTLE_ENDIAN__/__BIG_ENDIAN__ and
   * #errors "Unknown endianess" when neither is set.  c2mir advertises neither;
   * arm64 Darwin is unambiguously little-endian. */
  "#define __LITTLE_ENDIAN__ 1\n"
  /* <TargetConditionals.h> auto-detects the compiler and #errors at :398 when
   * it recognizes none.  Its own documented workaround (see the comment above
   * that #error) is to set TARGET_CPU_/TARGET_OS_ on the command line, which is
   * what this does -- rather than advertising __GNUC__, which does suppress the
   * #error but then unlocks GCC-only spellings elsewhere in the SDK that c2mir
   * cannot parse (__header_always_inline in sys/_types/_fd_def.h:59 is the
   * first one that bites).
   *
   * Only the macros clang computes as 1 for arm64 macOS are listed, taken from
   * `clang -dM -E -include TargetConditionals.h`.  The ~30 it computes as 0 are
   * deliberately omitted: an undefined macro already evaluates to 0 in #if, so
   * defining them adds nothing, and a short list is far easier to keep honest.
   * Guarded on __aarch64__ so this cannot mislabel an Intel host. */
  "#define TARGET_CPU_ARM64 1\n"
  "#define TARGET_OS_MAC 1\n"
  "#define TARGET_OS_OSX 1\n"
  "#define TARGET_RT_64_BIT 1\n"
  "#define TARGET_RT_LITTLE_ENDIAN 1\n"
  "#define TARGET_RT_MAC_MACHO 1\n"
  "#endif\n"
  "double __builtin_pow (double, double);\n"
  "double __builtin_sqrt (double);\n"
  "double __builtin_ceil (double);\n"
  "double __builtin_floor (double);\n"
  "double __builtin_fabs (double);\n"
  "void __builtin_trap (void);\n"
  "unsigned long __builtin_strlen (const char *);\n"
  "int __builtin_popcount (unsigned int);\n"
  "void *__builtin_memcpy (void *, const void *, unsigned long);\n"
  "double __builtin_sin (double);\n"
  "double __builtin_cos (double);\n"
  "double __builtin_exp (double);\n"
  "double __builtin_log (double);\n"
  "double __builtin_atan2 (double, double);\n"
  "unsigned short _OSSwapInt16 (unsigned short);\n"
  "unsigned int _OSSwapInt32 (unsigned int);\n"
  "unsigned long long _OSSwapInt64 (unsigned long long);\n";

static double jit_builtin_pow (double x, double y) { return pow (x, y); }
static double jit_builtin_sqrt (double x) { return sqrt (x); }
static double jit_builtin_ceil (double x) { return ceil (x); }
static double jit_builtin_floor (double x) { return floor (x); }
static double jit_builtin_fabs (double x) { return fabs (x); }
static void jit_builtin_trap (void) { abort (); }
static size_t jit_builtin_strlen (const char *s) { return strlen (s); }
static int jit_builtin_popcount (unsigned x) {
  int n = 0;
  while (x) { n += (int) (x & 1u); x >>= 1; }
  return n;
}
static void *jit_builtin_memcpy (void *d, const void *s, size_t n) {
  return memcpy (d, s, n);
}
/* The numeric tower (Rational/Complex, merged from main 2026-07-29) reaches
 * these from stdlib inline C; enumerated by the same grep as the rest. */
static double jit_builtin_sin (double x) { return sin (x); }
static double jit_builtin_cos (double x) { return cos (x); }
static double jit_builtin_exp (double x) { return exp (x); }
static double jit_builtin_log (double x) { return log (x); }
static double jit_builtin_atan2 (double y, double x) { return atan2 (y, x); }

/* ------------------------------------------------------------------ */
/* atexit interception (findings 9.4)                                  */
/* ------------------------------------------------------------------ */
/* Registering the real atexit is worse than failing: the handler is JIT'd
 * code and MIR_gen_finish unmaps it before libc drains its list, so the
 * process dies in freed code at exit.  The JIT owns the list and drains it
 * on the entry thread while the generated code is still mapped. */
#define MAX_JIT_ATEXIT 64
static void (*g_atexit_fns[MAX_JIT_ATEXIT]) (void);
static int g_n_atexit = 0;

static int jit_atexit (void (*fn) (void)) {
  if (g_n_atexit >= MAX_JIT_ATEXIT) return -1;
  g_atexit_fns[g_n_atexit++] = fn;
  return 0;
}

static void jit_atexit_drain (void) {
  while (g_n_atexit > 0) g_atexit_fns[--g_n_atexit] ();
}

/* ------------------------------------------------------------------ */
/* Darwin byte-swap inlines                                            */
/* ------------------------------------------------------------------ */
/* <libkern/_OSByteOrder.h> spells _OSSwapInt{16,32,64} as __DARWIN_OS_INLINE
 * (= static __inline__), and htons/ntohs reach them through the
 * __DARWIN_OSSwapInt16 macro.  c2mir emits no definition for them, so every
 * program touching network byte order -- the whole httpd family plus
 * async-echo-server -- died at MIR_link on `import of undefined item
 * _OSSwapInt16` and took the cc fallback (27 fixtures on macOS).
 *
 * Unlike the __atomic_* family above, these are safe to shim: one fixed
 * signature per name, a pure function of its argument, no memory ordering.
 *
 * The JIT_PRELUDE declarations are NOT optional.  Shimming these by address
 * alone reproduces findings 11.7 exactly: c2mir falls back to an implicit
 * int-returning declaration, the call reads the wrong register, and
 * htons(8080) yields 0xb8f6 instead of 0x901f -- silently corrupting ports
 * and header lengths rather than failing loudly.  Declaring the real
 * prototype ahead of the TU is what makes the shim correct. */
static uint16_t jit_osswap16 (uint16_t x) {
  return (uint16_t) ((x << 8) | (x >> 8));
}
static uint32_t jit_osswap32 (uint32_t x) {
  return ((x << 24) | ((x << 8) & 0x00ff0000u)
          | ((x >> 8) & 0x0000ff00u) | (x >> 24));
}
static uint64_t jit_osswap64 (uint64_t x) {
  return ((uint64_t) jit_osswap32 ((uint32_t) x) << 32)
         | jit_osswap32 ((uint32_t) (x >> 32));
}

static const struct { const char *name; void *addr; } JIT_SHIMS[] = {
  {"__builtin_pow", (void *) jit_builtin_pow},
  {"__builtin_sqrt", (void *) jit_builtin_sqrt},
  {"__builtin_ceil", (void *) jit_builtin_ceil},
  {"__builtin_floor", (void *) jit_builtin_floor},
  {"__builtin_fabs", (void *) jit_builtin_fabs},
  {"__builtin_trap", (void *) jit_builtin_trap},
  {"__builtin_strlen", (void *) jit_builtin_strlen},
  {"__builtin_popcount", (void *) jit_builtin_popcount},
  {"__builtin_memcpy", (void *) jit_builtin_memcpy},
  {"__builtin_sin", (void *) jit_builtin_sin},
  {"__builtin_cos", (void *) jit_builtin_cos},
  {"__builtin_exp", (void *) jit_builtin_exp},
  {"__builtin_log", (void *) jit_builtin_log},
  {"__builtin_atan2", (void *) jit_builtin_atan2},
  {"atexit", (void *) jit_atexit},
  {"_OSSwapInt16", (void *) jit_osswap16},
  {"_OSSwapInt32", (void *) jit_osswap32},
  {"_OSSwapInt64", (void *) jit_osswap64},
};

/* ------------------------------------------------------------------ */
/* serialized lazy generation (findings 8.1 / 34)                      */
/* ------------------------------------------------------------------ */
/* MIR's own `MIR_set_lazy_gen_interface` installs a first-call wrapper that
 * runs MIR-gen on the context's single shared gen_ctx.  Two threads entering
 * two not-yet-generated functions therefore corrupt that state: measured as
 * three different assertions across five runs of one fixture
 * (`destroy_func_cfg`, `mark_unreachable_bbs`, `undeclared reg N of func`).
 * MIR-gen is simply not thread-safe, and Turmeric has `spawn`, fibers, and a
 * work-stealing scheduler.
 *
 * The plan's instruction is "serialize generation behind a lock or generate
 * eagerly for any program that can spawn".  Serializing needs no fork patch:
 * every piece MIR's lazy path uses is public (`_MIR_get_wrapper`,
 * `_MIR_redirect_thunk`, `MIR_gen`, and `machine_code` on the func), so the
 * interface is reimplemented here with a mutex around it.  `MIR_gen` is
 * literally `generate_func_code (ctx, item, TRUE)` -- the same call MIR's hook
 * makes -- so this is MIR's lazy semantics plus mutual exclusion, not a
 * different code path.
 *
 * The DOUBLE-CHECK is the load-bearing half.  A plain lock still lets two
 * threads that both got past the stub generate the same function twice, which
 * is what trips `_MIR_duplicate_func_insns`; re-reading `machine_code` under
 * the lock makes the second arrival a lookup.
 *
 * Contention is bounded and self-extinguishing: a function is generated once,
 * after which its thunk goes straight to the code and never reaches this hook
 * again. The lock is process-wide rather than per-context because gen state is
 * per-context but the cost of over-serializing across images is a few
 * microseconds on a path that runs once per function. */
static pthread_mutex_t g_gen_lock = PTHREAD_MUTEX_INITIALIZER;

static void *jit_lazy_gen_locked (MIR_context_t ctx, MIR_item_t func_item) {
  pthread_mutex_lock (&g_gen_lock);
  void *code = func_item->u.func->machine_code;
  if (code == NULL) code = MIR_gen (ctx, func_item);
  pthread_mutex_unlock (&g_gen_lock);
  return code;
}

/* Drop-in for MIR_set_lazy_gen_interface, routing first-call generation
 * through the lock above.  Mirrors MIR's version exactly otherwise. */
static void jit_set_lazy_gen_interface (MIR_context_t ctx, MIR_item_t func_item) {
  void *addr;

  if (func_item == NULL) return;
  addr = _MIR_get_wrapper (ctx, func_item, jit_lazy_gen_locked);
  _MIR_redirect_thunk (ctx, func_item->addr, addr);
}

/* MIR's default error handler prints and EXITS the process -- from inside
 * MIR_link, an unresolved import (e.g. a GCC atomic builtin in user inline-C
 * that c2mir compiled as an implicit call) would kill `tur` before cmd_jit's
 * step-6 fallback could run.  Found by the first full-corpus sweep of the
 * real subcommand: 13 fixtures whose stdlib inline-C uses __atomic_* died
 * with empty output instead of falling back to cc (findings 18.1).  Unwind
 * to tur_jit_execute instead; the half-initialized context is deliberately
 * LEAKED (tearing it down from an undefined intermediate state is how a
 * fallback becomes a crash), which is acceptable in a one-shot CLI. */
static jmp_buf g_jit_err_jb;
static volatile int g_jit_err_active = 0;

static void MIR_NO_RETURN jit_mir_error (MIR_error_type_t type, const char *fmt, ...) {
  va_list ap;
  va_start (ap, fmt);
  fprintf (stderr, "tur: jit: ");
  vfprintf (stderr, fmt, ap);
  fputc ('\n', stderr);
  va_end (ap);
  (void) type;
  if (g_jit_err_active) longjmp (g_jit_err_jb, 1);
  exit (1);
}

static void *jit_import_resolver (const char *name) {
  for (size_t i = 0; i < sizeof JIT_SHIMS / sizeof JIT_SHIMS[0]; i++)
    if (strcmp (name, JIT_SHIMS[i].name) == 0) return JIT_SHIMS[i].addr;
  return dlsym (RTLD_DEFAULT, name);
}

/* ------------------------------------------------------------------ */
/* weak-symbol handshake (findings 11.7 / archived reactor report)     */
/* ------------------------------------------------------------------ */
/* libturi declares `__attribute__((weak)) int tur_closure_headers_enabled`
 * and the emitted program overrides it with a strong definition; the linker
 * resolves that under cc, but host code in this process was linked long ago
 * and reads its own weak copy.  Copy the program's value onto the host's
 * global after the module is loaded. */
extern int tur_closure_headers_enabled;

static void jit_sync_config_globals (MIR_context_t ctx) {
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_data_item && it->u.data->name != NULL
          && strcmp (it->u.data->name, "tur_closure_headers_enabled") == 0
          && it->addr != NULL)
        tur_closure_headers_enabled = *(int *) it->addr;
}

/* ------------------------------------------------------------------ */
/* autolink: dlopen -l<name> entries so dlsym can reach them           */
/* ------------------------------------------------------------------ */
/* The cc path hands `__tur_autolink__` flags to the linker; the JIT's
 * equivalent is loading each -l<name> into the process with RTLD_GLOBAL.
 * -lturi and the libs `tur` already links (m, pthread, dl) are skipped --
 * their symbols are resolvable in this process by construction.  Any entry
 * that fails to load fails the whole JIT attempt, which the caller turns
 * into the step-6 fallback to cc. */
static int jit_load_autolink (const char *flags) {
  if (!flags) return 0;
  const char *p = flags;
  while (*p) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if (e - p > 2 && p[0] == '-' && p[1] == 'l') {
      char name[256];
      size_t n = (size_t) (e - p - 2);
      if (n < sizeof name - 16) {
        memcpy (name, p + 2, n);
        name[n] = '\0';
        if (strcmp (name, "turi") != 0 && strcmp (name, "m") != 0
            && strcmp (name, "pthread") != 0 && strcmp (name, "dl") != 0) {
          char soname[300];
          snprintf (soname, sizeof soname, "lib%s.so", name);
          if (dlopen (soname, RTLD_NOW | RTLD_GLOBAL) == NULL) {
            fprintf (stderr, "tur: jit: cannot load %s: %s\n", soname, dlerror ());
            return -1;
          }
        }
      }
    }
    p = e;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* entry thread (findings 15.3)                                        */
/* ------------------------------------------------------------------ */
/* MIR-gen frames for direct-path recursion are ~1.4x gcc's, so a recursion
 * that fits the default 8 MB under `tur build` can blow the stack here.  A
 * sized entry stack is the sanctioned stopgap ("any size temporarily is
 * fine", owner 2026-07-29); the long-run fix is MIR frame-size work or the
 * runtime's existing stackless machinery, never a bigger constant.  The
 * atexit drain runs on this thread because handlers may read host TLS
 * (tur_tls.c) the program wrote here. */
struct jit_entry_box {
  int (*fn) (int, char **, char **);
  int argc;
  char **argv;
  int rc;
};

static void *jit_run_entry (void *p) {
  struct jit_entry_box *box = (struct jit_entry_box *) p;
  char *fake_envp[] = {NULL};
  box->rc = box->fn (box->argc, box->argv, fake_envp);
  jit_atexit_drain ();
  fflush (stdout);
  return NULL;
}

/* ------------------------------------------------------------------ */
/* shared front half: compile + load + link one emitted TU              */
/* ------------------------------------------------------------------ */
/* Everything up to "the program is linked and generated": prelude concat,
 * c2mir, module load, eager MIR_link with the process resolver, and the
 * config-global sync.  Shared verbatim by the one-shot tur_jit_execute and
 * the J2 image path; on success the caller owns tearing the context down
 * (MIR_gen_finish -> c2mir_finish -> MIR_finish, in that order).
 *
 * Returns TUR_JIT_OK with *out_ctx set, or a TUR_JIT_ERR_* class with all
 * engine state already torn down (except the deliberately-leaked context
 * on a longjmp'd link error -- see jit_mir_error). */
static int jit_compile_and_link (const char *csrc, size_t csrc_len,
                                 const char *autolink,
                                 const char **include_dirs, int n_include_dirs,
                                 MIR_context_t *out_ctx) {
  *out_ctx = NULL;
  if (jit_load_autolink (autolink) != 0) return TUR_JIT_ERR_LINK;

  /* Prepend the builtin prototypes.  A memory concat beats teaching c2mir
   * about a second stream. */
  size_t full_len = sizeof JIT_PRELUDE - 1 + csrc_len;
  char *full = (char *) malloc (full_len + 1);
  if (!full) return TUR_JIT_ERR_COMPILE;
  memcpy (full, JIT_PRELUDE, sizeof JIT_PRELUDE - 1);
  memcpy (full + sizeof JIT_PRELUDE - 1, csrc, csrc_len);
  full[full_len] = '\0';

  g_src = full;
  g_src_len = full_len;
  g_src_pos = 0;

  jit_timing_begin ();
  MIR_context_t ctx = MIR_init ();
  MIR_set_error_func (ctx, jit_mir_error);
  g_jit_err_active = 1;
  if (setjmp (g_jit_err_jb) != 0) {
    g_jit_err_active = 0;
    free (full);
    return TUR_JIT_ERR_LINK;
  }
  c2mir_init (ctx);

  struct c2mir_options ops;
  memset (&ops, 0, sizeof ops);
  ops.message_file = stderr;

  ops.include_dirs_num = (size_t) (n_include_dirs > 0 ? n_include_dirs : 0);
  ops.include_dirs = include_dirs;

  int ok = c2mir_compile (ctx, &ops, jit_getc, NULL, "<tur-jit>", NULL);
  if (!ok) {
    g_jit_err_active = 0;
    c2mir_finish (ctx);
    MIR_finish (ctx);
    free (full);
    return TUR_JIT_ERR_COMPILE;
  }

  /* I0 SPIKE (mir-interp-tier-plan section 3, phase I0): TUR_JIT_GEN=interp
   * selects MIR's bytecode interpreter instead of the generator, so the tier
   * can be measured on the production path rather than a scratch driver.
   * MIR_gen_init is SKIPPED in that mode -- the plan's benefit-3 claim (an
   * interp-only deployment need not link mir-gen-<arch>.c) is only testable
   * if the generator is never initialized.  g_jit_gen_inited keeps the
   * matching MIR_gen_finish calls honest. */
  g_jit_interp_mode = 0;
  {
    const char *g = getenv ("TUR_JIT_GEN");
    if (g != NULL && strcmp (g, "interp") == 0) g_jit_interp_mode = 1;
  }
  jit_timing_mark ("c2mir");
  if (!g_jit_interp_mode) {
    MIR_gen_init (ctx);
    MIR_gen_set_optimize_level (ctx, 2);
    g_jit_gen_inited = 1;
  } else {
    g_jit_gen_inited = 0;
  }
  jit_timing_mark ("gen_init");

  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    MIR_load_module (ctx, m);

  /* Generation mode.  LAZY is the default -- the plan's original
   * recommendation, restored now that both defects that withdrew it are
   * addressed (findings 34):
   *
   *   8.1  lazy is not re-entrant -> jit_set_lazy_gen_interface above
   *        serializes generation and double-checks machine_code, so two
   *        threads racing a first call cannot corrupt gen state or generate
   *        the same function twice.
   *   8.4.1  lazy miscompiled pthread entries single-threaded -> does not
   *        reproduce at the current fork pin; re-measured directly on both
   *        fixtures the finding named.  Its `undeclared reg N` symptom now
   *        appears only as one of the race's several faces, so 8.4.1 was
   *        most likely always 8.1 seen without the concurrency in view.
   *
   * Full corpus is identical either way (2394/0/47), and lazy saves 23-36%
   * of end-to-end wall time on a Release build -- enough to move `tur jit`
   * from parity with the cc round trip to clearly ahead of it.
   *
   *   TUR_JIT_GEN=eager   MIR_set_gen_interface
   *   TUR_JIT_GEN=lazy    jit_set_lazy_gen_interface (default)
   *
   * The escape hatch is worth keeping and worth understanding: eager doubles
   * as a VERIFICATION pass, generating every function before any of the
   * program runs, so a generation failure surfaces at compile time where
   * cmd_jit's step-6 cc fallback can still catch it.  Under lazy the same
   * failure surfaces at first call -- after output may already have been
   * written, and past the point where g_jit_err_active can unwind to the
   * fallback.  The set of functions that fail is the same in both modes (same
   * generator, same input; lazy merely skips ones never called), so this is a
   * question of WHEN, not WHETHER.  No fixture reaches it.  If one ever does,
   * TUR_JIT_GEN=eager is the diagnostic.
   *
   * Deliberately an env knob, not an --enable= experiment: it selects between
   * two implementations of one already-shipping behavior. */
  void (*gen_iface) (MIR_context_t, MIR_item_t) = jit_set_lazy_gen_interface;
  {
    const char *g = getenv ("TUR_JIT_GEN");
    if (g != NULL && strcmp (g, "eager") == 0) gen_iface = MIR_set_gen_interface;
    /* I0 spike: see the mode selection above. */
    if (g_jit_interp_mode) gen_iface = MIR_set_interp_interface;
  }
  jit_timing_mark ("load");
  MIR_link (ctx, gen_iface, jit_import_resolver);
  jit_timing_mark ("link");
  jit_sync_config_globals (ctx);
  g_jit_err_active = 0;   /* past the last MIR call that can raise */

  free (full);             /* c2mir consumed the stream; the text is done */
  *out_ctx = ctx;
  return TUR_JIT_OK;
}

/* Find the generated function named `name` across the context's modules.
 * MIR item lookup sees static functions too (unlike dlsym), which is what
 * lets the single-TU spice emission keep its `static` linkage. */
static MIR_item_t jit_find_func (MIR_context_t ctx, const char *name) {
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_func_item && strcmp (it->u.func->name, name) == 0)
        return it;
  return NULL;
}

/* ------------------------------------------------------------------ */
/* the engine                                                          */
/* ------------------------------------------------------------------ */
int tur_jit_execute (const char *csrc, size_t csrc_len, const char *autolink,
                     const char **include_dirs, int n_include_dirs,
                     int prog_argc, char **prog_argv, int *prog_rc) {
  g_n_atexit = 0;
  MIR_context_t ctx;
  int frc = jit_compile_and_link (csrc, csrc_len, autolink,
                                  include_dirs, n_include_dirs, &ctx);
  if (frc != TUR_JIT_OK) return frc;

  MIR_item_t main_item = jit_find_func (ctx, "main");
  if (main_item == NULL) {
    if (g_jit_gen_inited) MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
    fprintf (stderr, "tur: jit: no main in generated module\n");
    return TUR_JIT_ERR_COMPILE;
  }

  typedef int (*main_fn) (int, char **, char **);
  struct jit_entry_box box = { (main_fn) main_item->addr, prog_argc, prog_argv, 0 };

  size_t stack_mb = 64;
  const char *senv = getenv ("TUR_JIT_STACK_MB");
  if (senv && atoi (senv) > 0) stack_mb = (size_t) atoi (senv);
  pthread_attr_t attr;
  pthread_t entry_thread;
  pthread_attr_init (&attr);
  pthread_attr_setstacksize (&attr, stack_mb * 1024 * 1024);
  if (pthread_create (&entry_thread, &attr, jit_run_entry, &box) != 0) {
    pthread_attr_destroy (&attr);
    if (g_jit_gen_inited) MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
    fprintf (stderr, "tur: jit: entry thread create failed\n");
    return TUR_JIT_ERR_RUN;
  }
  pthread_join (entry_thread, NULL);
  pthread_attr_destroy (&attr);
  jit_timing_mark ("run");
  jit_timing_rss ();

  if (g_jit_gen_inited) MIR_gen_finish (ctx);
  c2mir_finish (ctx);
  MIR_finish (ctx);

  if (prog_rc) *prog_rc = box.rc;
  return TUR_JIT_OK;
}

/* ------------------------------------------------------------------ */
/* J2: persistent image mode (plan section 3.3)                        */
/* ------------------------------------------------------------------ */
struct TurJitImage {
  MIR_context_t ctx;
};

int tur_jit_compile_image (const char *csrc, size_t csrc_len,
                           const char *autolink,
                           const char **include_dirs, int n_include_dirs,
                           TurJitImage **out) {
  *out = NULL;
  /* atexit interception note: the list is engine-global.  Image-registered
   * handlers (module defers) accumulate and are drained when the image is
   * freed, while the generated code is still mapped -- approximate for
   * overlapping images (a reload window), exact for the common one-image
   * session.  A per-image list is J3 work if it ever matters. */
  MIR_context_t ctx;
  int frc = jit_compile_and_link (csrc, csrc_len, autolink,
                                  include_dirs, n_include_dirs, &ctx);
  if (frc != TUR_JIT_OK) return frc;

  /* S1b, applied to the no-main case: c2mir discards the constructor
   * attribute, so the wrapper never fires and module state (dynvar keys,
   * module defs, interned symbols, direct->CPS registry) stays
   * uninitialized.  Call the explicit initializer by name -- it is static
   * in the TU, which MIR item lookup sees fine, and idempotent by
   * construction. */
  MIR_item_t init = jit_find_func (ctx, "__tur_static_init");
  if (init != NULL && init->addr != NULL) ((void (*) (void)) init->addr) ();

  TurJitImage *img = (TurJitImage *) malloc (sizeof *img);
  if (!img) {
    if (g_jit_gen_inited) MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
    return TUR_JIT_ERR_RUN;
  }
  img->ctx = ctx;
  *out = img;
  return TUR_JIT_OK;
}

void *tur_jit_image_sym (TurJitImage *img, const char *name) {
  if (!img) return NULL;
  MIR_item_t it = jit_find_func (img->ctx, name);
  return it ? it->addr : NULL;
}

void tur_jit_image_free (TurJitImage *img) {
  if (!img) return;
  /* Drain pending image atexit handlers (module defers) while the
   * generated code is still mapped; the callers' contract is that no
   * image function pointer is used after this call. */
  jit_atexit_drain ();
  MIR_gen_finish (img->ctx);
  c2mir_finish (img->ctx);
  MIR_finish (img->ctx);
  free (img);
}
