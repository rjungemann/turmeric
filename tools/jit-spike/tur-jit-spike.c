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

/* c2mir emits calls to runtime functions by name; MIR asks us for an address.
   The runtime is compiled INTO this executable, so dlsym(RTLD_DEFAULT) finds
   it -- c2mir never parses a line of hamt.c. */
static void *import_resolver (const char *name) {
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
      char *fake_argv[] = {(char *) "jit", NULL};
      char *fake_envp[] = {NULL};
      rc = fn (1, fake_argv, fake_envp);
      fflush (stdout);
    }

    MIR_gen_finish (ctx);
    MIR_finish (ctx);
  }

  if (!quiet_p)
    fprintf (stderr, "jit-spike: c2mir %.1fms  link+gen %.1fms  (best of %d, -O%d, %s)\n",
             best_c2mir, best_gen, repeat, opt_level, eager_p ? "eager" : "lazy");
  return rc;
}
