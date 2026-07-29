/* J0 JIT spike harness -- reconstructed for the arm64 macOS gate.
 *
 * Reads one C file, compiles it in process with c2mir, links it against the
 * Turmeric runtime already present in THIS process (resolved by address via
 * dlsym(RTLD_DEFAULT), per plan section 3.2 step 4), generates native code
 * with MIR-gen, and calls main().  No cc subprocess, no disk artifacts.
 *
 * Usage: tur-jit-spike [-I dir]... [-O n] [--repeat n] [--eager]
 *                      [--shim subset-shim.h] file.c
 *
 * PROVENANCE: the original J0 harness was lost -- .gitignore's blanket `*.c`
 * rule carried no `tools/` negation, so `git add` silently skipped it and the
 * spike commit shipped a CMakeLists referencing a file that was never tracked.
 * This is a reconstruction written to close the arm64 macOS gate.  It does not
 * claim to reproduce whatever the original did about the c2mir subset gaps;
 * see subset-shim.h and the macOS section of jit-engine-j0-findings.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h> /* sized-stack entry thread */
#include <time.h>

#include "mir.h"
#include "mir-gen.h"
#include "c2mir.h"

static const char *src;      /* whole file in memory */
static size_t src_len, src_pos;

static int getc_func (void *data) {
  (void) data;
  return src_pos >= src_len ? EOF : (unsigned char) src[src_pos++];
}

/* GCC builtins that c2mir does not implement.  It emits them as ordinary
   external calls, so they arrive here as plain names and would otherwise fail
   to resolve.  Every one of these reaches the emitted C from INLINE C, not from
   codegen -- stdlib/math.tur calls __builtin_pow, and three fixtures use
   __builtin_strlen / popcount / memcpy directly.  Mapping them to their libc
   equivalents is what a real `tur jit` would do too (findings 8.4.3).

   The set is exactly what a grep for __builtin_ over stdlib and the fixture
   inputs reports, so it is complete for this corpus rather than accumulated one
   sweep failure at a time.  A missing entry still shows up as a clean
   "unresolved import" at link, which is a good signal: add it. */
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
/* The shim lowers atomics to plain memory ops; these two it does not cover,
   and they are reached by exactly one fixture each at full corpus. */
static long jit_atomic_exchange_n (volatile long *p, long v) {
  long o = *p; *p = v; return o;
}
static void jit_atomic_thread_fence (int order) { (void) order; }

static const struct { const char *name; void *addr; } BUILTIN_SHIMS[] = {
  {"__builtin_pow", (void *) jit_builtin_pow},
  {"__builtin_sqrt", (void *) jit_builtin_sqrt},
  {"__builtin_ceil", (void *) jit_builtin_ceil},
  {"__builtin_floor", (void *) jit_builtin_floor},
  {"__builtin_fabs", (void *) jit_builtin_fabs},
  {"__builtin_trap", (void *) jit_builtin_trap},
  {"__builtin_strlen", (void *) jit_builtin_strlen},
  {"__builtin_popcount", (void *) jit_builtin_popcount},
  {"__builtin_memcpy", (void *) jit_builtin_memcpy},
  {"__atomic_exchange_n", (void *) jit_atomic_exchange_n},
  {"__atomic_thread_fence", (void *) jit_atomic_thread_fence},
};

/* libturi is not self-contained: src/lsp/lsp.c calls tur_collect_symbols, whose
   only definition lives in src/main.c (the `tur` executable), so a
   --whole-archive link of the archive pulls in the reference without the
   definition.  The spike never drives the LSP, so a stub that reports "no
   symbols" is sufficient and keeps the archive link honest -- the alternative
   is going back to hand-picking TUs, which is the failure mode 11.5 is about.

   Worth noting as its own small finding: `tur build` never hits this because it
   links libturi normally, so only the members it actually needs are extracted.
   Anything that wants the WHOLE runtime present -- which a JIT does, since it
   resolves by name at runtime rather than by reference at link time -- meets
   this edge. */
int tur_collect_symbols (const char *path, void *out, int cap, int *count);
int tur_collect_symbols (const char *path, void *out, int cap, int *count) {
  (void) path; (void) out; (void) cap;
  if (count != NULL) *count = 0;
  return 0;
}

/* ------------------------------------------------------------------ */
/* atexit interception                                                 */
/* ------------------------------------------------------------------ */
/* `atexit` cannot be resolved by dlsym on glibc -- it lives in
   libc_nonshared.a, statically linked into each executable and never exported
   -- so `module-defer-*` programs failed to link at all.  But simply handing
   over the real atexit is WRONG and was measured to be so on macOS, where the
   symbol IS resolvable: the registered handler is JIT'd code, and the MIR
   context is torn down before libc drains its list, so the process dies in
   freed code at exit (findings 9.4).
   
   So the JIT owns the list.  Handlers are recorded here and drained while the
   generated code is still mapped.  `tur jit` needs the same shape. */
#define MAX_JIT_ATEXIT 64
static void (*jit_atexit_fns[MAX_JIT_ATEXIT]) (void);
static int n_jit_atexit = 0;

static int jit_atexit (void (*fn) (void)) {
  if (n_jit_atexit >= MAX_JIT_ATEXIT) return -1;
  jit_atexit_fns[n_jit_atexit++] = fn;
  return 0;
}

/* LIFO, matching C's atexit ordering. */
static void jit_atexit_drain (void) {
  while (n_jit_atexit > 0) jit_atexit_fns[--n_jit_atexit] ();
}

/* Entry thunk for the sized-stack thread the JIT'd main runs on -- see the
   comment at the pthread_create site.  The atexit drain lives here, on the
   entry thread, because handlers may read host-side TLS (tur_tls.c) written
   by the program; the cc path likewise runs them on the thread that called
   main.  Still strictly before MIR_gen_finish unmaps the handler code. */
struct jit_entry_box {
  int (*fn) (int, char **, char **);
  int rc;
};

static void *jit_run_entry (void *p) {
  struct jit_entry_box *box = (struct jit_entry_box *) p;
  char *fake_argv[] = {(char *) "jit", NULL};
  char *fake_envp[] = {NULL};
  box->rc = box->fn (1, fake_argv, fake_envp);
  jit_atexit_drain ();
  fflush (stdout);
  return NULL;
}

/* Link-time weak-symbol handshakes do not cross the JIT boundary.  libturi
   declares `__attribute__((weak)) int tur_closure_headers_enabled = 0` and the
   emitted program overrides it with a strong `= 1` when its closure boxes carry
   drop-glue headers; under `cc` the linker resolves that, but host code in this
   process was linked long ago and reads its own weak copy -- so the runtime
   plain-free'd an interior pointer and every reactor fixture died at teardown
   (docs/reported/jit-reactor-fixtures-abort-under-mir.md, root cause).  After
   the module is loaded, copy the program's value onto the host's global.

   The six weak no-op tur_scheduler_*_st FUNCTIONS this note used to flag as
   "the same hazard, needs J1's runtime-call redesign" turned out to live in a
   module with ZERO callers -- scheduler_common.c's every export was
   unreachable from src/, stdlib/, and all 1,928 emitted TUs (findings 17).
   The module is deleted; the hazard was in dead code all along. */
extern int tur_closure_headers_enabled;   /* libturi's weak definition */

static void sync_config_globals (MIR_context_t ctx) {
  for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
       m = DLIST_NEXT (MIR_module_t, m))
    for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT (MIR_item_t, it))
      if (it->item_type == MIR_data_item && it->u.data->name != NULL
          && strcmp (it->u.data->name, "tur_closure_headers_enabled") == 0
          && it->addr != NULL)
        tur_closure_headers_enabled = *(int *) it->addr;
}

/* c2mir emits calls to runtime functions by name; MIR asks us for an address.
   The runtime is compiled INTO this executable, so dlsym(RTLD_DEFAULT) finds
   it -- c2mir never parses a line of hamt.c. */
/* S2 proof (findings 19): when TUR_JIT_PRELIB names a host-resident runtime
   library, resolve imports against IT before the process-global search.  The
   executable exports its own copies of some runtime symbols (cps_rt.c, stm.c
   -- diverged vintages of what the preamble carries), and dlsym(RTLD_DEFAULT)
   searches the executable first, so without priority the program half binds a
   MIX of .so-runtime and host-runtime machinery: CPS took SIGSEGV, STM lost
   every increment.  Production S2 must make the runtime library THE runtime
   (replacing the host duplicates); this env hook is the proof-scale stand-in. */
static void *g_prelib_handle = NULL;

static void *import_resolver (const char *name) {
  if (g_prelib_handle != NULL) {
    void *a = dlsym (g_prelib_handle, name);
    if (a != NULL) return a;
  }
  /* Intercepts first: these must win over any host symbol of the same name. */
  if (strcmp (name, "atexit") == 0) return (void *) jit_atexit;
  for (size_t i = 0; i < sizeof BUILTIN_SHIMS / sizeof BUILTIN_SHIMS[0]; i++)
    if (strcmp (name, BUILTIN_SHIMS[i].name) == 0) return BUILTIN_SHIMS[i].addr;

  void *addr = dlsym (RTLD_DEFAULT, name);
  if (addr == NULL) fprintf (stderr, "jit-spike: unresolved import: %s\n", name);
  return addr;
}

static char *slurp (const char *path, size_t *len_out) {
  FILE *f = fopen (path, "rb");
  if (f == NULL) { perror (path); return NULL; }
  fseek (f, 0, SEEK_END);
  long n = ftell (f);
  fseek (f, 0, SEEK_SET);
  char *buf = malloc ((size_t) n + 1);
  if (buf == NULL || fread (buf, 1, (size_t) n, f) != (size_t) n) {
    perror (path);
    fclose (f);
    free (buf);
    return NULL;
  }
  buf[n] = 0;
  fclose (f);
  *len_out = (size_t) n;
  return buf;
}

static double now_ms (void) {
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main (int argc, char **argv) {
  const char *include_dirs[64];
  size_t include_dirs_num = 0;
  int opt_level = 2, repeat = 1, eager_p = 0, quiet_p = 0;
  const char *file_name = NULL, *shim_name = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp (argv[i], "-I") == 0 && i + 1 < argc) {
      include_dirs[include_dirs_num++] = argv[++i];
    } else if (strcmp (argv[i], "-O") == 0 && i + 1 < argc) {
      opt_level = atoi (argv[++i]);
    } else if (strcmp (argv[i], "--repeat") == 0 && i + 1 < argc) {
      repeat = atoi (argv[++i]);
    } else if (strcmp (argv[i], "--shim") == 0 && i + 1 < argc) {
      shim_name = argv[++i];
    } else if (strcmp (argv[i], "--eager") == 0) {
      eager_p = 1;
    } else if (strcmp (argv[i], "--quiet") == 0) {
      quiet_p = 1;
    } else if (argv[i][0] == '-' && argv[i][1] != 0) {
      /* Never silently treat an unrecognized flag as the input file: that turns
         a typo into a confusing "no main" or a wrong-file compile. */
      fprintf (stderr, "jit-spike: unknown option: %s\n", argv[i]);
      return 2;
    } else {
      file_name = argv[i];
    }
  }
  if (file_name == NULL) { fprintf (stderr, "usage: tur-jit-spike file.c\n"); return 2; }

  const char *prelib = getenv ("TUR_JIT_PRELIB");
  if (prelib != NULL && *prelib != '\0') {
    g_prelib_handle = dlopen (prelib, RTLD_NOW | RTLD_GLOBAL);
    if (g_prelib_handle == NULL) {
      fprintf (stderr, "jit-spike: TUR_JIT_PRELIB: %s\n", dlerror ());
      return 2;
    }
  }

  size_t body_len = 0, shim_len = 0;
  char *body = slurp (file_name, &body_len);
  if (body == NULL) return 2;
  char *shim = NULL;
  if (shim_name != NULL && (shim = slurp (shim_name, &shim_len)) == NULL) return 2;

  /* The shim is prepended rather than passed as -D macro_commands so that it
     stays a readable, reviewable file: the c2mir subset gaps it papers over are
     a finding, not an implementation detail to bury in flags.  The trailing
     `#line 1` re-syncs diagnostics with the real file, so reported line numbers
     still index the input rather than the shim-shifted text. */
  const char *resync = "#line 1\n";
  size_t resync_len = shim != NULL ? strlen (resync) : 0;
  char *buf = malloc (shim_len + resync_len + body_len + 1);
  if (shim != NULL) {
    memcpy (buf, shim, shim_len);
    memcpy (buf + shim_len, resync, resync_len);
  }
  memcpy (buf + shim_len + resync_len, body, body_len);
  src_len = shim_len + resync_len + body_len;
  buf[src_len] = 0;
  src = buf;

  double best_c2mir = 1e30, best_gen = 1e30;
  int rc = 0;

  for (int iter = 0; iter < repeat; iter++) {
    src_pos = 0;

    MIR_context_t ctx = MIR_init ();
    c2mir_init (ctx);

    struct c2mir_options options;
    memset (&options, 0, sizeof (options));
    options.message_file = stderr;
    options.include_dirs = include_dirs;
    options.include_dirs_num = include_dirs_num;
    options.module_num = (size_t) iter;

    double t0 = now_ms ();
    if (!c2mir_compile (ctx, &options, getc_func, NULL, file_name, NULL)) {
      fprintf (stderr, "jit-spike: c2mir FAILED on %s\n", file_name);
      return 3;
    }
    double t1 = now_ms ();
    c2mir_finish (ctx);

    /* Load every module c2mir produced, then link + generate. */
    for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
         m = DLIST_NEXT (MIR_module_t, m))
      MIR_load_module (ctx, m);

    MIR_gen_init (ctx);
    MIR_gen_set_optimize_level (ctx, (unsigned) opt_level);
    MIR_link (ctx, eager_p ? MIR_set_gen_interface : MIR_set_lazy_gen_interface, import_resolver);
    sync_config_globals (ctx);

    /* Find main across the loaded modules. */
    MIR_item_t main_item = NULL;
    for (MIR_module_t m = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx)); m != NULL;
         m = DLIST_NEXT (MIR_module_t, m))
      for (MIR_item_t it = DLIST_HEAD (MIR_item_t, m->items); it != NULL;
           it = DLIST_NEXT (MIR_item_t, it))
        if (it->item_type == MIR_func_item && strcmp (it->u.func->name, "main") == 0) main_item = it;
    if (main_item == NULL) { fprintf (stderr, "jit-spike: no main\n"); return 3; }
    double t2 = now_ms ();

    if (t1 - t0 < best_c2mir) best_c2mir = t1 - t0;
    if (t2 - t1 < best_gen) best_gen = t2 - t1;

    /* Only actually RUN the program on the last iteration: these fixtures
       print to stdout and we diff that against expected.stdout. */
    if (iter == repeat - 1) {
      typedef int (*main_fn) (int, char **, char **);
      main_fn fn = (main_fn) main_item->addr;
      /* Run the entry on a thread with an explicitly sized stack (findings
         15.3).  MIR-gen frames for direct-path recursion are roughly 2x
         gcc's, so a program whose deep recursion fits the default 8 MB under
         `tur build` can blow the stack under the JIT (gc-registry-growth:
         20,000 frames).  Sizing the entry stack is the sanctioned STOPGAP --
         "any size temporarily is fine" (owner, 2026-07-29) -- but the same
         decision names the long-run direction: keep the runtime's stackless
         architecture (the CPS/DK heap-continuation machinery, which runs
         under MIR unchanged) rather than institutionalizing ever-bigger
         stacks.  The eventual fix is MIR frame-size work or routing deep
         direct recursion through the existing stackless machinery -- not a
         larger constant here.  TUR_JIT_STACK_MB overrides the default 64. */
      struct jit_entry_box box = {fn, 0};
      size_t stack_mb = 64;
      const char *env = getenv ("TUR_JIT_STACK_MB");
      if (env != NULL && atoi (env) > 0) stack_mb = (size_t) atoi (env);
      pthread_attr_t attr;
      pthread_t entry_thread;
      pthread_attr_init (&attr);
      pthread_attr_setstacksize (&attr, stack_mb * 1024 * 1024);
      if (pthread_create (&entry_thread, &attr, jit_run_entry, &box) != 0) {
        fprintf (stderr, "jit-spike: entry thread create failed\n");
        return 4;
      }
      pthread_join (entry_thread, NULL);
      pthread_attr_destroy (&attr);
      rc = box.rc;
    }

    MIR_gen_finish (ctx);
    MIR_finish (ctx);
  }

  if (!quiet_p)
    fprintf (stderr, "jit-spike: c2mir %.1fms  link+gen %.1fms  (best of %d, -O%d, %s)\n",
             best_c2mir, best_gen, repeat, opt_level, eager_p ? "eager" : "lazy");
  return rc;
}
