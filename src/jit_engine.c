/* jit_engine.c -- the in-process MIR JIT engine behind `tur jit`.
 *
 * Phase J1 of docs/upcoming/jit-engine-plan.md.  Consumes the same emitted C
 * that `tur build` hands to cc -- after the same in-process post-passes
 * (hoist_tur_include_directives, scan_autolink_markers) -- and executes it
 * with no cc subprocess and no disk artifacts:
 *
 *   c2mir (C11 front end) -> MIR_link (symbols resolved against THIS process
 *   by dlsym(RTLD_DEFAULT)) -> MIR_gen (eager; recommendation 5 withdrew lazy
 *   until it is re-entrant) -> call main on a sized-stack thread.
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

#include <dlfcn.h>
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
  "double __builtin_pow (double, double);\n"
  "double __builtin_sqrt (double);\n"
  "double __builtin_ceil (double);\n"
  "double __builtin_floor (double);\n"
  "double __builtin_fabs (double);\n"
  "void __builtin_trap (void);\n"
  "unsigned long __builtin_strlen (const char *);\n"
  "int __builtin_popcount (unsigned int);\n"
  "void *__builtin_memcpy (void *, const void *, unsigned long);\n";

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
  {"atexit", (void *) jit_atexit},
};

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
/* the engine                                                          */
/* ------------------------------------------------------------------ */
int tur_jit_execute (const char *csrc, size_t csrc_len, const char *autolink,
                     const char **include_dirs, int n_include_dirs,
                     int prog_argc, char **prog_argv, int *prog_rc) {
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
  g_n_atexit = 0;

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
    c2mir_finish (ctx);
    MIR_finish (ctx);
    free (full);
    return TUR_JIT_ERR_COMPILE;
  }

  MIR_gen_init (ctx);
  MIR_gen_set_optimize_level (ctx, 2);

  MIR_item_t main_item = NULL;
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m)) {
    MIR_load_module (ctx, m);
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_func_item && strcmp (it->u.func->name, "main") == 0)
        main_item = it;
  }
  if (main_item == NULL) {
    MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
    free (full);
    fprintf (stderr, "tur: jit: no main in generated module\n");
    return TUR_JIT_ERR_COMPILE;
  }

  /* Eager generation: lazy is not re-entrant at this pin (findings 8.1) and
   * miscompiled pthread entries even single-threaded (8.4.1). */
  MIR_link (ctx, MIR_set_gen_interface, jit_import_resolver);
  jit_sync_config_globals (ctx);
  g_jit_err_active = 0;   /* past the last MIR call that can raise */

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
    MIR_gen_finish (ctx);
    c2mir_finish (ctx);
    MIR_finish (ctx);
    free (full);
    fprintf (stderr, "tur: jit: entry thread create failed\n");
    return TUR_JIT_ERR_RUN;
  }
  pthread_join (entry_thread, NULL);
  pthread_attr_destroy (&attr);

  MIR_gen_finish (ctx);
  c2mir_finish (ctx);
  MIR_finish (ctx);
  free (full);

  if (prog_rc) *prog_rc = box.rc;
  return TUR_JIT_OK;
}
