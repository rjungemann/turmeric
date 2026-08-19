/* tur: the Turmeric compiler driver (phase 2). */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* _DARWIN_C_SOURCE exposes mkstemps and DT_REG on macOS without suppressing
 * POSIX extensions; on Linux _DEFAULT_SOURCE covers both. */
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#ifndef _WIN32
/* poll(2) has no Windows counterpart for CRT file descriptors; the only user is
 * the fork-based fixture worker, which is compiled out below. */
#include <poll.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include "platform_fs.h"  /* realpath/mkdir/setenv/mkstemps/... on Windows */
#ifdef _WIN32
#include <io.h>       /* _setmode, _fileno */
#endif
#ifndef _WIN32
/* waitpid() is only used by the fork-based fixture worker, which is compiled out
 * on Windows.  The WIFEXITED/WEXITSTATUS uses that remain are applied to a
 * system() return value, and platform_fs.h defines those there. */
#include <sys/wait.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>     /* SN1: _NSGetExecutablePath for stdlib resolution */
#endif

#include "arena.h"
#include "assert.h"
#include "buf.h"
#include "borrow_check.h"  /* Phase 14 */
#include "cps.h"          /* Phase 18: CPS transformation */
#include "cps_ir.h"       /* CPS2: ANF/CPS IR (--dump-cps) */
#include "diag.h"
#include "effect_check.h" /* Phase P19-2: effect-row inference */
#include "effect.h"       /* built-in effect registration */
#include "kind_check.h"   /* Phase HKT H0: kind inference pass */
#include "elab.h"
#include "emit.h"
#include "runtime/hamt.h" /* S2: tur_hamt_hash_xxh64 for the split-artifact hash */
#include "runtime/rt_split_embed.h" /* S2: committed decls region + hash (TUR_JIT) */
#include "turi/spice_loader.h" /* J2: the REPL's in-process jit hook */
#include "turi/jit_ffi.h"      /* jit-ffi-c2mir-plan: dynamic-FFI provider */
#include "effect_lower.h" /* Phase 19: Effect lowering */
#include "expr.h"
#include "fmt.h"
#include "forms.h"
#include "pass.h"         /* Phase P19-1: pass scheduling */
#include "reader.h"
#include "reader_macros.h"
#include "lang_layers.h"  /* L5: `tur lang-layers` registry listing */
#include "refine_discharge.h" /* RT3: per-compile refinement stats reset */
#include "span_audit.h"  /* debugger Phase 1: breakpoint-span coverage audit */
#include "symbols.h"
/* Phase S0: eval API for tur repl */
#include "turi/eval.h"
#include "turi/collections_native.h"
#include "turi/interpreter_natives.h"
#include "turi/preload.h"
/* Phase S1: REPL with libedit, multi-line input, :type/:doc/:reload */
#include "turi/repl.h"
/* Phase PKG-1: Spice package manager */
#include "pkg.h"
/* RN0-RN7: Justfile-compatible task runner */
#include "justrun.h"
/* Global configuration variables — defined in globals.c */
#include "globals.h"
#include "experiments.h"  /* XF1: --enable=<name> experimental-flag registry */
#include "jit_engine.h"   /* J1: tur jit (TUR_HAVE_JIT builds only) */
#include "mono_specs.h"   /* VBM1: --dump-mono-specs registry dump */
/* LSP server */
#include "lsp/lsp.h"
#include "lsp/lsp_sym.h"
#include "lsp/lsp_collect.h"
#include "stdlib_autoload.h"
#include "lsp/lsp_docs.h"
/* MCP server */
#include "lsp/mcp.h"
/* DAP server (debugger Phase 3) */
#include "turi/dap.h"
/* lsp-lite: lightweight completion/calltip backend for editors */
#include "cli/lsp_lite.h"
/* tur completion <zsh|bash>: emit a shell completion script */
#include "cli/completion.h"

#ifndef TUR_VERSION
#define TUR_VERSION "unknown"
#endif

/* E14: structured JSON output global — set by --json flag.
 * When true: tur doc prints JSON; tur test prints JSON; tur check uses JSON diag.
 * The storage lives in install.c so libturi (which omits main.c but pulls in
 * tur_core) resolves it cleanly. */
extern bool use_json_output;

/* Escape a string for JSON output (handles backslash, double-quote, newline). */
static void json_escape(const char *s, char *out, size_t cap) {
    size_t i = 0;
    while (*s && i + 4 < cap) {
        if (*s == '\\' || *s == '"') { out[i++] = '\\'; out[i++] = *s++; }
        else if (*s == '\n')          { out[i++] = '\\'; out[i++] = 'n'; s++; }
        else                          { out[i++] = *s++; }
    }
    out[i] = '\0';
}

/* Extract basename from a path. */
static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Global configuration variables — defined in globals.c (part of tur_core) */
/* Phase HKT-P6: --dump-kinds flag: print kind annotations after kind-check */
static bool g_dump_kinds = false;

/* debugger Phase 1: `tur audit-spans` mode.  When set, run_core_passes stops
 * after PASS_ELABORATE and audits the program for breakpoint-eligible nodes
 * lacking a usable source span; the hole count lands in g_audit_span_holes. */
static bool g_audit_spans = false;
static int  g_audit_span_holes = 0;

/* Helper to detect language and adjust source for #lang directive.  Also
 * yields the additive `#lang` layer set (lang-layers-plan L1) via
 * `out_layers` (may be NULL for callers that don't thread it). */
static ReaderType detect_and_adjust_lang(const char *path, char *src, size_t len,
                                        const char **out_src, size_t *out_len,
                                        LangLayerSet *out_layers) {
    ReaderType ext_type = reader_type_from_extension(path);

    /* Always run detect_lang so any leading "#lang ..." line is stripped from
     * the source — otherwise the chosen reader would choke on the '#'. When
     * the extension already selected a non-default reader, the extension
     * still wins for the base; the directive is treated as an optional,
     * redundant hint.  Layers ride alongside the base regardless of source. */
    const char *src_rest = src;
    size_t len_rest = len;
    LangLayerSet layers = 0;
    const char *bad = NULL;
    size_t bad_len = 0;
    ReaderType lang_type = detect_lang_layered(src, len, &src_rest, &len_rest,
                                               &layers, &bad, &bad_len);

    if (bad) {
        /* Unknown layer token -- hard error (TUR-E0330), mirroring the
         * unimplemented-base exit below. */
        fprintf(stderr,
                "tur: error [TUR-E0330]: unknown #lang layer '%.*s' in %s\n",
                (int)bad_len, bad, path);
        exit(1);
    }

    ReaderType detected_type = (ext_type != READER_TURMERIC) ? ext_type : lang_type;

    /* Check if the reader is implemented */
    if (!reader_type_is_implemented(detected_type)) {
        fprintf(stderr, "tur: error: #lang %s is not yet implemented\n",
                reader_type_name(detected_type));
        exit(1);
    }

    *out_src = src_rest;
    *out_len = len_rest;
    if (out_layers) *out_layers = layers;
    return detected_type;
}

/* Compute the directory part of a file path (Phase M2: module base dir). */
static void dir_of_path(const char *path, char *out, size_t cap) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        out[0] = '.'; out[1] = '\0';
    } else {
        size_t n = (size_t)(last_slash - path);
        if (n == 0) n = 1; /* handle "/foo" → "/" */
        if (n >= cap) n = cap - 1;
        memcpy(out, path, n);
        out[n] = '\0';
    }
}

/* SN1: argv[0] stashed at startup as the last-resort exe-path hint.
 * Resolved via platform APIs in get_exe_path() first; argv[0] is only used
 * if those fail. */
static const char *g_argv0 = NULL;

/* SN1: write the absolute path of the running `tur` executable to out.
 * Returns 0 on success, -1 on failure (in which case out is unmodified). */
static int get_exe_path(char *out, size_t cap) {
#ifdef __APPLE__
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) == 0) {
        /* _NSGetExecutablePath returns a path that may contain symlinks
         * or `..`; resolve to a canonical form so the walk-up sees the
         * real directory layout. */
        char real[4096];
        if (realpath(out, real)) {
            size_t rl = strlen(real);
            if (rl < cap) { memcpy(out, real, rl + 1); }
        }
        return 0;
    }
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0) { out[n] = '\0'; return 0; }
#endif
    if (g_argv0 && strchr(g_argv0, '/')) {
        /* argv[0] contains a path component -- resolve it. */
        if (realpath(g_argv0, out)) return 0;
    }
    return -1;
}

/* SN1: resolved stdlib root directory.  Set lazily on first call to
 * resolve_stdlib_root(), reused thereafter.  Empty string means "we
 * tried and could not find one"; NULL means "not yet attempted". */
static char  g_stdlib_root[4096] = "";
static int   g_stdlib_root_state = 0;  /* 0=unresolved, 1=found, 2=not-found */

/* SN1: locate the stdlib root directory.  Precedence:
 *   1. $TUR_STDLIB_DIR (verbatim, no walk-up)
 *   2. walk up from exe directory looking for `stdlib/macros.tur`
 *      -- supports both the in-tree dev layout (sibling of `build/`) and
 *      a sibling `stdlib/` in any ancestor.
 *   3. `<exe_dir>/../share/turmeric/stdlib` -- prefix-style installed layout.
 *   4. literal "stdlib" -- last-resort fallback matching the legacy
 *      cwd-relative behavior so users running from a repo root still work.
 * Returns a pointer to a static buffer, or NULL if no directory was found
 * and the legacy fallback was not desired.  The current implementation
 * always returns non-NULL: in the worst case it returns the literal
 * "stdlib" so callers can still attempt the open. */
static const char *resolve_stdlib_root(void) {
    if (g_stdlib_root_state != 0) {
        return g_stdlib_root[0] ? g_stdlib_root : "stdlib";
    }
    g_stdlib_root_state = 2;  /* assume not-found until we succeed */

    /* TUR_STDLIB_DIR is honored, but no longer taken on faith.
     *
     * It is an ordinary environment variable, so it is inherited by anything
     * a tur process spawns and it outlives the install that set it. A stale
     * value therefore points a freshly built `tur` at a stdlib that has moved
     * or been deleted, and taking it verbatim meant the failure surfaced much
     * later as a wall of `load: cannot open .../macros.tur` errors with
     * nothing naming the variable that caused them.
     *
     * macros.tur is the anchor, matching the walk-up probe below: it is the
     * first file every preload touches, so if it is missing nothing else will
     * resolve either. A directory that fails the check is reported once and
     * then ignored, letting the walk-up find the stdlib shipped beside this
     * binary -- which is nearly always what the user actually wanted. */
    const char *env = getenv("TUR_STDLIB_DIR");
    if (env && *env) {
        size_t n = strlen(env);
        if (n < sizeof(g_stdlib_root)) {
            char probe[4096];
            int pn = snprintf(probe, sizeof(probe), "%s/macros.tur", env);
            if (pn > 0 && (size_t)pn < sizeof(probe) && access(probe, R_OK) == 0) {
                memcpy(g_stdlib_root, env, n + 1);
                g_stdlib_root_state = 1;
                return g_stdlib_root;
            }
            fprintf(stderr,
                    "tur: ignoring TUR_STDLIB_DIR=%s "
                    "(no readable macros.tur there); "
                    "falling back to the stdlib beside the binary\n", env);
            /* Drop it so every downstream reader -- elab_toplevel.c, the REPL
             * preload, lsp_lite.c -- agrees with the value resolved below
             * instead of re-reading the bad one out of the environment. */
            unsetenv("TUR_STDLIB_DIR");
        }
    }

    char exe[4096];
    if (get_exe_path(exe, sizeof(exe)) == 0) {
        char dir[4096];
        dir_of_path(exe, dir, sizeof(dir));

        /* Walk up to 8 levels looking for `<dir>/stdlib/macros.tur`.
         * macros.tur is the anchor: it's the first file every preload
         * loop touches, so if it's missing nothing else will resolve. */
        char probe[4096];
        for (int depth = 0; depth < 8; depth++) {
            int n = snprintf(probe, sizeof(probe), "%s/stdlib/macros.tur", dir);
            if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
                int rn = snprintf(g_stdlib_root, sizeof(g_stdlib_root),
                                  "%s/stdlib", dir);
                if (rn > 0 && (size_t)rn < sizeof(g_stdlib_root)) {
                    g_stdlib_root_state = 1;
                    /* Propagate to TUR_STDLIB_DIR so the elaborator
                     * (elab_toplevel.c) sees the same value without
                     * needing its own copy of this resolver.  Use
                     * overwrite=0 so an explicit user override wins;
                     * we already returned above if env was set. */
                    setenv("TUR_STDLIB_DIR", g_stdlib_root, 0);
                    return g_stdlib_root;
                }
            }
            /* Try installed prefix layout: `<dir>/share/turmeric/stdlib`. */
            n = snprintf(probe, sizeof(probe),
                         "%s/share/turmeric/stdlib/macros.tur", dir);
            if (n > 0 && (size_t)n < sizeof(probe) && access(probe, R_OK) == 0) {
                int rn = snprintf(g_stdlib_root, sizeof(g_stdlib_root),
                                  "%s/share/turmeric/stdlib", dir);
                if (rn > 0 && (size_t)rn < sizeof(g_stdlib_root)) {
                    g_stdlib_root_state = 1;
                    setenv("TUR_STDLIB_DIR", g_stdlib_root, 0);
                    return g_stdlib_root;
                }
            }
            /* Step up one level. */
            char *slash = strrchr(dir, '/');
            if (!slash || slash == dir) break;
            *slash = '\0';
        }
    }

    /* Legacy fallback: literal "stdlib" relative to cwd.  Preserved so
     * developers running from the repo root still get a working binary
     * even if the exe-walk failed (e.g. unusual sandbox).  Callers that
     * want a hard error on missing stdlib should check g_stdlib_root_state. */
    return "stdlib";
}

/* Resolve a stdlib basename like "macros.tur" to a full path.
 *
 * Order of precedence:
 *   1. $TUR_STDLIB_DIR/<basename>  -- explicit env-var override
 *   2. <exe_walk_up>/stdlib/<basename> -- walk up from the tur binary
 *   3. ./stdlib/<basename>         -- legacy cwd-relative fallback
 *
 * Returns a pointer to `out` (NUL-terminated). On overflow, the path is
 * truncated and a warning is emitted to stderr; callers still get a usable
 * pointer.
 */
static const char *tur_stdlib_path(const char *basename,
                                   char *out, size_t outlen) {
    const char *sdir = resolve_stdlib_root();
    int n = snprintf(out, outlen, "%s/%s", sdir, basename);
    if (n < 0 || (size_t)n >= outlen) {
        fprintf(stderr,
                "tur: stdlib path too long for '%s' (TUR_STDLIB_DIR='%s')\n",
                basename, sdir);
    }
    return out;
}

/* SN2: read a file without printing on failure.  Used by stdlib preload
 * loops where missing optional files should be silent.  Returns 0 on
 * success (out + out_len populated, caller frees out), -1 if the file
 * cannot be opened or read.  Never writes to stderr. */
static int read_entire_file_quiet(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

static int read_entire_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tur: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); fprintf(stderr, "tur: oom\n"); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }
    buf[size] = '\0';
    *out = buf;
    *out_len = (size_t)size;
    return 0;
}

/* Ordered list of compiler passes executed for every compilation unit.
 * To insert a new pass: add a PassKind constant to pass.h, add a case here,
 * and add the case in run_core_passes() below.                              */
static const PassKind core_passes[] = {
    PASS_ELABORATE,
    PASS_KIND_CHECK,
    PASS_EFFECT_LOWER,
    PASS_EFFECT_ROW_INFER,  /* P19-2: stub slot — no-op until inference lands */
    PASS_CPS,
    PASS_BORROW_CHECK,
};
static const int n_core_passes = (int)(sizeof(core_passes) / sizeof(core_passes[0]));

/* Run all core compiler passes in order.  ctx->forms/nforms must be set
 * before calling; ctx->prog and ctx->effect_env are populated by the passes.
 * Returns 0 on success, 1 on the first pass failure.                        */
static int run_core_passes(PassContext *ctx) {
    for (int i = 0; i < n_core_passes; i++) {
        switch (core_passes[i]) {
        case PASS_ELABORATE:
            ctx->prog = elaborate_program(ctx->arena, ctx->st,
                                          ctx->forms, ctx->nforms,
                                          ctx->stdlib_prefix,
                                          ctx->module_base_dir,
                                          ctx->separate_compilation,
                                          /*sandboxed=*/false,
                                          &ctx->tc_env,
                                          ctx->include_dirs,
                                          ctx->n_include_dirs,
                                          NULL,
                                          ctx->reader_macros);
            if (!ctx->prog || diag_had_error()) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info is preserved after elaboration */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_ELABORATE");
#endif
            /* VBM1/VBM2 (van-laarhoven-monomorphization-plan): resolve each
             * abstract lens spec discovered during elaboration to the concrete
             * lens passed at its enclosing fn's top-level invocations (VBM2's
             * cross-procedural collapse -- plan OQ #1/#2), then --dump-mono-specs
             * prints both the abstract and resolved-concrete registries.
             * Registry-only: codegen is unchanged (the per-spec by-value body
             * emit is tracked separately).  Graduated (vl-wide-mono, 2026-07-05):
             * the resolve pass is unconditional; it no-ops when no wide-functor
             * lens pin populated the registry during elaboration. */
            mono_specs_resolve_program(ctx->prog);
            if (g_dump_mono_specs) {
                mono_specs_dump(stdout);
                /* `tur run` calls this pass in-process then execv's the compiled
                 * binary, which discards any unflushed stdio buffer.  Flush so the
                 * dump survives to a redirected (fully-buffered) stdout. */
                fflush(stdout);
            }
            /* debugger Phase 1: audit breakpoint-span coverage on the freshly
             * elaborated tree, before any transform pass introduces synthetic
             * (legitimately span-less) nodes.  Stop the pipeline afterwards --
             * audit mode never emits. */
            if (g_audit_spans) {
                g_audit_span_holes = span_audit_program(ctx->prog, stdout);
                return 0;
            }
            break;
        case PASS_KIND_CHECK:
            /* Phase HKT H0: kind inference and validation pass (v1 stub). */
            if (kind_check_pass(ctx->arena, ctx->prog) != 0)
                return 1;
            /* Phase HKT-P6: optionally dump kind annotations for debugging. */
            if (g_dump_kinds)
                kind_dump_program(ctx->prog, stdout);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info is preserved after kind-check */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_KIND_CHECK");
#endif
            break;
        case PASS_EFFECT_LOWER:
            /* Phase 19: transform perform/handle into shift/reset. */
            ctx->effect_env = effect_env_new(ctx->arena);
            effect_env_register_builtin_unsafe(
                ctx->effect_env, ctx->arena,
                symtab_intern(ctx->st, strslice(EFFECT_NAME_UNSAFE, 6)));
            ctx->prog = effect_lower(ctx->arena, ctx->st,
                                     ctx->prog, ctx->effect_env);
            if (!ctx->prog || diag_had_error()) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after effect-lower */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_EFFECT_LOWER");
#endif
            break;
        case PASS_EFFECT_ROW_INFER:
            /* P19-2: effect-row inference and validation pass. */
            if (effect_check_pass(ctx->arena, ctx->prog, ctx->effect_env) != 0)
                return 1;
            /* ER6: --dump-effects: print inferred effect row for each defn. */
            if (g_dump_effects)
                effect_check_dump_effects(ctx->prog, stdout);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after effect-row-infer */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_EFFECT_ROW_INFER");
#endif
            break;
        case PASS_CPS:
            /* CPS1 (cps-transform-plan): --dump-cps-coloring runs the
             * whole-program may-capture coloring on the pre-transform tree and
             * prints the colored/uncolored partition. */
            if (g_dump_cps_coloring)
                cps_dump_coloring(ctx->arena, ctx->prog, stdout);
            /* CPS2 (cps-transform-plan): --dump-cps prints the ANF/CPS IR for
             * every colored function (dump-only; not wired into codegen). */
            if (g_dump_cps)
                cps_ir_dump_program(ctx->arena, ctx->prog, stdout);
            /* Phase 18: CPS transformation for shift/reset. */
            ctx->prog = cps_transform(ctx->arena, ctx->prog, &ctx->tc_env);
            if (!ctx->prog || diag_had_error()) return 1;
            /* Phase B5: --dump-clone-plan: print cloneable capture plan after CPS */
            if (g_dump_clone_plan) cps_dump_clone_plan(ctx->prog, stderr);
            /* CPS1: --dump-cps-coloring: print colored/uncolored partition */
            if (g_dump_cps_coloring) cps_dump_cps_coloring(ctx->prog, stderr);
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after CPS */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_CPS");
#endif
            break;
        case PASS_BORROW_CHECK:
            /* Phase 14: ownership, move, and borrow analysis. */
            if (!borrow_check_program(ctx->prog)) return 1;
            /* Phase P19-7: Always-on check that handler case bodies do not
             * capture borrow-typed variables from the enclosing scope. */
            if (!borrow_check_effect_handler_captures(ctx->prog)) return 1;
            /* TY4: Always-on lifetime pass -- elision + outlives cycle check. */
            if (!lifetime_check_program(ctx->prog)) return 1;
#ifndef NDEBUG
            /* Phase HKT-P6: verify kind info preserved after borrow-check */
            assert(kind_verify_program(ctx->prog) && "Kind info cleared after PASS_BORROW_CHECK");
#endif
            break;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * LSP symbol collection (LD1)
 * --------------------------------------------------------------------- */

/* Forward declaration: compile_to_c is defined later in this file */
static int compile_to_c(const char *path, Buf *out_c,
                        const char **include_dirs, int n_include_dirs,
                        const char **reader_macro_paths,
                        int n_reader_macro_paths);

/* RM4 follow-up forward decls. */
static char **discover_manifest_reader_macros(const char *input_path,
                                              int *n_out);
static void free_reader_macro_paths(char **paths, int n);

/* SC1/SC2 forward decls: include-flag helpers used from cmd_run before
 * their definitions later in this file. */
static int  parse_include_flags(int argc, char **argv, int start, char ***out_dirs);
static bool is_include_flag(int argc, char **argv, int i, int *consumed);
static int  usage_run(void);

/* SC4 forward decl: --no-auto-spice flag inspected by auto_append_spice_src
 * (defined below find_spice_root) but set by parse_no_auto_spice in main(). */
static bool g_no_auto_spice;

/* Forward decl: set by the tur check handler when --no-auto-stdlib is passed.
 * Used inside compile_to_c's stdlib auto-load loop (suffix-skip). */
static bool g_no_auto_stdlib;

/* J6 forward decl: --no-abi-cache / TUR_NO_ABI_CACHE inspected at the
 * .tur-abi-cache/ write sites in cmd_build_multi / cmd_emit_c_to_dir;
 * set by parse_no_abi_cache in main(). */
static bool g_no_abi_cache;

/* tur-link-and-build-split-plan Phase 2/3c/6: runtime-linkage mode, read by
 * apply_runtime_lib_mode() in cmd_build / cmd_compile.  Set by the build/compile
 * dispatch and seeded from TUR_RUNTIME.  Values:
 *   TUR_RT_AUTO   (0, the default) -- prefer the lean non-ASan libturt_runtime.a
 *                 when it is locatable, else transparently fall back to
 *                 recompiling the bare runtime sources.  Never links the
 *                 (possibly ASan) full libturi.a on its own, so a default build
 *                 is always behaviorally identical to the old source path.
 *   TUR_RT_LIB    (1) -- force the archive link (lean preferred, else libturi.a,
 *                 else a -lturi fallback with a warning); explicit opt-in.
 *   TUR_RT_SOURCE (2) -- force recompiling the bare runtime sources. */
enum { TUR_RT_AUTO = 0, TUR_RT_LIB = 1, TUR_RT_SOURCE = 2 };
static int g_runtime_mode = TUR_RT_AUTO;

/* DEDUP-4b (docs/archive/gc-cycle-collection-plan.md): resolve whether the emitted preamble
 * should DECLARE the rc<T>/GC runtime -- because libturt_runtime.a will supply
 * it -- instead of defining its own hand-written replica, and tell the emitter
 * before codegen runs.
 *
 * ON by default, but ONLY when `tur` is the one doing the linking.  A preamble
 * that declares without defining is not self-contained C, and bare
 * `tur emit-c` exists precisely to hand someone a translation unit they will
 * build themselves -- so that path keeps the definitions (and its snapshots
 * keep matching).  `tur build` / project builds control their own link line, so
 * they take the archive.  g_emit_for_link is what distinguishes the two.
 *
 * Also requires an actual archive: without one the program would link nothing
 * at all, so a failed probe silently keeps the emitted definitions.  That is
 * what makes this safe to default -- a toolchain with no archive still builds,
 * it just keeps running the replica.
 *
 * TUR_RCGC_FROM_ARCHIVE=0 forces the replica back (escape hatch if an archive
 * link ever misbehaves); =1 forces the archive even for bare `emit-c`, for
 * someone who links libturt_runtime.a from their own build system.
 *
 * Safe to call before the link step decides anything: locate_runtime_lib is a
 * pure filesystem probe with no side effects. */
static int locate_runtime_lib(char *libdir, size_t dcap,
                              char *libname, size_t ncap);

/* True while emitting C that `tur` itself will go on to compile and link. */
static bool g_emit_for_link = false;

/* J2: when non-NULL, compile_to_c also appends the exports manifest of the
 * compiled program here (set only by the REPL's in-process spice build). */
static Buf *g_manifest_sink = NULL;

static void resolve_rcgc_from_archive(void) {
    const char *opt = getenv("TUR_RCGC_FROM_ARCHIVE");
    bool forced_on = opt && strcmp(opt, "1") == 0;

    if (opt && strcmp(opt, "0") == 0)     { emit_set_rcgc_from_archive(false); return; }
    if (g_runtime_mode == TUR_RT_SOURCE)  { emit_set_rcgc_from_archive(false); return; }
    if (!g_emit_for_link && !forced_on)   { emit_set_rcgc_from_archive(false); return; }

    char libdir[4096], libname[128];
    int found = locate_runtime_lib(libdir, sizeof(libdir), libname, sizeof(libname));
    emit_set_rcgc_from_archive(found && strcmp(libname, "turt_runtime") == 0);
}

/* SC4+SC5+SC6 forward decl: auto-append helper used from tur_check_only
 * (called by the LSP server) and the per-file dispatchers.
 *
 * LS2: when out_ls2 is non-NULL, the helper also populates a parallel
 * provenance array (workspace-sibling producer path per include dir),
 * the consumer's declared :spices map keys, and the TUR_DEBUG_RESOLVER
 * flag. Caller is responsible for calling ls2_resolver_ctx_dispose
 * after compile_to_c returns. Pass NULL to skip the LS2 bookkeeping. */
static int auto_append_spice_includes(const char *input,
                                      char ***inc, int *n_inc,
                                      char ***owned, int *n_owned,
                                      Ls2ResolverCtx *out_ls2);

static void ls2_resolver_ctx_dispose(Ls2ResolverCtx *ctx);

int tur_collect_symbols(const char *path, LspSymbol *out, int cap,
                        int *count_out) {
    lsp_collect_begin(out, cap, count_out);
    Buf discard;
    buf_init(&discard);
    int rm_n = 0;
    char **rm_p = discover_manifest_reader_macros(path, &rm_n);
    int rc = compile_to_c(path, &discard, NULL, 0,
                          (const char **)rm_p, rm_n);
    free_reader_macro_paths(rm_p, rm_n);
    buf_free(&discard);
    lsp_collect_end();
    return rc;
}

/* Reads a .tur file and emits its C source into `out_c`. Returns 0 on success,
 * nonzero on error (diagnostics already emitted).
 * include_dirs/n_include_dirs: additional module search paths for (import ...).
 * reader_macro_paths/n_reader_macro_paths: RM4 — absolute paths to
 * `(reader-macros/define ...)` definition files that are preloaded into
 * the reader's macro registry before the entry file is parsed. Typically
 * derived from the project's `build.tur :reader-macros [...]` entry. */
/* Thin adapter over the shared prepend in compiler/stdlib_autoload.c.
 * The list, the read loop, and the form splice moved there so the WASM
 * playground's in-process analyzer prepends exactly the same stdlib this
 * does; what stays here is the CLI's own two answers -- where the stdlib
 * lives (resolve_stdlib_root, an exe-relative walk-up that means nothing in
 * a browser) and whether --no-auto-stdlib was passed. */
static uint32_t prepend_stdlib_forms(Arena *arena, SymbolTable *st,
                                     const char *entry_path,
                                     Form ***forms_in_out,
                                     uint32_t *nforms_in_out,
                                     uint8_t *file_id_in_out) {
    return tur_stdlib_prepend_forms(arena, st, resolve_stdlib_root(),
                                    entry_path, g_no_auto_stdlib,
                                    forms_in_out, nforms_in_out,
                                    file_id_in_out);
}

static int compile_to_c(const char *path, Buf *out_c,
                         const char **include_dirs, int n_include_dirs,
                         const char **reader_macro_paths,
                         int n_reader_macro_paths) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    LangLayerSet lang_layers = 0;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj, &lang_layers);

    /* Each compile_to_c call is a self-contained compilation unit.  Clear the
     * global diagnostic state (the `had_error_` flag and the file registry)
     * before registering this file so a prior failed compile in the same
     * process cannot poison this one.  Without this, batch drivers that loop
     * compile_to_c in-process (`tur test <dir>`, `tur check <dir>`) would mark
     * every alphabetically-later file as FAIL once any earlier file failed to
     * compile -- the stale `had_error_` short-circuits the `diag_had_error()`
     * gate below. */
    diag_reset();
    experiment_reset_warnings();  /* XF2: once-per-compile TUR-W006x dedup */
    pkg_tur_version_reassert();   /* :tur-version floor survives the reset above */
    pkg_manifest_reassert();      /* a broken build.tur likewise: error: + exit 0 is not an error */
    refine_discharge_reset();     /* RT3: once-per-compile refinement stats */

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
    file.lang_layers = lang_layers;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4: preload spice-manifest `:reader-macros [...]` into a registry
     * the entry-file reader will see. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    for (int i = 0; i < n_reader_macro_paths; ++i) {
        if (reader_macros_load_file(&arena, &st,
                                    reader_macro_paths[i],
                                    &reader_macros_reg) != 0) {
            arena_free(&arena);
            symtab_free(&st);
            free(src);
            return 1;
        }
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    /* Phase 7: prepend stdlib autoload forms.  Shared with compile_to_h via
     * prepend_stdlib_forms so project-mode builds see the same stdlib API
     * (Cons / Option / Result / typeclass stubs / etc.) that single-file
     * builds do.  See docs/archive/history/project-mode-no-stdlib-autoload.md. */
    uint8_t file_id = 1;
    uint32_t total_stdlib_forms = prepend_stdlib_forms(&arena, &st, path,
                                                       &forms, &nforms,
                                                       &file_id);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        char base_dir[4096];
        dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena = &arena;
        ctx.st    = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.stdlib_prefix = total_stdlib_forms;
        ctx.module_base_dir = base_dir;
        ctx.include_dirs    = include_dirs;
        ctx.n_include_dirs  = n_include_dirs;
        ctx.reader_macros   = &reader_macros_reg;
        rc = run_core_passes(&ctx);
        /* Collect symbols whether or not later passes failed -- elaboration
         * may have succeeded even when borrow-check reports errors. */
        if (lsp_collect_active() && ctx.prog)
            lsp_collect_program(ctx.prog);
        if (g_audit_spans) {
            /* Audit-only mode: never emit.  A clean audit is rc 0; remaining
             * holes surface as rc 3 (distinct from rc 1 = elaboration failure,
             * rc 2 = file error) so harnesses can tell "audited, found holes"
             * apart from "could not audit". */
            if (rc == 0 && g_audit_span_holes > 0) rc = 3;
        } else if (rc == 0) {
            /* DEDUP-4b: decide before emitting -- the preamble's text depends
             * on whether the archive will supply the rc<T>/GC runtime. */
            resolve_rcgc_from_archive();
            if (emit_program(out_c, ctx.prog) != 0) rc = 1;
            /* J2: the REPL's in-process spice build wants the exports
             * manifest from this same single-TU compile (the sink is set
             * only around that call; every other caller leaves it NULL). */
            if (rc == 0 && g_manifest_sink
                && emit_exports_manifest(g_manifest_sink, ctx.prog) != 0)
                rc = 1;
        }
    }

    /* Phase U5: Print unsafe statistics if enabled */
    if (g_unsafe_stats_enabled) {
        fprintf(stderr, "unsafe stats: %u blocks, %u total forms\n",
                g_unsafe_block_count, g_unsafe_total_lines);
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Load macro-only prelude files (macros.tur) into an arena/symtab so that
 * project-mode compilation (compile_to_h / compile_to_implementation) has the
 * same prelude macros visible as single-file mode.  Only macro-providing files
 * are loaded here to avoid the separate-compilation defn-emission gap (F6 in
 * the prelude-macros bug report): macros are compile-time only and emit no
 * object code, so they are safe to inject into every module's elaboration.
 *
 * Returns a new forms array with prelude forms prepended before user_forms.
 * *stdlib_count_out is set to the number of prepended prelude forms.
 * On any load failure the returned array equals user_forms unchanged and
 * *stdlib_count_out = 0. */
static Form **load_project_prelude(Arena *arena, SymbolTable *st,
                                    Form **user_forms, uint32_t nuser,
                                    uint32_t *stdlib_count_out) {
    *stdlib_count_out = 0;

    /* The prelude IS the project-mode stdlib auto-load: every file the
     * single-file compile_to_c path loads (g_stdlib_autoload_files) is
     * preloaded here in the same order, so `(Cons A)` / `tnil?` /
     * `Option`/`Result` etc. are visible inside spice defmodule bodies
     * built via `tur build .` without explicit imports.  See
     * docs/archive/history/project-mode-no-stdlib-autoload.md.
     *
     * Codegen note: bindings created during this prelude window are
     * marked `is_from_stdlib`. emit_implementation skips non-exported
     * stdlib bindings; exported ones (`ok`, `ok-val`,
     * `make-struct Result ...`) are static-or-inline-C wrappers so the
     * per-TU duplication mirrors `emit_closure_fat_runtime`. */
    const char *const *prelude_files = tur_stdlib_autoload_files();

    uint32_t total = 0;
    Form **all = NULL;
    uint8_t fid = 1;

    for (int i = 0; prelude_files[i] != NULL; i++) {
        char path_buf[4096];
        tur_stdlib_path(prelude_files[i], path_buf, sizeof(path_buf));
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        if (read_entire_file_quiet(path_buf, &stdlib_src, &stdlib_len) != 0)
            continue;

        char *src_copy = (char *)arena_alloc(arena, stdlib_len);
        memcpy(src_copy, stdlib_src, stdlib_len);
        char *path_copy = (char *)arena_alloc(arena, strlen(path_buf) + 1);
        memcpy(path_copy, path_buf, strlen(path_buf) + 1);
        free(stdlib_src);

        SourceFile *sf = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
        *sf = (SourceFile){0};
        sf->path       = path_copy;
        sf->src        = src_copy;
        sf->len        = stdlib_len;
        sf->file_id    = fid++;
        sf->reader_type = READER_TURMERIC;
        diag_register_file(sf);

        uint32_t n = 0;
        Form **f = read_all(arena, st, sf, &n);

        if (f && n > 0) {
            Form **merged = (Form **)arena_alloc(arena, (total + n) * sizeof(Form *));
            for (uint32_t j = 0; j < total; j++) merged[j] = all[j];
            for (uint32_t j = 0; j < n;     j++) merged[total + j] = f[j];
            all   = merged;
            total += n;
        }
    }

    if (total == 0)
        return user_forms;

    Form **combined = (Form **)arena_alloc(arena, (total + nuser) * sizeof(Form *));
    for (uint32_t i = 0; i < total; i++) combined[i]         = all[i];
    for (uint32_t i = 0; i < nuser;  i++) combined[total + i] = user_forms[i];

    *stdlib_count_out = total;
    return combined;
}

/* Compile a .tur file to a C header (.h). Returns 0 on success.
 * J5/J6: forced/n_forced inject ABI clone decls for cross-module borrow specs. */
static int compile_to_h(const char *path, Buf *out_h, const char *module_name,
                        const char **include_dirs, int n_include_dirs,
                        const char **reader_macro_paths,
                        int n_reader_macro_paths,
                        const ForcedAbiSpec *forced, uint32_t n_forced) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    LangLayerSet lang_layers = 0;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj, &lang_layers);

    /* Fresh diagnostic slate per compilation unit -- see compile_to_c.  The
     * project-mode dir build loops compile_to_h / compile_to_implementation
     * in-process, so a stale `had_error_` from an earlier module would
     * otherwise short-circuit every later module's `diag_had_error()` gate. */
    diag_reset();
    experiment_reset_warnings();  /* XF2: once-per-compile TUR-W006x dedup */
    pkg_tur_version_reassert();   /* :tur-version floor survives the reset above */
    pkg_manifest_reassert();      /* a broken build.tur likewise: error: + exit 0 is not an error */
    refine_discharge_reset();     /* RT3: once-per-compile refinement stats */

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
    file.lang_layers = lang_layers;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4: preload spice-manifest reader-macros into the reader registry. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    for (int i = 0; i < n_reader_macro_paths; ++i) {
        if (reader_macros_load_file(&arena, &st,
                                    reader_macro_paths[i],
                                    &reader_macros_reg) != 0) {
            symtab_free(&st); arena_free(&arena); free(src);
            return 1;
        }
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    /* F1: inject macro-only prelude so project-mode files see when/cond/for/min/max. */
    uint32_t stdlib_prefix = 0;
    forms = load_project_prelude(&arena, &st, forms, nforms, &stdlib_prefix);
    nforms += stdlib_prefix;

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        char base_dir[4096];
        dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.stdlib_prefix    = stdlib_prefix;
        ctx.module_base_dir = base_dir;
        ctx.include_dirs    = include_dirs;
        ctx.n_include_dirs  = n_include_dirs;
        ctx.separate_compilation = true;
        ctx.reader_macros   = &reader_macros_reg;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_header(out_h, module_name, ctx.prog, true,
                                   forced, n_forced) != 0) {
            rc = 1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}
/* compile_to_implementation follows. */

/* Compile a .tur file to a C implementation (.c). Returns 0 on success.
 * RP1: when `out_manifest` is non-NULL, append one exports.manifest line
 * per exported defn (see emit.h:emit_exports_manifest). Pass NULL when
 * the caller only wants the .c output.
 * J5/J6: forced/n_forced inject forced ABI clone bodies; out_borrow_specs
 * receives borrow spec info for the cross-module cache (pass NULL to skip). */
static int compile_to_implementation(const char *path, Buf *out_c, const char *module_name,
                                     const char **include_dirs, int n_include_dirs,
                                     const char **reader_macro_paths,
                                     int n_reader_macro_paths,
                                     Buf *out_manifest,
                                     const ForcedAbiSpec *forced, uint32_t n_forced,
                                     BorrowSpecInfo **out_borrow_specs, uint32_t *out_n_borrow_specs) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    /* Detect language and adjust source for #lang directive */
    const char *src_adj = src;
    size_t len_adj = len;
    LangLayerSet lang_layers = 0;
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj, &lang_layers);

    /* Fresh diagnostic slate per compilation unit -- see compile_to_c.  The
     * project-mode dir build loops compile_to_h / compile_to_implementation
     * in-process, so a stale `had_error_` from an earlier module would
     * otherwise short-circuit every later module's `diag_had_error()` gate. */
    diag_reset();
    experiment_reset_warnings();  /* XF2: once-per-compile TUR-W006x dedup */
    pkg_tur_version_reassert();   /* :tur-version floor survives the reset above */
    pkg_manifest_reassert();      /* a broken build.tur likewise: error: + exit 0 is not an error */
    refine_discharge_reset();     /* RT3: once-per-compile refinement stats */

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
    file.lang_layers = lang_layers;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4: preload spice-manifest reader-macros into the reader registry. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    for (int i = 0; i < n_reader_macro_paths; ++i) {
        if (reader_macros_load_file(&arena, &st,
                                    reader_macro_paths[i],
                                    &reader_macros_reg) != 0) {
            symtab_free(&st); arena_free(&arena); free(src);
            return 1;
        }
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    /* F1: inject macro-only prelude so project-mode files see when/cond/for/min/max. */
    uint32_t stdlib_prefix = 0;
    forms = load_project_prelude(&arena, &st, forms, nforms, &stdlib_prefix);
    nforms += stdlib_prefix;

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        char base_dir[4096];
        dir_of_path(path, base_dir, sizeof(base_dir));
        PassContext ctx = {0};
        ctx.arena  = &arena;
        ctx.st     = &st;
        ctx.forms  = forms;
        ctx.nforms = nforms;
        ctx.stdlib_prefix    = stdlib_prefix;
        ctx.module_base_dir = base_dir;
        ctx.include_dirs    = include_dirs;
        ctx.n_include_dirs  = n_include_dirs;
        ctx.separate_compilation = true;
        ctx.reader_macros   = &reader_macros_reg;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_implementation(out_c, module_name, ctx.prog, true,
                                           forced, n_forced,
                                           out_borrow_specs, out_n_borrow_specs) != 0) {
            rc = 1;
        }
        /* RP1: emit_exports_manifest only inspects bindings on `ctx.prog`
         * and is safe to run after emit_implementation (which doesn't
         * mutate the program). Skip on prior error to avoid surfacing
         * incomplete manifest entries. */
        if (rc == 0 && out_manifest
            && emit_exports_manifest(out_manifest, ctx.prog) != 0) {
            rc = 1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Generate _main.c that includes all .h files and has main(). */
static int generate_main_c(Buf *out, const char **h_files, int n_files, const char *output_name) {
    buf_printf(out, "/* generated by tur (phase 2) - _main.c */\n");
    /* Feature-test macros must precede any system #include.  _main.c pulls in
     * <stdio.h> below (and tur_runtime.h transitively), and the first system
     * header locks in <features.h>'s feature set -- so a later
     * `#define _DEFAULT_SOURCE` inside tur_runtime.h arrives too late and
     * leaves POSIX symbols (strdup, clock_gettime/CLOCK_MONOTONIC, nanosleep)
     * unprototyped/undeclared.  Emit it first so it covers every include. */
    buf_puts(out, "#define _DEFAULT_SOURCE 1\n");
    buf_puts(out, "#include <stdio.h>\n");
    buf_puts(out, "#include <stdint.h>\n");
    buf_puts(out, "#include <stdbool.h>\n\n");

    for (int i = 0; i < n_files; i++) {
        buf_printf(out, "#include \"%s\"\n", h_files[i]);
    }
    buf_puts(out, "\n");

    /* For now, just declare main in _main.c if no module has it.
     * In the future, we'll detect which module has main and only include that.
     * For phase 2, we assume the user provides main in one of the modules. */
    buf_puts(out, "/* main() should be defined in one of the included modules */\n");
    return 0;
}

/* UC-3 (user-config-experiments-plan): whether the user-level experiments
 * file has already been consulted this process (or deliberately bypassed, as
 * the LSP hot path does).  The read happens at most once; suppression by a
 * project manifest is decided on that first consult. */
static bool g_user_config_experiments_done = false;

/* Run type-check only on `path`; discard generated C.
 * Used by the LSP server. Must be called with diag_lsp_begin active.
 *
 * SC6: applies the same SC4+SC5 auto-discovery that `tur check` uses,
 * so editor diagnostics in spice files don't show bogus "module not
 * found" errors for intra-spice and cross-spice imports.  --no-auto-spice
 * is honored if the user passed it at the top level. */
int tur_check_only(const char *path) {
    /* LSP/MCP diagnostics run experiment-hot: enable every registered
     * in-flight feature (kind annotations, constrained forall, HKT rank-2,
     * dictionary passing, curried rank-2 results, ...) so the editor sees
     * the same "this parses" answer a developer would get with the matching
     * --enable= flags on the CLI.  The TUR-W006x lifecycle warnings write to
     * stderr, not the JSON diagnostics stream, so they never surface as
     * editor diagnostics. */
    for (size_t xi = 0, xn = experiment_count(); xi < xn; xi++) {
        const ExperimentDescriptor *d = experiment_at(xi);
        if (d) experiment_enable(d->name, XF_SRC_CLI);
    }
    /* UC-3: everything is already on at XF_SRC_CLI above, so the user-level
     * experiments file would be a redundant no-op -- and a malformed one
     * must not crash an editor keystroke.  Mark it consulted so the later
     * discover_manifest_reader_macros call skips the read entirely (and the
     * source column stays 'cli'). */
    g_user_config_experiments_done = true;

    char **inc = NULL;
    int    n_inc = 0;
    char **owned = NULL;
    int    n_owned = 0;
    Ls2ResolverCtx ls2;
    auto_append_spice_includes(path, &inc, &n_inc, &owned, &n_owned, &ls2);

    Buf discard;
    buf_init(&discard);
    int rm_n = 0;
    char **rm_p = discover_manifest_reader_macros(path, &rm_n);
    ls2_resolver_ctx_set(&ls2);
    int rc = compile_to_c(path, &discard, (const char **)inc, n_inc,
                          (const char **)rm_p, rm_n);
    ls2_resolver_ctx_set(NULL);
    ls2_resolver_ctx_dispose(&ls2);
    free_reader_macro_paths(rm_p, rm_n);
    buf_free(&discard);

    for (int i = 0; i < n_owned; i++) free(owned[i]);
    free(owned);
    free(inc);
    return rc;
}

static int cmd_emit_c(const char *path,
                      const char **include_dirs, int n_include_dirs) {
    Buf out;
    buf_init(&out);
    int n_rm = 0;
    char **rm = discover_manifest_reader_macros(path, &n_rm);
    int rc = compile_to_c(path, &out, include_dirs, n_include_dirs,
                          (const char **)rm, n_rm);
    free_reader_macro_paths(rm, n_rm);
    if (rc == 0) buf_to_file(&out, stdout);
    buf_free(&out);
    return rc;
}

/* Phase B: emit per-module .h and .c files to a directory.
 * Usage: tur emit-c --output-dir <dir> <file1.tur> [<file2.tur> ...]
 * Each input produces <dir>/<module>.h and <dir>/<module>.c.
 * J7: uses the same two-pass ABI specialization as cmd_build_multi. */
static int cmd_emit_c_to_dir(const char *out_dir, char **inputs, int n_inputs,
                             const char **include_dirs, int n_include_dirs) {
    if (n_inputs < 1) {
        fprintf(stderr, "tur emit-c --output-dir: at least one input required\n");
        return 1;
    }

    /* Create output directory if it doesn't exist */
    struct stat st_od;
    if (stat(out_dir, &st_od) != 0) {
        if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "tur emit-c: cannot create '%s': %s\n",
                    out_dir, strerror(errno));
            return 2;
        }
    }

    /* Pre-compute module names. */
    char **ecd_mod_names = (char **)malloc(n_inputs * sizeof(char *));
    char **ecd_h_paths   = (char **)malloc(n_inputs * sizeof(char *));
    char **ecd_c_paths   = (char **)malloc(n_inputs * sizeof(char *));
    if (!ecd_mod_names || !ecd_h_paths || !ecd_c_paths) {
        fprintf(stderr, "tur: oom\n"); return 2;
    }
    for (int i = 0; i < n_inputs; i++) {
        const char *base = basename_of(inputs[i]);
        size_t base_len = strlen(base);
        size_t n = (base_len >= 4 && strcmp(base + base_len - 4, ".tur") == 0)
                   ? base_len - 4 : base_len;
        ecd_mod_names[i] = (char *)malloc(n + 1);
        memcpy(ecd_mod_names[i], base, n);
        ecd_mod_names[i][n] = '\0';
        size_t path_sz = strlen(out_dir) + n + 4;
        ecd_h_paths[i] = (char *)malloc(path_sz);
        ecd_c_paths[i] = (char *)malloc(path_sz);
        snprintf(ecd_h_paths[i], path_sz, "%s/%s.h", out_dir, ecd_mod_names[i]);
        snprintf(ecd_c_paths[i], path_sz, "%s/%s.c", out_dir, ecd_mod_names[i]);
    }

    /* J7 Pass 1: compile all with no forced specs; collect borrow specs. */
    BorrowSpecInfo **ecd_borrow = (BorrowSpecInfo **)calloc(n_inputs, sizeof(BorrowSpecInfo *));
    uint32_t *ecd_n_borrow = (uint32_t *)calloc(n_inputs, sizeof(uint32_t));
    if (!ecd_borrow || !ecd_n_borrow) { fprintf(stderr, "tur: oom\n"); return 2; }

    int rc = 0;
    for (int i = 0; i < n_inputs && rc == 0; i++) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(inputs[i], &rm_n);
        Buf h_buf, c_buf;
        buf_init(&h_buf); buf_init(&c_buf);
        int h_rc = compile_to_h(inputs[i], &h_buf, ecd_mod_names[i],
                                include_dirs, n_include_dirs,
                                (const char **)rm_p, rm_n, NULL, 0);
        int c_rc = compile_to_implementation(inputs[i], &c_buf, ecd_mod_names[i],
                                             include_dirs, n_include_dirs,
                                             (const char **)rm_p, rm_n, NULL,
                                             NULL, 0,
                                             &ecd_borrow[i], &ecd_n_borrow[i]);
        free_reader_macro_paths(rm_p, rm_n);
        if (h_rc != 0 || c_rc != 0) {
            rc = h_rc != 0 ? h_rc : c_rc;
        } else if (buf_to_path(&h_buf, ecd_h_paths[i]) != 0 ||
                   buf_to_path(&c_buf, ecd_c_paths[i]) != 0) {
            fprintf(stderr, "tur emit-c: failed to write output for '%s'\n", inputs[i]);
            rc = 2;
        }
        buf_free(&h_buf); buf_free(&c_buf);
    }

    /* J7 Pass 2: recompile owners with forced specs. */
    if (rc == 0) {
        ForcedAbiSpec **ecd_forced = (ForcedAbiSpec **)calloc(n_inputs, sizeof(ForcedAbiSpec *));
        uint32_t *ecd_n_forced = (uint32_t *)calloc(n_inputs, sizeof(uint32_t));
        uint32_t *ecd_cap_forced = (uint32_t *)calloc(n_inputs, sizeof(uint32_t));
        if (!ecd_forced || !ecd_n_forced || !ecd_cap_forced) { fprintf(stderr, "tur: oom\n"); rc = 2; }

        if (rc == 0) {
            for (int i = 0; i < n_inputs; i++) {
                for (uint32_t bi = 0; bi < ecd_n_borrow[i]; bi++) {
                    BorrowSpecInfo *bsi = &ecd_borrow[i][bi];
                    if (!bsi->owning_module || !bsi->fn_symbol) continue;
                    int owner = -1;
                    for (int j = 0; j < n_inputs; j++) {
                        if (strcmp(ecd_mod_names[j], bsi->owning_module) == 0) { owner = j; break; }
                    }
                    if (owner < 0) continue;
                    bool dup = false;
                    for (uint32_t fi = 0; fi < ecd_n_forced[owner]; fi++) {
                        if (strcmp(ecd_forced[owner][fi].clone_name, bsi->clone_name) == 0) { dup = true; break; }
                    }
                    if (dup) continue;
                    if (ecd_n_forced[owner] >= ecd_cap_forced[owner]) {
                        uint32_t nc = ecd_cap_forced[owner] ? ecd_cap_forced[owner] * 2 : 4;
                        ForcedAbiSpec *nf = (ForcedAbiSpec *)realloc(ecd_forced[owner], nc * sizeof(ForcedAbiSpec));
                        if (!nf) { fprintf(stderr, "tur: oom\n"); rc = 2; break; }
                        ecd_forced[owner] = nf; ecd_cap_forced[owner] = nc;
                    }
                    ForcedAbiSpec *fs = &ecd_forced[owner][ecd_n_forced[owner]++];
                    fs->clone_name = bsi->clone_name;
                    fs->fn_symbol  = bsi->fn_symbol;
                    fs->result_kind = bsi->result_kind;
                    fs->n_args = bsi->n_args;
                    for (uint8_t ai = 0; ai < bsi->n_args; ai++) fs->arg_kinds[ai] = bsi->arg_kinds[ai];
                }
            }
            for (int i = 0; i < n_inputs && rc == 0; i++) {
                if (ecd_n_forced[i] == 0) continue;
                int rm_n = 0;
                char **rm_p = discover_manifest_reader_macros(inputs[i], &rm_n);
                Buf h_buf, c_buf;
                buf_init(&h_buf); buf_init(&c_buf);
                int h_rc = compile_to_h(inputs[i], &h_buf, ecd_mod_names[i],
                                        include_dirs, n_include_dirs,
                                        (const char **)rm_p, rm_n,
                                        ecd_forced[i], ecd_n_forced[i]);
                int c_rc = compile_to_implementation(inputs[i], &c_buf, ecd_mod_names[i],
                                                     include_dirs, n_include_dirs,
                                                     (const char **)rm_p, rm_n, NULL,
                                                     ecd_forced[i], ecd_n_forced[i],
                                                     NULL, NULL);
                free_reader_macro_paths(rm_p, rm_n);
                if (h_rc != 0 || c_rc != 0) rc = h_rc != 0 ? h_rc : c_rc;
                else if (buf_to_path(&h_buf, ecd_h_paths[i]) != 0 ||
                         buf_to_path(&c_buf, ecd_c_paths[i]) != 0) rc = 2;
                buf_free(&h_buf); buf_free(&c_buf);
            }
        }
        for (int i = 0; i < n_inputs; i++) free(ecd_forced[i]);
        free(ecd_forced); free(ecd_n_forced); free(ecd_cap_forced);
    }

    /* J5: Write .tur-abi-cache/index for emit-c --output-dir.
     * J6: skipped entirely when the cache is disabled (--no-abi-cache /
     * TUR_NO_ABI_CACHE); the build is still correct without it. */
    if (rc == 0 && !g_no_abi_cache) {
        char cache_dir[4096];
        snprintf(cache_dir, sizeof(cache_dir), "%s/.tur-abi-cache", out_dir);
        struct stat st_cd;
        bool cache_dir_new = (stat(cache_dir, &st_cd) != 0);
        if (cache_dir_new) mkdir(cache_dir, 0755);
        char idx_tmp[4096];
        snprintf(idx_tmp, sizeof(idx_tmp), "%s/index.tmp", cache_dir);
        FILE *cf = fopen(idx_tmp, "w");
        if (cf) {
            for (int i = 0; i < n_inputs; i++) {
                for (uint32_t bi = 0; bi < ecd_n_borrow[i]; bi++) {
                    BorrowSpecInfo *bsi = &ecd_borrow[i][bi];
                    if (!bsi->owning_module || !bsi->fn_symbol) continue;
                    fprintf(cf, "%s\t%s\t%s\t%d\t%d",
                            bsi->clone_name, bsi->owning_module, bsi->fn_symbol,
                            (int)bsi->result_kind, (int)bsi->n_args);
                    for (uint8_t ai = 0; ai < bsi->n_args; ai++)
                        fprintf(cf, "\t%d", (int)bsi->arg_kinds[ai]);
                    fprintf(cf, "\n");
                }
            }
            fclose(cf);
            char idx_path[4096];
            snprintf(idx_path, sizeof(idx_path), "%s/index", cache_dir);
            rename(idx_tmp, idx_path);
        }
    }

    for (int i = 0; i < n_inputs; i++) {
        for (uint32_t bi = 0; bi < ecd_n_borrow[i]; bi++) {
            free(ecd_borrow[i][bi].clone_name);
            free(ecd_borrow[i][bi].owning_module);
            free(ecd_borrow[i][bi].fn_symbol);
        }
        free(ecd_borrow[i]);
        free(ecd_mod_names[i]); free(ecd_h_paths[i]); free(ecd_c_paths[i]);
    }
    free(ecd_borrow); free(ecd_n_borrow);
    free(ecd_mod_names); free(ecd_h_paths); free(ecd_c_paths);
    return rc;
}

/* Phase M3: emit the .h for a single module file to stdout. */
static int cmd_emit_h(const char *path,
                      const char **include_dirs, int n_include_dirs) {
    const char *base = basename_of(path);
    size_t base_len = strlen(base);
    char mod_name[256];
    size_t n = (base_len >= 4 && strcmp(base + base_len - 4, ".tur") == 0)
               ? base_len - 4 : base_len;
    if (n >= sizeof(mod_name)) n = sizeof(mod_name) - 1;
    memcpy(mod_name, base, n);
    mod_name[n] = '\0';

    Buf out;
    buf_init(&out);
    int n_rm = 0;
    char **rm = discover_manifest_reader_macros(path, &n_rm);
    int rc = compile_to_h(path, &out, mod_name, include_dirs, n_include_dirs,
                          (const char **)rm, n_rm, NULL, 0);
    free_reader_macro_paths(rm, n_rm);
    if (rc == 0) buf_to_file(&out, stdout);
    buf_free(&out);
    return rc;
}

/* Choose an output executable name from the input path: foo.tur -> foo.
 *
 * For inputs that resolve to "the current directory" (".", "./", "" after
 * basename), basename_of() returns "." or "" -- which would yield artifact
 * names like "lib..so" downstream. Resolve to an absolute path via realpath()
 * so the cwd's real basename is used instead. */
static void default_output_name(const char *input, char *out, size_t cap) {
    const char *base = basename_of(input);
    char resolved[PATH_MAX];
    /* True when base came from resolving a "current directory" input to its
     * real path: the basename is then a directory name, which has no
     * extension to strip (a "." in "my.project" is part of the name). */
    bool resolved_dir = false;
    if (base[0] == '\0' || (base[0] == '.' && base[1] == '\0')) {
        if (realpath(input && input[0] ? input : ".", resolved)) {
            base = basename_of(resolved);
            if (base[0] == '\0') base = "root";  /* realpath("/") -> "/" */
            resolved_dir = true;
        }
    }
    size_t n = strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    /* Only strip extension if this looks like a file (has a dot that's not at
     * the start) -- never for a resolved directory name. */
    if (!resolved_dir && n > 0 && out[n-1] != '/') {
        char *dot = strrchr(out, '.');
        if (dot && dot != out) { *dot = '\0'; }
    }
}

/* target: NULL for native build, "wasm" to compile with emcc. */
static int cmd_build(const char *input, const char *out_path,
                     const char **include_dirs, int n_include_dirs,
                     const char *target,
                     const char **reader_macro_paths,
                     int n_reader_macro_paths);
static char *find_project_root(const char *start);
static void collect_spice_aux_c(const char *root, Buf *includes, Buf *sources);
/* used-attr-whole-program: collect slash-separated names of -I-reachable
 * modules carrying a #[used] attribute, so cmd_build can force-load them into
 * the whole-program TU (defined after collect_tur_recursive/file_has_used_attr
 * below). Caller frees via free_tur_files. */
static char **collect_used_attr_modules(const char *entry_path,
                                        const char **include_dirs,
                                        int n_include_dirs, int *n_out);
static void free_tur_files(char **files, int n);

/* build-output-directory-plan: ensure_dir(path) creates a directory tree with
 * mkdir -p semantics.  Returns 0 on success (or if the directory already
 * exists), -1 with errno set on failure.  Walks `path` component-wise so a
 * fresh `<root>/build/obj` works even when only `<root>` exists. */
static int ensure_dir(const char *path) {
    if (!path || !*path) return -1;
    char tmp[4096];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, path, n + 1);
    /* Strip trailing slashes (except a bare "/"). */
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = '\0';
    /* Skip leading slash (or "./") so the loop starts on the first real component. */
    char *p = tmp;
    if (*p == '/') p++;
    while (*p) {
        while (*p && *p != '/') p++;
        if (!*p) break;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p++ = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* build-output-directory-plan: resolve the absolute build directory for a
 * compile.  Precedence (highest first):
 *   1. cli_flag (--build-dir / -B / TUR's emit-c --build-dir).
 *   2. TUR_BUILD_DIR env var.
 *   3. The nearest `build.tur`'s `:build-dir` (walking up from input_or_root),
 *      resolved relative to that manifest's directory.
 *   4. `<project-root>/build` when a manifest exists; otherwise `<cwd>/build`.
 *
 * On success returns a heap-allocated absolute path with the directory plus
 * `obj/`, `bin/`, `lib/` subdirs created, and a `.gitignore` containing
 * `*\n` dropped in on first creation.  Returns NULL with a diagnostic on
 * failure.  Caller frees with free(). */
static char *find_spice_root(const char *file_path);   /* defined below */

/* engine-selection-plan E1: resolve the execution engine for `tur run`.
 * Precedence (highest first), mirroring resolve_build_dir's ladder:
 *   1. --engine <name> on the command line.
 *   2. TUR_ENGINE env var.
 *   3. The nearest `build.tur`'s `:engine` (walking up from input_or_root).
 *   4. "cc" -- compile via the C emitter and run the binary (the reference).
 *
 * Returns a static string ("cc" | "jit" | "interp"), or NULL after printing
 * TUR-E0311 for an unknown CLI/env value.  A manifest value is validated at
 * parse time (pkg.c, same code), so an invalid manifest never reaches here.
 * The engines differ in SEMANTICS, not just speed (#?(:tur ... :turi ...),
 * inline-C carve-outs, c2mir divergences), which is why an unknown value is
 * a hard error and never a fallback. */
static const char *resolve_engine(const char *input_or_root,
                                  const char *cli_flag) {
    const char *cand = NULL;
    const char *from = NULL;
    if (cli_flag && *cli_flag) {
        cand = cli_flag;
        from = "--engine";
    } else if (getenv("TUR_ENGINE") && *getenv("TUR_ENGINE")) {
        cand = getenv("TUR_ENGINE");
        from = "TUR_ENGINE";
    }
    if (!cand) {
        const char *start = (input_or_root && *input_or_root) ? input_or_root
                                                              : ".";
        /* find_spice_root handles both file and directory inputs, relative
         * or absolute -- the same walker the RM4 manifest discovery uses. */
        char *root = find_spice_root(start);
        if (root) {
            char mp[4096];
            (void)pkg_resolve_manifest_path(root, mp, sizeof(mp));
            PkgManifest m;
            memset(&m, 0, sizeof(m));
            const char *resolved = NULL;
            if (pkg_manifest_read(mp, &m) && m.engine && *m.engine) {
                if (strcmp(m.engine, "jit") == 0)         resolved = "jit";
                else if (strcmp(m.engine, "interp") == 0) resolved = "interp";
                else                                      resolved = "cc";
            }
            pkg_manifest_free(&m);
            free(root);
            if (resolved) return resolved;
        }
        return "cc";
    }
    if (strcmp(cand, "cc") == 0)     return "cc";
    if (strcmp(cand, "jit") == 0)    return "jit";
    if (strcmp(cand, "interp") == 0) return "interp";
    fprintf(stderr,
            "tur: error TUR-E0311: unknown engine '%s' (from %s); expected "
            "\"cc\", \"jit\", or \"interp\"\n"
            "     precedence: --engine > TUR_ENGINE > build.tur :engine > "
            "\"cc\"; see `tur explain TUR-E0311`\n",
            cand, from);
    return NULL;
}

static char *resolve_build_dir(const char *input_or_root, const char *cli_flag) {
    /* Step 1: pick the raw (unresolved) path string. */
    char raw[4096] = {0};
    char manifest_dir[4096] = {0};

    if (cli_flag && *cli_flag) {
        snprintf(raw, sizeof(raw), "%s", cli_flag);
    } else if (getenv("TUR_BUILD_DIR") && *getenv("TUR_BUILD_DIR")) {
        snprintf(raw, sizeof(raw), "%s", getenv("TUR_BUILD_DIR"));
    } else {
        const char *start = (input_or_root && *input_or_root) ? input_or_root : ".";
        char start_dir[4096];
        struct stat st;
        if (stat(start, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(start_dir, sizeof(start_dir), "%s", start);
        } else {
            dir_of_path(start, start_dir, sizeof(start_dir));
        }
        char *root = find_project_root(start_dir);
        if (root) {
            snprintf(manifest_dir, sizeof(manifest_dir), "%s", root);
            char mp[4096];
            (void)pkg_resolve_manifest_path(root, mp, sizeof(mp));
            PkgManifest m;
            memset(&m, 0, sizeof(m));
            if (pkg_manifest_read(mp, &m) && m.build_dir && *m.build_dir) {
                if (m.build_dir[0] == '/')
                    snprintf(raw, sizeof(raw), "%s", m.build_dir);
                else
                    snprintf(raw, sizeof(raw), "%s/%s", root, m.build_dir);
            } else {
                snprintf(raw, sizeof(raw), "%s/build", root);
            }
            pkg_manifest_free(&m);
            free(root);
        } else {
            snprintf(raw, sizeof(raw), "./build");
        }
    }

    /* Step 2: ensure the directory exists, then canonicalize. */
    if (ensure_dir(raw) != 0) {
        fprintf(stderr, "tur: cannot create build directory '%s': %s\n",
                raw, strerror(errno));
        return NULL;
    }

    char abs[PATH_MAX];
    if (!realpath(raw, abs)) {
        fprintf(stderr, "tur: cannot resolve build directory '%s': %s\n",
                raw, strerror(errno));
        return NULL;
    }

    /* Create obj/, bin/, lib/ subdirs. */
    char sub[PATH_MAX + 16];
    const char *subs[] = {"obj", "bin", "lib"};
    for (size_t i = 0; i < sizeof(subs) / sizeof(*subs); i++) {
        snprintf(sub, sizeof(sub), "%s/%s", abs, subs[i]);
        if (mkdir(sub, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "tur: cannot create '%s': %s\n", sub, strerror(errno));
            return NULL;
        }
    }

    /* Drop a `.gitignore` containing `*` on first creation so the build dir
     * is never accidentally committed even if it gets tracked. Skip if one
     * already exists -- the user may have customized it. */
    char gi[PATH_MAX + 16];
    snprintf(gi, sizeof(gi), "%s/.gitignore", abs);
    struct stat gst;
    if (stat(gi, &gst) != 0) {
        FILE *gf = fopen(gi, "w");
        if (gf) { fprintf(gf, "*\n"); fclose(gf); }
    }

    (void)manifest_dir;  /* reserved for future workspace-member resolution */

    /* Return heap copy. */
    size_t n = strlen(abs);
    char *res = (char *)malloc(n + 1);
    if (!res) return NULL;
    memcpy(res, abs, n + 1);
    return res;
}

/* Scan generated C source for "__tur_include__" comments and prepend each
 * captured line to the top of the buffer so the include lands at file
 * scope. The literal marker is the comment "__tur_include__: LINE" inside
 * a C block comment. Required for headers that declare top-level static
 * inline functions (mbedTLS, anything that uses psa/crypto.h); a plain
 * #include placed inside a function body would put those declarations at
 * block scope, which is not legal C.
 *
 * In-place: replaces *csrc with a new buffer containing
 * [hoisted-includes] + [original csrc]. No-op when no markers exist. */
static void hoist_tur_include_directives(Buf *csrc) {
    /* Buf is not guaranteed NUL-terminated; append a sentinel before
     * any strstr() then drop it back off the logical length when done. */
    buf_putc(csrc, '\0');
    csrc->len--;

    const char *inc_mark = "/* __tur_include__: ";
    size_t inc_mlen = strlen(inc_mark);
    const char *p = csrc->data;
    /* TWO buckets, not one.  `__tur_include__` carries two different kinds of
     * payload -- preprocessor directives (`#include <stdlib.h>`) and file-scope
     * CODE (a `typedef`, or a `static` helper).  Concatenating them in source
     * order lets a code payload land ahead of a directive payload it depends
     * on, which is exactly what stdlib/httpd.tur does: the malloc-using
     * `httpd_conn_own_cstr` is hoisted from line 93 and `#include <stdlib.h>`
     * from line 3916, so the emitted TU called malloc with no declaration in
     * scope.  That compiled only because every cc invocation passed
     * -Wno-error=implicit-function-declaration, and it is UB regardless.
     *
     * So: all directive payloads first (in their own source order, which keeps
     * any feature-test `#define` ahead of the include it conditions), then all
     * code payloads (likewise in source order).  Relative order within each
     * bucket is preserved; only the two kinds are separated.
     * See docs/archive/history/hoisted-inline-c-precedes-includes.md. */
    Buf hdr, code;
    buf_init(&hdr);
    buf_init(&code);
    while (p && (p = strstr(p, inc_mark)) != NULL) {
        p += inc_mlen;
        const char *end = strstr(p, " */");
        if (!end) break;
        const char *q = p;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        int is_directive = (q < end && *q == '#')
            && (strncmp(q, "#include", 8) == 0 || strncmp(q, "#define", 7) == 0
                || strncmp(q, "#undef", 6) == 0 || strncmp(q, "#pragma", 7) == 0);
        Buf *dst = is_directive ? &hdr : &code;
        buf_write(dst, p, (size_t)(end - p));
        buf_putc(dst, '\n');
        p = end + 3;
    }
    if (code.len > 0) {
        buf_write(&hdr, code.data, code.len);
    }
    buf_free(&code);
    if (hdr.len > 0) {
        Buf new_csrc;
        buf_init(&new_csrc);
        /* Feature-test macros must precede any hoisted system #include.  A
         * hoisted header (e.g. <stdint.h> from an autolink __tur_include__)
         * pulls in <features.h>, which locks in the feature set on first
         * inclusion; a later `#define _DEFAULT_SOURCE` in the main preamble
         * then has no effect.  Under -std=c99 (strict ANSI) that leaves POSIX
         * functions like strdup() unprototyped, so the compiler assumes an
         * implicit `int` return and truncates the 64-bit pointer to 32 bits
         * -- corrupting, e.g., httpd request-attribute storage (SEGV on a
         * later strcmp/free).  Emitting the macro here guarantees it is seen
         * before any hoisted include. */
        buf_puts(&new_csrc, "#define _DEFAULT_SOURCE 1\n");
        buf_write(&new_csrc, hdr.data, hdr.len);
        buf_write(&new_csrc, csrc->data, csrc->len);
        buf_free(csrc);
        *csrc = new_csrc;
    }
    buf_free(&hdr);
}

/* T2: resolve the turmeric source root -- the directory that holds both
 * `stdlib/` and `src/runtime/`.  It is the parent of the located stdlib
 * root (`<root>/stdlib` in the dev layout, `<prefix>/share/turmeric/stdlib`
 * when prefix-installed; in both, `src/runtime/` lives beside `stdlib/`).
 * Writes the path into `out` and returns it.  Falls back to "." when the
 * stdlib root could not be resolved to an absolute path (preserving the
 * legacy cwd-relative behavior). */
static const char *resolve_turmeric_root(char *out, size_t cap) {
    const char *sdir = resolve_stdlib_root();  /* e.g. "<root>/stdlib" or "stdlib" */
    snprintf(out, cap, "%s", sdir);
    char *slash = strrchr(out, '/');
    if (slash && slash != out) { *slash = '\0'; return out; }
    if (slash == out) { out[1] = '\0'; return out; }  /* "/stdlib" -> "/" */
    snprintf(out, cap, ".");  /* bare "stdlib" -> cwd-relative, as before */
    return out;
}

/* T2: stdlib `__tur_autolink__` markers embed turmeric-tree-relative paths
 * (e.g. `src/runtime/hamt.c -Isrc/runtime` from stdlib/hamt.tur).  Those
 * only resolve when cwd is the turmeric source root -- true for `tur install`
 * driven dev builds, but NOT for project-mode `tur run` from an arbitrary
 * project directory, where cc fails with `src/runtime/hamt.c: No such file`.
 * Rewrite each whitespace-separated token in `flags` so that relative paths
 * (a bare path, or the argument of `-I` / `-L`) are anchored at `root`.
 * Absolute paths and non-path flags (`-l`, `-f`, `-D`, ...) pass through
 * unchanged.  The rewritten flags are appended to `out` (no trailing NUL). */
static void rewrite_autolink_relative_paths(const char *flags,
                                             const char *root, Buf *out) {
    const char *p = flags;
    bool first = true;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - tok);
        if (!first) buf_putc(out, ' ');
        first = false;
        if (len > 2 && tok[0] == '-' && (tok[1] == 'I' || tok[1] == 'L') &&
            tok[2] != '/') {
            /* -I<rel> / -L<rel>: anchor the path part at root. */
            buf_write(out, tok, 2);
            buf_printf(out, "%s/", root);
            buf_write(out, tok + 2, len - 2);
        } else if (tok[0] != '-' && tok[0] != '/') {
            /* bare relative path (source file or lib): anchor it at root. */
            buf_printf(out, "%s/", root);
            buf_write(out, tok, len);
        } else {
            buf_write(out, tok, len);
        }
    }
}

/* Build a deterministic path for the intermediate generated-C file so that
 * ccache can cache repeated compilations of the same .tur source.
 * Maps the input path to <tmpdir>/tur-build/<sanitized>.c where non-alphanumeric
 * characters (except '-') are replaced with '_'.
 * Creates <tmpdir>/tur-build/ on first call.
 *
 * The directory comes from tur_temp_dir(), not a literal "/tmp": on Windows a
 * leading "/" means the root of the current drive, so "/tmp/tur-build" would
 * point at a C:\tmp that does not exist. */
/* "<tmpdir>/tur-build/" (with trailing separator), created on first use.
 * Exposed so the unlink-vs-keep decision after cc can test membership against
 * the real prefix instead of pattern-matching a hardcoded "/tmp/...". */
static const char *stable_c_prefix(void) {
    static char prefix_buf[512];
    static int  made = 0;
    if (!made) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/tur-build", tur_temp_dir());
        mkdir(dir, 0700);
        snprintf(prefix_buf, sizeof(prefix_buf), "%s/", dir);
        made = 1;
    }
    return prefix_buf;
}

static void stable_c_path(const char *input, char *out, size_t cap) {
    const char *prefix = stable_c_prefix();
    size_t plen = strlen(prefix);
    if (plen + 3 >= cap) { out[0] = '\0'; return; }
    memcpy(out, prefix, plen);
    size_t i = plen;
    for (const char *p = input; *p && i < cap - 3; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-') {
            out[i++] = c;
        } else {
            out[i++] = '_';
        }
    }
    out[i++] = '.';
    out[i++] = 'c';
    out[i] = '\0';
}

/* tur-link-and-build-split-plan Phase 3: scan generated C for
 * `__tur_autolink__` comments and collect the linker flags they embed (e.g.
 * -lturi from stdlib/turi/eval.tur).  The marker format is:
 *   slash-star __tur_autolink__: FLAGS star-slash
 * On return `autolink` holds the raw space-joined flag string (NUL-terminated
 * content when non-empty), before any SDK/ASan/anchor resolution.  Shared by
 * cmd_build and cmd_compile so the two cannot drift. */
static void scan_autolink_markers(const Buf *csrc, Buf *autolink) {
    const char *marker = "/* __tur_autolink__: ";
    size_t mlen = strlen(marker);
    const char *p = csrc->data;
    while (p && (p = strstr(p, marker)) != NULL) {
        p += mlen;
        const char *end = strstr(p, " */");
        if (!end) break;
        if (autolink->len > 0) buf_putc(autolink, ' ');
        buf_write(autolink, p, (size_t)(end - p));
        p = end + 3;
    }
    if (autolink->len > 0) buf_putc(autolink, '\0');
}

/* tur-link-and-build-split-plan Phase 3: collect the enclosing spice's cmake
 * dep flags (-I/-L/-l from cmake/spice-deps-manifest.json) plus its
 * `:c-includes` (-I) and `:c-sources` (vendored .c) by walking up from the
 * input file to the project root.  No-op when the input is not inside a
 * manifested project.  Shared by cmd_build and cmd_compile. */
static void collect_build_aux(const char *input, Buf *cmake_flags,
                              Buf *aux_includes, Buf *aux_sources) {
    /* Walk up from the input file's directory to find project root.  Resolve
     * to an absolute path first -- find_project_root walks via strrchr('/'),
     * so a bare "." or "foo.tur" would stop after one step. */
    char input_dir[4096];
    strncpy(input_dir, input, sizeof(input_dir) - 1);
    input_dir[sizeof(input_dir) - 1] = '\0';
    char *slash = strrchr(input_dir, '/');
    if (slash) *slash = '\0';
    else strncpy(input_dir, ".", sizeof(input_dir));
    char abs_input_dir[4096];
    if (realpath(input_dir, abs_input_dir)) {
        strncpy(input_dir, abs_input_dir, sizeof(input_dir) - 1);
        input_dir[sizeof(input_dir) - 1] = '\0';
    }
    char *proj_root = find_project_root(input_dir);
    if (proj_root) {
        char manifest_path[4096];
        snprintf(manifest_path, sizeof(manifest_path),
                 "%s/cmake/spice-deps-manifest.json", proj_root);
        PkgCmakeManifest cmake_manifest;
        if (pkg_cmake_manifest_read(manifest_path, &cmake_manifest)) {
            pkg_cmake_manifest_append_cc_flags(&cmake_manifest, cmake_flags);
            pkg_cmake_manifest_free(&cmake_manifest);
        }
        /* ffi-spices-integration-plan S1: `:build-opts :link-libs` was
         * parsed into the manifest (pkg.c) and then consumed nowhere --
         * documented, round-tripped by `tur init`, and ignored by every
         * build path.  Append it here, next to the cmake-dep flags, so both
         * consumers of this function (the cc link line via
         * cmd_build/cmd_compile, and the REPL's in-process JIT hook via
         * repl_jit_build) receive it.  Entries are bare lib names
         * (:link-libs ["m"] -> -lm), the same spelling the cmake manifest
         * uses. */
        char bm[4096];
        if (pkg_resolve_manifest_path(proj_root, bm, sizeof(bm))) {
            PkgManifest pm;
            memset(&pm, 0, sizeof(pm));
            if (pkg_manifest_read(bm, &pm)) {
                for (int i = 0; i < pm.n_link_libs; i++) {
                    if (pm.link_libs[i] && pm.link_libs[i][0])
                        buf_printf(cmake_flags, " -l%s", pm.link_libs[i]);
                }
            }
            pkg_manifest_free(&pm);
        }
        collect_spice_aux_c(proj_root, aux_includes, aux_sources);
        free(proj_root);
    }
}

/* tur-link-and-build-split-plan Phase 3: append the -I tokens from a resolved
 * autolink/link flag string to `dst` (each space-prefixed).  The `-c` compile
 * of the generated TU needs the include dirs (e.g. -Isrc/runtime for inline-C
 * referencing runtime headers) but must not receive -l/-L or bare .c sources. */
static void append_include_tokens(Buf *dst, const char *flags) {
    if (!flags) return;
    const char *p = flags;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t tlen = (size_t)(p - start);
        if (tlen >= 2 && start[0] == '-' && start[1] == 'I') {
            buf_putc(dst, ' ');
            buf_write(dst, start, tlen);
        }
    }
}

/* tur-link-and-build-split-plan Phase 2/3c: true when the raw autolink string
 * carries at least one bare `.c` source arg (a runtime TU like
 * src/runtime/hamt.c).  These are exactly what --runtime=lib replaces with a
 * link against the prebuilt libturi.a. */
static int autolink_has_bare_c_source(const Buf *autolink) {
    if (!autolink || autolink->len == 0) return 0;
    const char *p = autolink->data;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        size_t t = (size_t)(p - s);
        if (t > 2 && s[0] != '-' && s[t - 2] == '.' && s[t - 1] == 'c') return 1;
    }
    return 0;
}

/* Probe `dir` for a runtime archive, preferring the lean non-sanitized
 * libturt_runtime.a over the full libturi.a.  On a hit, writes the linker name
 * (`turt_runtime` or `turi`) to `libname` and returns 1. */
static int probe_runtime_lib_in(const char *dir, char *libname, size_t ncap) {
    struct stat st;
    char probe[4096];
    snprintf(probe, sizeof(probe), "%s/libturt_runtime.a", dir);
    if (stat(probe, &st) == 0) { snprintf(libname, ncap, "turt_runtime"); return 1; }
    snprintf(probe, sizeof(probe), "%s/libturi.a", dir);
    if (stat(probe, &st) == 0) { snprintf(libname, ncap, "turi"); return 1; }
    return 0;
}

/* tur-link-and-build-split-plan Phase 2/3c + Phase 6 prereq: locate the runtime
 * archive to link under --runtime=lib.  Prefers the lean, non-sanitized
 * libturt_runtime.a (behaviorally identical to a bare-source recompile) and
 * falls back to the full libturi.a (which is ASan-instrumented in Debug builds).
 * Resolution order for the directory:
 *   1. $TUR_RUNTIME_LIB (an archive file path, or the directory holding one).
 *   2. <exe_dir>/src         (dev layout: build/tur -> build/src).
 *   3. <turmeric_root>/build/src.
 * A prefix-installed SDK's libturi.a is picked up separately by
 * resolve_autolink_flags step 1 once -lturi is on the line.  Returns 1 with the
 * dir in `libdir` and the linker name in `libname` on success, else 0. */
static int locate_runtime_lib(char *libdir, size_t dcap,
                              char *libname, size_t ncap) {
    libdir[0] = '\0';
    libname[0] = '\0';
    struct stat st;
    const char *env = getenv("TUR_RUNTIME_LIB");
    if (env && *env && stat(env, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            if (probe_runtime_lib_in(env, libname, ncap)) {
                snprintf(libdir, dcap, "%s", env);
                return 1;
            }
            return 0;
        }
        /* A file path: derive libname from lib<name>.a and use its dir. */
        char d[4096];
        dir_of_path(env, d, sizeof(d));
        snprintf(libdir, dcap, "%s", d);
        const char *base = strrchr(env, '/');
        base = base ? base + 1 : env;
        if (strncmp(base, "lib", 3) == 0) base += 3;
        snprintf(libname, ncap, "%s", base);
        char *dot = strrchr(libname, '.');
        if (dot) *dot = '\0';
        return libname[0] ? 1 : 0;
    }
    char exe[4096];
    if (get_exe_path(exe, sizeof(exe)) == 0) {
        char d[4096];
        dir_of_path(exe, d, sizeof(d));
        char sd[4200];
        snprintf(sd, sizeof(sd), "%s/src", d);
        if (probe_runtime_lib_in(sd, libname, ncap)) {
            snprintf(libdir, dcap, "%s", sd);
            return 1;
        }
        /* DEDUP-4b: the INSTALLED layout -- <prefix>/bin/tur next to
         * <prefix>/lib/libturt_runtime.a.  Without this probe the archive was
         * only ever findable in a dev build tree, so an installed toolchain
         * silently recompiled the runtime on every build and (since 4b) kept
         * running the emitted GC replica.  Checked after the dev layout so a
         * build tree still wins over a stale system install. */
        snprintf(sd, sizeof(sd), "%s/../lib", d);
        if (probe_runtime_lib_in(sd, libname, ncap)) {
            snprintf(libdir, dcap, "%s", sd);
            return 1;
        }
    }
    char root[4096];
    resolve_turmeric_root(root, sizeof(root));
    if (root[0]) {
        char bd[4200];
        snprintf(bd, sizeof(bd), "%s/build/src", root);
        if (probe_runtime_lib_in(bd, libname, ncap)) {
            snprintf(libdir, dcap, "%s", bd);
            return 1;
        }
    }
    return 0;
}

/* Rebuild `autolink` dropping every bare `.c` source token (a runtime TU),
 * keeping all flags.  Used when --runtime=lib replaces the recompiled sources
 * with a link against a runtime archive.  Appends into a fresh buffer. */
static void autolink_drop_bare_sources(const Buf *autolink, Buf *out) {
    const char *p = autolink->data;
    while (p && *p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        size_t t = (size_t)(p - s);
        int bare_c = t > 2 && s[0] != '-' && s[t - 2] == '.' && s[t - 1] == 'c';
        if (bare_c) continue;
        if (out->len > 0) buf_putc(out, ' ');
        buf_write(out, s, t);
    }
}

/* tur-link-and-build-split-plan Phase 2/3c/6: replace the bare runtime `.c`
 * autolink sources with a link against a prebuilt runtime archive so the
 * runtime TUs (hamt/symbols/tur_string) are linked instead of recompiled per
 * build.  Behavior by mode (see g_runtime_mode):
 *   AUTO (default) -- use the lean non-ASan libturt_runtime.a when locatable;
 *     otherwise leave the bare sources in place (recompile).  Never links the
 *     full libturi.a here, so a default build cannot regress into a link
 *     failure (no archive -> source) or an unexpected ASan/LSan run.
 *   LIB -- force the archive: lean preferred, else the full libturi.a, else a
 *     -lturi fallback (with a warning) that relies on a -L in TUR_CC_FLAGS.
 * No-op when the program autolinks no runtime sources or already links -lturi.
 * `autolink` is consumed and replaced in place. */
static void apply_runtime_lib_mode(Buf *autolink) {
    if (g_runtime_mode == TUR_RT_SOURCE) return;
    if (!autolink_has_bare_c_source(autolink)) return;
    if (autolink->len > 0 && strstr(autolink->data, "-lturi")) return;

    char libdir[4096], libname[128];
    int found = locate_runtime_lib(libdir, sizeof(libdir), libname, sizeof(libname));

    /* AUTO only links the lean, guaranteed-non-ASan archive; anything else
     * (only the full libturi.a, or no archive at all) falls back to recompiling
     * the sources so a default build stays behaviorally identical to source. */
    if (g_runtime_mode == TUR_RT_AUTO &&
        !(found && strcmp(libname, "turt_runtime") == 0))
        return;

    Buf inj;
    buf_init(&inj);
    if (found) {
        buf_printf(&inj, "-l%s -L%s", libname, libdir);
    } else {
        /* Reachable only in explicit LIB mode: rely on a -L in TUR_CC_FLAGS. */
        buf_puts(&inj, "-lturi");
        fprintf(stderr,
                "tur: --runtime=lib could not locate a runtime archive; relying "
                "on a -L in TUR_CC_FLAGS (set TUR_RUNTIME_LIB to override)\n");
    }
    /* Keep every flag, drop the bare .c sources the archive supersedes.  The
     * drop helper inserts the separating space before each kept token. */
    if (autolink->len > 1) autolink_drop_bare_sources(autolink, &inj);
    buf_putc(&inj, '\0');
    buf_free(autolink);
    *autolink = inj;
}

/* tur-link-and-build-split-plan Phase 1: resolve the raw `__tur_autolink__`
 * flag string (as scanned out of the generated C, or read from a `.link`
 * sidecar) into the final link-ready flag set, and report whether the linked
 * libturi was ASan-instrumented.
 *
 * This is the shared link-side flag logic factored out of the old monolithic
 * cmd_build.  It performs, in order:
 *   1. -lturi SDK anchoring -- prepend absolute -I/-L for a prefix-installed SDK.
 *   2. ASan autodetect -- scan libturi.a for __asan_init via nm.
 *   3. Turmeric-tree-relative path anchoring (src/runtime/hamt.c -> abs).
 *   4. -lturi supersedes bare .c source args (drop them; libturi has the objs).
 *
 * `autolink` is consumed and replaced in place: on return it holds the resolved
 * flag string (NUL-terminated content, like the caller's original buffer).
 * `cc_flags` is read for -L paths during the ASan probe.  `*out_needs_asan` is
 * set true when a sanitized libturi.a is on the link line.
 *
 * Behavior is byte-for-byte identical to the inline blocks it replaces; both
 * cmd_build and `tur link` call it so they cannot drift. */
static void resolve_autolink_flags(Buf *autolink, const char *cc_flags,
                                    bool *out_needs_asan) {
    *out_needs_asan = false;

    /* 1. -lturi SDK anchoring. */
    if (autolink->len > 0 && strstr(autolink->data, "-lturi")) {
        char sdk_root[4096] = "";
        const char *sdk_env = getenv("TUR_SDK_ROOT");
        if (sdk_env && *sdk_env) {
            snprintf(sdk_root, sizeof(sdk_root), "%s", sdk_env);
        } else {
            char exe_buf[4096] = "";
            if (get_exe_path(exe_buf, sizeof(exe_buf)) == 0) {
                char dir[4096];
                dir_of_path(exe_buf, dir, sizeof(dir));
                for (int d = 0; d < 8; d++) {
                    char probe[4096];
                    struct stat sdk_st;
                    snprintf(probe, sizeof(probe),
                             "%s/share/turmeric/src/turi/eval.h", dir);
                    if (stat(probe, &sdk_st) == 0 && S_ISREG(sdk_st.st_mode)) {
                        snprintf(sdk_root, sizeof(sdk_root),
                                 "%s/share/turmeric", dir);
                        break;
                    }
                    char *sl = strrchr(dir, '/');
                    if (!sl || sl == dir) break;
                    *sl = '\0';
                }
            }
        }
        if (sdk_root[0]) {
            Buf sdk_flags;
            buf_init(&sdk_flags);
            buf_printf(&sdk_flags, "-I%s/src -I%s/src/compiler -I%s/src/runtime",
                       sdk_root, sdk_root, sdk_root);
            struct stat sdk_lib_st;
            char lib_probe[4096];
            snprintf(lib_probe, sizeof(lib_probe), "%s/lib/libturi.a", sdk_root);
            if (stat(lib_probe, &sdk_lib_st) == 0)
                buf_printf(&sdk_flags, " -L%s/lib", sdk_root);
            snprintf(lib_probe, sizeof(lib_probe), "%s/build/src/libturi.a", sdk_root);
            if (stat(lib_probe, &sdk_lib_st) == 0)
                buf_printf(&sdk_flags, " -L%s/build/src", sdk_root);
            Buf new_al;
            buf_init(&new_al);
            buf_write(&new_al, sdk_flags.data, sdk_flags.len);
            buf_free(&sdk_flags);
            buf_putc(&new_al, ' ');
            if (autolink->len > 1)
                buf_write(&new_al, autolink->data, autolink->len - 1);
            buf_putc(&new_al, '\0');
            buf_free(autolink);
            *autolink = new_al;
        }
    }

    /* 2. ASan autodetect: sanitized libturi.a needs -fsanitize on the link. */
    if (autolink->len > 0 && strstr(autolink->data, "-lturi")) {
        char nm_cmd[512];
        const char *cf = cc_flags;
        while (cf && *cf) {
            const char *lf = strstr(cf, "-L");
            if (!lf) break;
            lf += 2;
            const char *lf_end = lf;
            while (*lf_end && *lf_end != ' ') lf_end++;
            if (lf_end > lf) {
                char lib_path[512];
                size_t plen = (size_t)(lf_end - lf);
                if (plen < sizeof(lib_path) - 20) {
                    memcpy(lib_path, lf, plen);
                    snprintf(lib_path + plen, sizeof(lib_path) - plen, "/libturi.a");
                    snprintf(nm_cmd, sizeof(nm_cmd),
                             "nm %s 2>/dev/null | grep -q __asan_init", lib_path);
                    if (system(nm_cmd) == 0) {
                        *out_needs_asan = true;
                        break;
                    }
                }
            }
            cf = lf_end;
        }
    }

    /* 3. Anchor turmeric-tree-relative autolink paths at the located root. */
    if (autolink->len > 1) {
        char tur_root[4096];
        resolve_turmeric_root(tur_root, sizeof(tur_root));
        if (tur_root[0] && strcmp(tur_root, ".") != 0) {
            Buf rewritten;
            buf_init(&rewritten);
            rewrite_autolink_relative_paths(autolink->data, tur_root, &rewritten);
            buf_putc(&rewritten, '\0');
            buf_free(autolink);
            *autolink = rewritten;
        }
    }

    /* 4. -lturi supersedes bare .c source args; drop them to avoid duplicate
     * symbols (libturi.a already provides every runtime TU's object). */
    if (autolink->len > 1 && strstr(autolink->data, "-lturi")) {
        Buf filtered;
        buf_init(&filtered);
        const char *p = autolink->data;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;
            size_t tlen = (size_t)(p - start);
            bool is_c_source = tlen > 2 && start[0] != '-' &&
                               start[tlen - 2] == '.' && start[tlen - 1] == 'c';
            if (is_c_source) continue;
            if (filtered.len > 0) buf_putc(&filtered, ' ');
            buf_write(&filtered, start, tlen);
        }
        buf_putc(&filtered, '\0');
        buf_free(autolink);
        *autolink = filtered;
    }
}

/* tur-link-and-build-split-plan Phase 1: assemble and run the final link `cc`
 * command.  `inputs` is the space-joined list of primary inputs -- the single
 * generated `.c` for the monolithic path, or one-or-more `.o`/`.c` args for the
 * split/`tur link` path.  Every other argument mirrors what the old inline
 * assembly appended, in the same order, so a monolithic build (inputs = the
 * generated `.c`) produces a byte-identical command.
 *
 * Returns 0 on success, 2 on cc failure or a failed exe settle.  Does not touch
 * the generated-C temp file (the caller owns that lifecycle). */
static int link_command_run(const char *cc, const char *cc_flags,
                            const char *inputs,
                            const Buf *aux_includes, const Buf *aux_sources,
                            const Buf *autolink, bool needs_asan,
                            const Buf *cmake_flags,
                            const char **include_dirs, int n_include_dirs,
                            const char *out_path) {
    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s -o %s %s", cc, cc_flags, out_path, inputs);
    if (aux_includes && aux_includes->len > 0) buf_puts(&cmd, aux_includes->data);
    if (aux_sources  && aux_sources->len  > 0) buf_puts(&cmd, aux_sources->data);
    if (autolink && autolink->len > 0) buf_printf(&cmd, " %s", autolink->data);
    if (needs_asan) buf_puts(&cmd, " -fsanitize=address,undefined");
    if (cmake_flags && cmake_flags->len > 0) buf_puts(&cmd, cmake_flags->data);
    for (int _i = 0; _i < n_include_dirs; _i++) {
        if (include_dirs[_i] && include_dirs[_i][0])
            buf_printf(&cmd, " -I%s", include_dirs[_i]);
    }
    buf_puts(&cmd, " -lm");
#ifdef _WIN32
    buf_puts(&cmd, " -lpthread -lws2_32 -lshlwapi");
#endif
    buf_putc(&cmd, '\0');
    int sys_rc = system(cmd.data);
    buf_free(&cmd);

    if (sys_rc == 0 && tur_settle_exe_output(out_path) != 0) {
        fprintf(stderr, "tur: could not place the linked binary at '%s'\n", out_path);
        return 2;
    }
    if (sys_rc != 0) {
        fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }
    return 0;
}

static int cmd_build(const char *input, const char *out_path,
                     const char **include_dirs, int n_include_dirs,
                     const char *target,
                     const char **reader_macro_paths,
                     int n_reader_macro_paths) {
    /* RT3: reset per-compile refinement state, like the check/run/emit-c entry
     * points. The memo caches VC pointers into the per-compile arena; a process
     * that builds >1 file (`tur test`, LSP) otherwise keeps stale pointers and
     * memo_lookup dereferences them on a fingerprint collision. Partial fix for
     * the strict-refine multi-compile crash (see docs/reported). */
    refine_discharge_reset();
    /* DEDUP-4b: `tur` links this output, so the rc<T>/GC runtime may come
     * from the archive rather than being replicated into the preamble. */
    g_emit_for_link = true;
    Buf csrc;
    buf_init(&csrc);
    /* used-attr-whole-program: this single-file/whole-program path inlines only
     * the entry's Turmeric import closure, so a #[used] defn in a sibling
     * module reached only via a raw mangled C symbol (no `(import)`) would be
     * dropped and dangle at link time -- unlike `tur build <project>`, which
     * falls back to separate compilation.  Scan the -I search dirs for such
     * modules and publish them so elaborate_program force-loads (and thus
     * emits) them.  Scoped to the build path (not check/emit-c) so inspection
     * output and snapshots are unaffected. */
    int n_used_mods = 0;
    char **used_mods = collect_used_attr_modules(input, include_dirs,
                                                 n_include_dirs, &n_used_mods);
    UsedModulesCtx used_ctx = { (const char **)used_mods, n_used_mods };
    if (n_used_mods > 0) used_modules_ctx_set(&used_ctx);
    int rc = compile_to_c(input, &csrc, include_dirs, n_include_dirs,
                          reader_macro_paths, n_reader_macro_paths);
    if (n_used_mods > 0) used_modules_ctx_set(NULL);
    free_tur_files(used_mods, n_used_mods);
    if (rc != 0) { buf_free(&csrc); return rc; }

    /* Write generated C to a deterministic path so ccache can cache the result
     * across repeated builds of the same .tur file.  Fall back to a random
     * temp if the stable path cannot be constructed (e.g. path too long). */
    char tmpl[1024];
    stable_c_path(input, tmpl, sizeof(tmpl));
    FILE *tf = tmpl[0] ? fopen(tmpl, "wb") : NULL;
    if (!tf) {
        /* fallback: random temp */
        char fallback[512];
        snprintf(fallback, sizeof(fallback), "%s/tur-XXXXXX.c", tur_temp_dir());
        int fd = mkstemps(fallback, 2);
        if (fd < 0) {
            fprintf(stderr, "tur: cannot create temp file\n");
            buf_free(&csrc);
            return 2;
        }
        tf = fdopen(fd, "wb");
        memcpy(tmpl, fallback, sizeof(fallback));
    }
    /* Phase S2: scan for __tur_include__ directives and inject them at the
     * top of the generated C so stdlib modules can add file-level includes. */
    hoist_tur_include_directives(&csrc);

    if (!tf || fwrite(csrc.data, 1, csrc.len, tf) != csrc.len) {
        fprintf(stderr, "tur: write failed\n");
        if (tf) fclose(tf);
        buf_free(&csrc);
        return 2;
    }
    fclose(tf);

    /* Phase S2 / tur-link-and-build-split-plan Phase 3: scan the generated C
     * for __tur_autolink__ comments (shared helper). */
    Buf autolink;
    buf_init(&autolink);
    scan_autolink_markers(&csrc, &autolink);
    buf_free(&csrc);

    bool wasm_target = target && strcmp(target, "wasm") == 0;

    char chosen_out[1024];
    if (!out_path) {
        default_output_name(input, chosen_out, sizeof(chosen_out));
        /* emcc outputs <name>.js + <name>.wasm; use .js as the primary output */
        if (wasm_target) {
            size_t n = strlen(chosen_out);
            if (n + 3 < sizeof(chosen_out)) { chosen_out[n++] = '.'; chosen_out[n++] = 'j'; chosen_out[n++] = 's'; chosen_out[n] = '\0'; }
        }
        out_path = chosen_out;
    }

    const char *cc;
    if (wasm_target) {
        cc = "emcc";
    } else {
        cc = getenv("CC");
        if (!cc || !*cc) cc = "cc";
    }

    /* TUR_CC_FLAGS overrides the default compiler flags.  Useful for test runs
     * where -O0 is fast enough and ccache benefits from consistent flags.
     * -fno-strict-aliasing: emitted code and inline C blocks routinely pun
     * pointers through int64_t, which GCC's TBAA miscompiles at -O2. */
    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags) {
        if (wasm_target)
            cc_flags = "-O2 -std=c99 -Wall -fno-strict-aliasing -s WASM=1";
        else if (g_emit_debug_lines)
            /* Debugger Phase 4 (--debug): -g for DWARF, -Og for a debugging-
             * friendly optimization level that still maps cleanly onto the
             * `#line`-anchored `.tur` source.  -O0 is NOT used: a self-contained
             * single-file build relies on the optimizer's dead-code elimination
             * to drop preloaded stdlib defns (e.g. contract handlers) that
             * reference libturi-only symbols this build does not link; -O0 keeps
             * them and the link fails.  -Og keeps that DCE while preserving
             * line-accurate stepping. */
            cc_flags = "-g -Og -std=c99 -Wall -fno-strict-aliasing";
        else
            cc_flags = "-O2 -std=c99 -Wall -fno-strict-aliasing";
    }

    /* Collect cmake dep flags from cmake/spice-deps-manifest.json if present */
    Buf cmake_flags;
    buf_init(&cmake_flags);
    /* spices-c-sources-plan: the enclosing spice's :c-includes (as -I, visible
     * to inline-C in the generated TU) and :c-sources (vendored .c, compiled
     * as extra translation units and linked in). Resolved relative to the
     * manifest dir found by walking up from the input file. */
    Buf aux_includes; buf_init(&aux_includes);
    Buf aux_sources;  buf_init(&aux_sources);
    collect_build_aux(input, &cmake_flags, &aux_includes, &aux_sources);

    /* tur-link-and-build-split-plan Phase 2/3c: under --runtime=lib, swap bare
     * runtime .c autolink sources for a link against the prebuilt libturi.a
     * (native builds only -- emcc/wasm has no libturi.a). */
    if (!wasm_target) apply_runtime_lib_mode(&autolink);

    /* tur-link-and-build-split-plan Phase 1: resolve the raw autolink flags
     * (SDK anchoring, ASan autodetect, tree-relative path anchoring, and the
     * -lturi bare-.c filter) via the shared helper -- the same resolution
     * `tur link` runs on flags read from a `.link` sidecar. */
    bool autolink_needs_asan = false;
    resolve_autolink_flags(&autolink, cc_flags, &autolink_needs_asan);

    /* tur-link-and-build-split-plan Phase 1: assemble + run the link `cc`
     * command via the shared helper.  Inputs is the single generated `.c`, so
     * this is the byte-identical monolithic compile+link -- the same helper
     * `tur link` calls with `.o` inputs. */
    int link_rc = link_command_run(cc, cc_flags, tmpl, &aux_includes,
                                   &aux_sources, &autolink, autolink_needs_asan,
                                   &cmake_flags, include_dirs, n_include_dirs,
                                   out_path);
    buf_free(&aux_includes);
    buf_free(&aux_sources);
    buf_free(&autolink);
    buf_free(&cmake_flags);
    /* Leave the stable temp file for ccache; only unlink random fallbacks.
     * Tested against the real prefix rather than a literal "/tmp/tur-build/":
     * on Windows the path starts with a drive letter, so the old check was
     * never true there and every fallback temp file would have been left
     * behind. */
    {
        const char *sp = stable_c_prefix();
        if (tmpl[0] && strncmp(tmpl, sp, strlen(sp)) != 0) {
            unlink(tmpl);
        }
    }
    return link_rc;
}

/* Walk up from 'start' to find a directory containing build.tur.
 * Returns malloc'd path string or NULL if not found. Caller must free(). */
/* RM4: collect the manifest's `:reader-macros [...]` entries as absolute
 * paths suitable for compile_to_c. `root_dir` is the spice root (where
 * build.tur lives); each manifest entry is joined onto it unless it is
 * already absolute. Returns a heap-allocated `char **` (each element also
 * heap-allocated) and writes the count to `*n_out`. Caller frees with
 * `free_reader_macro_paths`. Returns NULL with `*n_out = 0` if the
 * manifest has no entries. */
static char **resolve_manifest_reader_macros(const char *root_dir,
                                             const PkgManifest *m,
                                             int *n_out) {
    *n_out = 0;
    if (!m || m->n_reader_macros == 0) return NULL;
    char **out = (char **)malloc(sizeof(char *) * (size_t)m->n_reader_macros);
    if (!out) return NULL;
    for (int i = 0; i < m->n_reader_macros; ++i) {
        const char *rel = m->reader_macros[i];
        char buf[4096];
        if (rel[0] == '/') {
            snprintf(buf, sizeof(buf), "%s", rel);
        } else {
            snprintf(buf, sizeof(buf), "%s/%s", root_dir, rel);
        }
        out[i] = (char *)malloc(strlen(buf) + 1);
        if (!out[i]) {
            for (int j = 0; j < i; ++j) free(out[j]);
            free(out);
            return NULL;
        }
        strcpy(out[i], buf);
    }
    *n_out = m->n_reader_macros;
    return out;
}

static void free_reader_macro_paths(char **paths, int n) {
    if (!paths) return;
    for (int i = 0; i < n; ++i) free(paths[i]);
    free(paths);
}

/* RM4 follow-up: convenience wrapper around find_spice_root +
 * pkg_manifest_read + resolve_manifest_reader_macros. Walks up from
 * `input_path` to find an enclosing build.tur, then returns the resolved
 * absolute reader-macro paths. Returns NULL/0 when there is no manifest
 * or no entries. Caller frees with free_reader_macro_paths. Used by
 * every compile entry point that takes a single input file path. */
static char **discover_manifest_reader_macros(const char *input_path,
                                              int *n_out);
/* Defined below find_spice_root, which it depends on. */

static char *find_project_root(const char *start) {
    char dir[4096];
    strncpy(dir, start, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    for (;;) {
        char candidate[4096];
        if (pkg_resolve_manifest_path(dir, candidate, sizeof(candidate))) {
            char *res = (char *)malloc(strlen(dir) + 1);
            if (res) strcpy(res, dir);
            return res;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return NULL;
}

/* SC3: maximum number of parent directories find_spice_root walks before
 * giving up.  The deepest known intra-spice file is `<root>/src/<mod>/x.tur`
 * (3 levels above the spice root); 16 leaves ample headroom for worktrees,
 * nested temp checkouts, and `node_modules`-style nesting without ever
 * climbing all the way to `/` on a filesystem that has no `build.tur`. */
#define TUR_SPICE_WALK_MAX 16

/* SC3: walk up from `file_path`'s directory looking for a sibling
 * `build.tur`.  Returns a heap-allocated absolute path to the directory
 * containing the manifest (the spice root), or NULL if no build.tur is
 * found within TUR_SPICE_WALK_MAX steps.
 *
 * The plan calls for an absolute result so callers don't have to worry
 * about cwd drift between resolution and use.  We canonicalize the
 * starting directory via realpath() when possible; on failure we fall
 * back to a cwd-prefixed path so a relative input file still resolves
 * predictably. */
static char *find_spice_root(const char *file_path) {
    if (!file_path) return NULL;

    char raw_dir[4096];
    dir_of_path(file_path, raw_dir, sizeof(raw_dir));

    /* Canonicalize.  realpath() requires the path to exist; for a file
     * the caller is about to read, the directory always exists, so this
     * should generally succeed.  If it fails (e.g. permissions), fall
     * back to cwd-prefixing for a relative path. */
    char dir[4096];
    if (realpath(raw_dir, dir) == NULL) {
        if (raw_dir[0] == '/') {
            strncpy(dir, raw_dir, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
        } else {
            char cwd[4096];
            if (!getcwd(cwd, sizeof(cwd))) return NULL;
            int n;
            if (raw_dir[0] == '.' && raw_dir[1] == '\0') {
                n = snprintf(dir, sizeof(dir), "%s", cwd);
            } else {
                n = snprintf(dir, sizeof(dir), "%s/%s", cwd, raw_dir);
            }
            if (n < 0 || (size_t)n >= sizeof(dir)) return NULL;
        }
    }

    for (int steps = 0; steps < TUR_SPICE_WALK_MAX; steps++) {
        char candidate[4096];
        if (pkg_resolve_manifest_path(dir, candidate, sizeof(candidate))) {
            size_t dl = strlen(dir);
            char *res = (char *)malloc(dl + 1);
            if (res) memcpy(res, dir, dl + 1);
            return res;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return NULL;
}

/* XF1 (experimental-flag-mechanism-plan): merge an enclosing spice's
 * :experiments [...] into the active experiment set.  CLI --enable= has
 * already run (and wins on conflict).  An unknown name in the manifest is a
 * hard configuration error: emit TUR-E0310 and abort, the same "typos surface
 * immediately" contract the CLI path enforces. */
static void apply_manifest_experiments(const PkgManifest *m) {
    /* lang-layers L4: a manifest that states its own :experiments list (even
     * the empty one) has SCOPED the experiment set for this project.  A file
     * that then asks for a semantic `#lang` layer whose experiment is not in
     * that list is a hard error rather than a silent ignore -- see
     * lang_layers_apply_semantic. */
    if (m->has_experiments_key) g_manifest_experiments_scoped = true;
    for (int i = 0; i < m->n_experiments; i++) {
        if (!experiment_enable(m->experiments[i], XF_SRC_MANIFEST)) {
            fprintf(stderr,
                    "error [TUR-E0310]: unknown experiment '%s' in build.tur "
                    ":experiments; run 'tur experiments' for the list\n",
                    m->experiments[i]);
            exit(2);
        }
    }
}

/* UC-3 (user-config-experiments-plan): consult the user-level experiments
 * file ($XDG_CONFIG_HOME/turmeric/experiments.tur) at most once per process.
 * Suppressed entirely when the current compile runs inside a manifested
 * project that declared its own :experiments list -- even the empty
 * `:experiments []` (Goal 2): the project owner's stated intent governs, and
 * user preferences do not silently union in.  `m_or_null` is the resolved
 * project manifest, or NULL for a scratch file / non-project invocation.
 * CLI --enable= (applied earlier at XF_SRC_CLI) still wins over whatever this
 * turns on, because a higher-numbered source always beats a lower one. */
static void apply_user_config_experiments(const PkgManifest *m_or_null) {
    if (g_user_config_experiments_done) return;
    g_user_config_experiments_done = true;
    if (m_or_null && m_or_null->has_experiments_key) return;  /* suppressed */
    experiments_read_user_config();
}

static char **discover_manifest_reader_macros(const char *input_path,
                                              int *n_out) {
    *n_out = 0;
    if (!input_path) return NULL;
    char *sroot = find_spice_root(input_path);
    if (!sroot) {
        /* No enclosing project: the user-level experiments file applies
         * unconditionally (nothing to suppress it). */
        apply_user_config_experiments(NULL);
        return NULL;
    }
    char mp[4096];
    if (!pkg_resolve_manifest_path(sroot, mp, sizeof(mp))) {
        apply_user_config_experiments(NULL);
        free(sroot);
        return NULL;
    }
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    char **out = NULL;
    if (pkg_manifest_read(mp, &m)) {
        /* User file first (suppressed if the manifest declares :experiments),
         * then the manifest's own list, then -- earlier -- CLI --enable=.
         * Precedence is by source rank, so the read order does not matter for
         * the source column; we read user-config first only to skip it when
         * the manifest carries the key. */
        apply_user_config_experiments(&m);
        apply_manifest_experiments(&m);
        out = resolve_manifest_reader_macros(sroot, &m, n_out);
    } else {
        /* Manifest present but unreadable -- treat as no project key. */
        apply_user_config_experiments(NULL);
    }
    pkg_manifest_free(&m);
    free(sroot);
    return out;
}

/* Append `path` (taken by ownership of a strdup'd copy of `s`) to both
 * the active include list and the owned-pointers ledger.  The owned
 * ledger lets the caller free each strdup'd path after the compile
 * completes.  Returns 0 on success, -1 on allocation failure (no
 * partial state -- both arrays untouched). */
static int append_inc_owned(const char *s,
                            char ***inc, int *n_inc,
                            char ***owned, int *n_owned) {
    char *copy = strdup(s);
    if (!copy) return -1;
    char **bigger_inc = (char **)realloc(*inc, (size_t)(*n_inc + 1) * sizeof(char *));
    if (!bigger_inc) { free(copy); return -1; }
    *inc = bigger_inc;
    char **bigger_own = (char **)realloc(*owned, (size_t)(*n_owned + 1) * sizeof(char *));
    if (!bigger_own) {
        /* Roll back the inc realloc growth so caller's view is consistent. */
        free(copy);
        return -1;
    }
    *owned = bigger_own;
    (*inc)[(*n_inc)++]   = copy;
    (*owned)[(*n_owned)++] = copy;
    return 0;
}

/* SC4+SC5: auto-discover the enclosing spice and append both its own
 * `src/` (SC4) and every `:spices` dep's `src/` (SC5) to the include
 * list.  Each appended path is strdup'd; the returned `*owned` array
 * (size in `*n_owned`) is the strdup'd pointers in insertion order so
 * the caller can free them after the compile call.
 *
 * Order matters: explicit `-I` flags from the user are already at the
 * front of `*inc`, so the elaborator's first-match-wins behavior keeps
 * them as the highest priority.  Within the auto-appended block, the
 * enclosing spice's own src/ comes before fetched-dep src/ dirs (so a
 * local module shadows a vendored one of the same name).
 *
 * Skipped (returns 0 with no entries) when:
 *   - g_no_auto_spice is set, OR
 *   - no build.tur is found within TUR_SPICE_WALK_MAX ancestors, OR
 *   - both checks (own src/, manifest deps) yield nothing.
 *
 * The caller must own/free the returned pointer array even when empty:
 *
 *     char **owned = NULL;
 *     int    n_owned = 0;
 *     auto_append_spice_includes(input, &inc, &n_inc, &owned, &n_owned);
 *     // ...compile...
 *     for (int i = 0; i < n_owned; i++) free(owned[i]);
 *     free(owned);
 */
/* LS2: grow the producer-per-include parallel array by one entry, copying
 * `producer` (may be NULL) onto the end. Length is tracked via *n_inc by
 * the caller; this is called immediately after a successful
 * append_inc_owned to keep the arrays aligned. */
static int ls2_push_producer(Ls2ResolverCtx *ls2, int new_n_inc,
                             const char *producer) {
    if (!ls2) return 0;
    const char **bigger = (const char **)realloc(
        (void *)ls2->producer_per_inc, (size_t)new_n_inc * sizeof(char *));
    if (!bigger) return -1;
    ls2->producer_per_inc = bigger;
    /* The caller passes new_n_inc = old_n_inc + 1, so the new slot is at
     * new_n_inc - 1.  Any pre-existing slots stay as they were. */
    char *copy = producer ? strdup(producer) : NULL;
    if (producer && !copy) return -1;
    bigger[new_n_inc - 1] = copy;
    return 0;
}

/* LS2: when the LS2 context is active, initialize the leading slots of
 * producer_per_inc to NULL so the per-include indices line up.  Called
 * once at the top of auto_append_spice_includes with the count of -I
 * entries the caller already prepended. */
static int ls2_prime_producer_array(Ls2ResolverCtx *ls2, int n_existing) {
    if (!ls2 || n_existing <= 0) return 0;
    const char **arr = (const char **)calloc((size_t)n_existing, sizeof(char *));
    if (!arr) return -1;
    ls2->producer_per_inc = arr;
    return 0;
}

static int auto_append_spice_includes(const char *input,
                                      char ***inc, int *n_inc,
                                      char ***owned, int *n_owned,
                                      Ls2ResolverCtx *out_ls2) {
    *owned = NULL;
    *n_owned = 0;
    if (out_ls2) {
        memset(out_ls2, 0, sizeof(*out_ls2));
        const char *dbg = getenv("TUR_DEBUG_RESOLVER");
        out_ls2->debug_resolver = (dbg && dbg[0] == '1');
        /* Pre-existing entries are user-supplied -I flags; mark them with
         * NULL producer so the parallel array aligns with the final
         * include list. */
        if (ls2_prime_producer_array(out_ls2, *n_inc) != 0) return -1;
    }
    if (g_no_auto_spice || !input) return 0;

    char *root = find_spice_root(input);
    if (!root) return 0;

    /* SC4: own src/ (always preferred over deps on name collision). */
    char own_src[4096];
    int n = snprintf(own_src, sizeof(own_src), "%s/src", root);
    if (n > 0 && (size_t)n < sizeof(own_src)) {
        struct stat st;
        if (stat(own_src, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (append_inc_owned(own_src, inc, n_inc, owned, n_owned) == 0)
                (void)ls2_push_producer(out_ls2, *n_inc, NULL);
        }
    }

    /* SC5: parse the manifest and append every fetched `:spices` dep's
     * src/.  Layout mirrors the convention used by cmd_run's project
     * mode: spices/<name>-<ref>/src/  (preferred), or spices/<name>/src/
     * for unversioned, or <root>/<s->path>/src/ for local-path deps.
     * If a `:subdir` is set (monorepo sub-package), descend into it
     * first, then look for src/.  If no src/ exists, fall back to the
     * dep dir itself so the user gets *some* search path. */
    char manifest_path[4096];
    if (pkg_resolve_manifest_path(root, manifest_path, sizeof(manifest_path))) {
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (pkg_manifest_read(manifest_path, &m)) {
            char spices_dir[4096];
            snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                char dep_dir[4096];
                if (s->path) {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root, s->path);
                } else if (s->ref) {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                } else {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                }
                if (s->subdir) {
                    char joined[4096];
                    snprintf(joined, sizeof(joined), "%s/%s", dep_dir, s->subdir);
                    strncpy(dep_dir, joined, sizeof(dep_dir) - 1);
                    dep_dir[sizeof(dep_dir) - 1] = '\0';
                }
                char dep_src[4096];
                snprintf(dep_src, sizeof(dep_src), "%s/src", dep_dir);
                struct stat ss;
                const char *chosen = (stat(dep_src, &ss) == 0 && S_ISDIR(ss.st_mode))
                                     ? dep_src : dep_dir;
                /* Only add the path if it actually exists on disk.  A
                 * missing fetched dep (offline run, etc.) shouldn't
                 * pollute the include path with bogus dirs. */
                if (stat(chosen, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                    if (append_inc_owned(chosen, inc, n_inc, owned, n_owned) == 0)
                        (void)ls2_push_producer(out_ls2, *n_inc, NULL);
                }
            }
            /* LS2: snapshot the consumer's declared :spices names so the
             * elaborator can suppress the warning when a workspace
             * sibling is already redeclared via :spices. */
            if (out_ls2 && m.n_spices > 0) {
                out_ls2->declared_spices =
                    (const char **)calloc((size_t)m.n_spices, sizeof(char *));
                if (out_ls2->declared_spices) {
                    for (int i = 0; i < m.n_spices; i++) {
                        out_ls2->declared_spices[i] = strdup(m.spices[i].name);
                    }
                    out_ls2->n_declared_spices = m.n_spices;
                }
            }
            pkg_manifest_free(&m);
        }
    }

    /* LS2 (local-spice-dev-workflow-plan): workspace member auto-resolution.
     *
     * Walk ancestors of `root` looking for any directory containing a
     * `build.tur` whose `:members [...]` list names our spice (matched by
     * comparing the resolved absolute path of `<workspace>/<member>` to
     * `root`).  When found, add every *other* member's `src/` to the
     * include path.  Workspace membership is the consent boundary:
     * sibling members can import each other without an explicit :spices
     * entry; external publication still uses URL deps.
     *
     * Optional one-time warning when an undeclared sibling is consulted is
     * a future extension (see plan §2); resolution itself is the load-
     * bearing change.
     */
    {
        char anc[4096];
        size_t rlen = strlen(root);
        if (rlen + 1 < sizeof(anc)) {
            memcpy(anc, root, rlen + 1);
            for (int up = 0; up < TUR_SPICE_WALK_MAX; up++) {
                char *last = strrchr(anc, '/');
                if (!last || last == anc) break;
                *last = '\0';

                char ws_manifest[4096];
                if (!pkg_resolve_manifest_path(anc, ws_manifest,
                                               sizeof(ws_manifest)))
                    continue;

                PkgManifest wm;
                memset(&wm, 0, sizeof(wm));
                bool ok = pkg_manifest_read(ws_manifest, &wm);
                if (ok && wm.n_members > 0) {
                    /* Self-detection: locate the member entry whose
                     * absolute path matches `root`.  Only proceed if the
                     * current spice is itself a listed member of this
                     * candidate workspace. */
                    const char *self_member_path = NULL;
                    for (int i = 0; i < wm.n_members; i++) {
                        char mp[4096];
                        int mn = snprintf(mp, sizeof(mp), "%s/%s",
                                          anc, wm.members[i]);
                        if (mn <= 0 || (size_t)mn >= sizeof(mp)) continue;
                        char real_mp[4096];
                        if (realpath(mp, real_mp) == NULL) continue;
                        if (strcmp(real_mp, root) == 0) {
                            self_member_path = wm.members[i];
                            break;
                        }
                    }
                    if (self_member_path) {
                        for (int i = 0; i < wm.n_members; i++) {
                            if (strcmp(wm.members[i], self_member_path) == 0)
                                continue;
                            char sib_src[4096];
                            int sn = snprintf(sib_src, sizeof(sib_src),
                                              "%s/%s/src",
                                              anc, wm.members[i]);
                            if (sn <= 0 || (size_t)sn >= sizeof(sib_src))
                                continue;
                            struct stat sst;
                            if (stat(sib_src, &sst) == 0
                                && S_ISDIR(sst.st_mode)) {
                                if (append_inc_owned(sib_src,
                                                     inc, n_inc,
                                                     owned, n_owned) == 0) {
                                    (void)ls2_push_producer(out_ls2, *n_inc,
                                                            wm.members[i]);
                                    if (out_ls2 && out_ls2->debug_resolver) {
                                        fprintf(stderr,
                                            "tur: resolver: added workspace "
                                            "sibling '%s' src/ -> %s\n",
                                            wm.members[i], sib_src);
                                    }
                                }
                            }
                        }
                        pkg_manifest_free(&wm);
                        break;
                    }
                }
                pkg_manifest_free(&wm);
                /* Any build.tur (workspace or not) terminates the walk so
                 * we don't accidentally treat a non-workspace ancestor's
                 * project as the enclosing workspace. */
                if (ok) break;
            }
        }
    }

    free(root);
    /* LS2: finalize ctx — set length to current *n_inc and allocate the
     * per-include warned[] dedup array so the elaborator can mark slots
     * after firing the first warning. */
    if (out_ls2) {
        out_ls2->n_inc = *n_inc;
        if (*n_inc > 0) {
            out_ls2->warned_per_inc =
                (bool *)calloc((size_t)*n_inc, sizeof(bool));
            if (!out_ls2->warned_per_inc) return -1;
        }
    }
    return 0;
}

/* LS2: free the parallel arrays owned by an Ls2ResolverCtx populated by
 * auto_append_spice_includes. Idempotent; safe to call on a zero-init
 * struct. */
static void ls2_resolver_ctx_dispose(Ls2ResolverCtx *ctx) {
    if (!ctx) return;
    if (ctx->producer_per_inc) {
        for (int i = 0; i < ctx->n_inc; i++) {
            if (ctx->producer_per_inc[i])
                free((void *)ctx->producer_per_inc[i]);
        }
        free((void *)ctx->producer_per_inc);
    }
    free(ctx->warned_per_inc);
    if (ctx->declared_spices) {
        for (int i = 0; i < ctx->n_declared_spices; i++) {
            if (ctx->declared_spices[i])
                free((void *)ctx->declared_spices[i]);
        }
        free((void *)ctx->declared_spices);
    }
    memset(ctx, 0, sizeof(*ctx));
}

/* Collect all .tur files in a directory. Returns malloc'd array, sets *n_out. */
static char **collect_tur_files(const char *dir, int *n_out) {
    DIR *d = opendir(dir);
    if (!d) return NULL;

    char **files = NULL;
    int cap = 0;
    int n = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!tur_dirent_is_reg(dir, ent)) continue;
        size_t len = strlen(ent->d_name);
        if (len >= 4 && strcmp(ent->d_name + len - 4, ".tur") == 0) {
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                files = (char **)realloc(files, cap * sizeof(char *));
            }
            files[n] = (char *)malloc(strlen(dir) + 1 + len + 1);
            snprintf(files[n], strlen(dir) + 1 + len + 1, "%s/%s", dir, ent->d_name);
            n++;
        }
    }
    closedir(d);
    *n_out = n;
    return files;
}

/* Free the array returned by collect_tur_files. */
static void free_tur_files(char **files, int n) {
    for (int i = 0; i < n; i++) free(files[i]);
    free(files);
}

static int compare_cstr_ptrs(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

/* Recursively collect `*.tur` files under `dir` into a growing array.
 * Skips the package manifest (`build.tur`) and any dotfile entry (so
 * `.git/`, `.tur-cache/`, `.tur-abi-cache/`, etc. are not descended into).
 * Paths are heap-allocated; the array is freed via free_tur_files. */
static void collect_tur_recursive(const char *dir,
                                  char ***files, int *n, int *cap) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* ., .., and dot subtrees */
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_tur_recursive(path, files, n, cap);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(ent->d_name);
            if (len < 4 || strcmp(ent->d_name + len - 4, ".tur") != 0) continue;
            if (pkg_is_manifest_name(ent->d_name)) continue; /* manifest */
            if (*n >= *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *files = (char **)realloc(*files, (size_t)*cap * sizeof(char *));
            }
            (*files)[(*n)++] = strdup(path);
        }
    }
    closedir(d);
}

/* Collect every `.tur` file under `dir`, recursively.
 *
 * `tur build <dir>`, `tur check <dir>`, and `tur test <dir>` used the FLAT
 * collect_tur_files, which sees only files sitting directly in `dir`.  A spice
 * whose modules live one level down -- `src/demo/lib.tur`, the layout
 * `:exports "demo/lib"` implies and the guides use -- therefore reported
 * `tur: no .tur files found in 'src/'`, on the invocation the `module not
 * found` diagnostic itself recommends.  Project mode already recursed
 * (collect_project_src_files), so the two spellings of "build this spice"
 * disagreed, with nothing in the output to say which one you got.
 *
 * Same shape as free_tur_files' input, so the caller frees it unchanged.
 * See docs/archive/tur-build-nested-src-dir-finds-no-files.md. */
static char **collect_tur_files_deep(const char *dir, int *n_out) {
    char **files = NULL;
    int n = 0, cap = 0;
    collect_tur_recursive(dir, &files, &n, &cap);
    *n_out = n;
    return files;
}

/* Collect a spice/library project's module files for a directory build.
 * Prefers a recursive walk of `<root>/src` (the conventional layout used by
 * every first-party spice, including nested `src/<pkg>/` trees); when there
 * is no `src/`, falls back to a shallow scan of `root` with the manifest
 * filtered out.  Heap array; free via free_tur_files. */
static char **collect_project_src_files(const char *root, int *n_out) {
    *n_out = 0;
    char **files = NULL;
    int n = 0, cap = 0;

    char src_dir[4096];
    snprintf(src_dir, sizeof(src_dir), "%s/src", root);
    struct stat st;
    if (stat(src_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        collect_tur_recursive(src_dir, &files, &n, &cap);
    } else {
        /* No src/ tree: shallow scan of the root, skipping build.tur. */
        int rn = 0;
        char **raw = collect_tur_files(root, &rn);
        for (int i = 0; i < rn; i++) {
            if (pkg_is_manifest_name(basename_of(raw[i]))) {
                free(raw[i]);
                continue;
            }
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                files = (char **)realloc(files, (size_t)cap * sizeof(char *));
            }
            files[n++] = raw[i];
        }
        free(raw);
    }
    *n_out = n;
    return files;
}

/* Build a project's include search path from an already-loaded manifest.
 * `root` is the project root (the dir holding build.tur); `m` its manifest.
 * When `include_own_src` is set, `root/src` is added first (so intra-spice
 * imports resolve for callers whose entry/test files live outside `src/`).
 * Each `:spices` dep is resolved in priority order:
 *   workspace-member sibling -> explicit `:path` -> fetched `:ref` dir
 *   -> `spices/<name>`; then `:subdir`, a `src/` preference, and a
 *   monorepo-sibling fallback when the resolved dir is absent.
 * *out_dirs is a heap array of strdup'd dirs (*out_n entries); the caller
 * frees each string and the array.  Shared by `tur run`, `tur test`, and
 * `tur build <dir>` so all three resolve the include path identically. */
/* spice-include-dirs-transitive: recurse one level into each dep's manifest
 * and add its :spices' src/ dirs too.  Walking only direct deps left tourist
 * unable to resolve `thread-pool/pool` (httpd's dep), even with the workspace
 * `:members` declaring it -- imports from one spice into another silently
 * dropped at the first hop.  Capacity grows as we go since transitive width is
 * not bounded by m->n_spices.
 *
 * Two separate dedups are at work, and they are not interchangeable:
 *   - include_dir_seen / push_include_dir dedupe the OUTPUT list, so a dep
 *     pulled in by several parents contributes one -I.
 *   - VisitedRoots below dedupes the WALK, so a cycle terminates.
 * This comment used to claim the first also did the second; it does not, and
 * a manifest cycle recursed until something else gave out.  See
 * docs/archive/history/spice-cycle-include-path-blowup.md. */
static bool include_dir_seen(const char **dirs, int n, const char *cand) {
    for (int i = 0; i < n; i++)
        if (dirs[i] && strcmp(dirs[i], cand) == 0) return true;
    return false;
}

static void grow_include_dirs(const char ***dirs, int *cap, int need) {
    if (need <= *cap) return;
    int new_cap = *cap ? *cap * 2 : 8;
    while (new_cap < need) new_cap *= 2;
    *dirs = (const char **)realloc(*dirs, (size_t)new_cap * sizeof(char *));
    *cap = new_cap;
}

/* Append `cand` to the include list unless an equivalent entry is already
 * there.  The path is canonicalized first (realpath, falling back to the raw
 * spelling when it fails): a transitively-resolved dep dir arrives as
 * `a/../b/src`, which the textual dedup would otherwise treat as distinct from
 * the `b/src` a different parent contributed.  Canonicalizing keeps the `-I`
 * list short and caps the damage from any future unbounded walk independently
 * of the visited set. */
static void push_include_dir(const char ***dirs, int *n, int *cap,
                             const char *cand) {
    char canon_buf[4096];
    const char *canon = cand;
    if (realpath(cand, canon_buf)) canon = canon_buf;
    if (include_dir_seen(*dirs, *n, canon)) return;
    grow_include_dirs(dirs, cap, *n + 1);
    if (!*dirs) return;
    (*dirs)[(*n)++] = strdup(canon);
}

/* spice-cycle-include-path-blowup: visited set for the transitive :spices walk,
 * keyed on each package root's canonical (realpath'd) path.  Without it a
 * manifest cycle (A declares B, B declares A) recurses forever: each lap
 * resolves through one more `../` hop, so the textual paths never repeat and
 * the output-dir dedup below never fires.  The symptom was either a
 * multi-kilobyte `-I` argument that cc rejected with "File name too long" on an
 * unrelated system header, or -- on a sanitized build -- a stack overflow.
 * Mirrors the keying pkg_collect_transitive_cmake_deps already uses for the
 * same shape of walk. */
typedef struct { char **paths; int n; int cap; } VisitedRoots;

/* True if `dir` was already visited; otherwise records it and returns false. */
static bool visited_roots_mark(VisitedRoots *v, const char *dir) {
    char canon_buf[4096];
    const char *canon = dir;
    if (realpath(dir, canon_buf)) canon = canon_buf;
    for (int i = 0; i < v->n; i++)
        if (strcmp(v->paths[i], canon) == 0) return true;
    if (v->n >= v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        char **np = (char **)realloc(v->paths, (size_t)nc * sizeof(char *));
        if (!np) return true;   /* OOM: treat as visited so the walk terminates */
        v->paths = np;
        v->cap   = nc;
    }
    v->paths[v->n++] = strdup(canon);
    return false;
}

static void visited_roots_free(VisitedRoots *v) {
    for (int i = 0; i < v->n; i++) free(v->paths[i]);
    free(v->paths);
}

static void collect_dep_dirs_recursive(const char *root, const PkgManifest *m,
                                       const char ***dirs, int *n, int *cap,
                                       VisitedRoots *visited);

static void resolve_include_dirs_from_manifest(const char *root,
                                               const PkgManifest *m,
                                               bool include_own_src,
                                               const char ***out_dirs,
                                               int *out_n) {
    *out_dirs = NULL;
    *out_n    = 0;

    int cap = 0;
    const char **dirs = NULL;
    int n = 0;
    grow_include_dirs(&dirs, &cap, (include_own_src ? 1 : 0) + m->n_spices + 8);
    if (!dirs) return;

    if (include_own_src) {
        char own_src[4096];
        snprintf(own_src, sizeof(own_src), "%s/src", root);
        struct stat ss;
        if (stat(own_src, &ss) == 0 && S_ISDIR(ss.st_mode))
            push_include_dir(&dirs, &n, &cap, own_src);
    }

    /* Seed the visited set with the root itself, so a dep that declares the
     * root back (the minimal A -> B -> A cycle) stops at the second hop. */
    VisitedRoots visited; memset(&visited, 0, sizeof(visited));
    visited_roots_mark(&visited, root);
    collect_dep_dirs_recursive(root, m, &dirs, &n, &cap, &visited);
    visited_roots_free(&visited);

    *out_dirs = dirs;
    *out_n    = n;
}

/* Walk every :spices entry in m, resolve its on-disk dir, push its src/ (or
 * dep_dir) into *dirs (deduped), then recurse into the dep's own manifest. */
static void collect_dep_dirs_recursive(const char *root, const PkgManifest *m,
                                       const char ***dirs, int *n, int *cap,
                                       VisitedRoots *visited) {
    char spices_dir[4096];
    snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);
    for (int i = 0; i < m->n_spices; i++) {
        const PkgSpice *s = &m->spices[i];
        char dep_dir[4096];
        /* LS4: a :spices entry that is a workspace sibling resolves to the
         * sibling's on-disk dir, ignoring any :url/:ref declared for
         * external publication. */
        char *ws_path = s->path ? NULL
                                : pkg_workspace_member_path(root, s->name);
        if (ws_path) {
            strncpy(dep_dir, ws_path, sizeof(dep_dir) - 1);
            dep_dir[sizeof(dep_dir) - 1] = '\0';
            free(ws_path);
        } else if (s->path) {
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root, s->path);
        } else if (s->ref) {
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                     spices_dir, s->name, s->ref);
        } else {
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s", spices_dir, s->name);
        }
        if (s->subdir) {
            char tmp[4096];
            snprintf(tmp, sizeof(tmp), "%s/%s", dep_dir, s->subdir);
            strncpy(dep_dir, tmp, sizeof(dep_dir) - 1);
            dep_dir[sizeof(dep_dir) - 1] = '\0';
        }
        char src_sub[4096];
        snprintf(src_sub, sizeof(src_sub), "%s/src", dep_dir);
        struct stat ss;
        const char *chosen = NULL;
        if (stat(src_sub, &ss) == 0 && S_ISDIR(ss.st_mode))
            chosen = src_sub;
        else if (stat(dep_dir, &ss) == 0 && S_ISDIR(ss.st_mode))
            chosen = dep_dir;

        /* `resolved_root` is the dep's true on-disk package root, used both
         * for manifest lookup (the recursion's `root`) and for choosing which
         * include path to push when src/ is not directly present. */
        char resolved_root[4096];
        resolved_root[0] = '\0';
        if (chosen) {
            /* dep_dir/src exists OR dep_dir itself exists; the package root is
             * dep_dir.  If chosen == src_sub, strip the trailing "/src" so the
             * manifest read targets dep_dir, not its src/. */
            strncpy(resolved_root, dep_dir, sizeof(resolved_root) - 1);
            resolved_root[sizeof(resolved_root) - 1] = '\0';
        }
        bool fallback_added = false;
        if (!chosen && s->subdir) {
            char ancestor[4096];
            strncpy(ancestor, root, sizeof(ancestor) - 1);
            ancestor[sizeof(ancestor) - 1] = '\0';
            for (int up = 0; up < 4 && !fallback_added; up++) {
                char *slash = strrchr(ancestor, '/');
                if (!slash || slash == ancestor) break;
                *slash = '\0';
                char sib_src[4096];
                snprintf(sib_src, sizeof(sib_src),
                         "%s/%s/src", ancestor, s->subdir);
                if (stat(sib_src, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                    push_include_dir(dirs, n, cap, sib_src);
                    snprintf(resolved_root, sizeof(resolved_root),
                             "%s/%s", ancestor, s->subdir);
                    chosen = NULL;
                    fallback_added = true;
                    break;
                }
                char sib_dir[4096];
                snprintf(sib_dir, sizeof(sib_dir),
                         "%s/%s", ancestor, s->subdir);
                if (stat(sib_dir, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                    push_include_dir(dirs, n, cap, sib_dir);
                    strncpy(resolved_root, sib_dir, sizeof(resolved_root) - 1);
                    resolved_root[sizeof(resolved_root) - 1] = '\0';
                    chosen = NULL;
                    fallback_added = true;
                    break;
                }
            }
        }
        if (chosen) push_include_dir(dirs, n, cap, chosen);

        /* Recurse into the dep's own manifest so its :spices contribute their
         * src/ too.  Use `resolved_root` (the real on-disk package root) for
         * manifest lookup; `dep_dir` may have :subdir appended to a :path,
         * yielding a non-existent path when both are set together. */
        if (!resolved_root[0]) continue;
        /* Stop on re-entry. A diamond (two parents pulling the same dep) is
         * deduped for free by the same check -- its src/ was already pushed on
         * the first visit. */
        if (visited_roots_mark(visited, resolved_root)) continue;
        char dmp[4096];
        if (!pkg_resolve_manifest_path(resolved_root, dmp, sizeof(dmp))) continue;
        PkgManifest dm; memset(&dm, 0, sizeof(dm));
        if (!pkg_manifest_read(dmp, &dm)) continue;
        collect_dep_dirs_recursive(resolved_root, &dm, dirs, n, cap, visited);
        pkg_manifest_free(&dm);
    }
}

/* spices-c-sources-plan: collect a project's vendored C build inputs into two
 * command-line fragments (caller inits/frees the Bufs):
 *   - `includes`: -I flags. The project's OWN :c-includes (private to the
 *     spice) plus, for each :spices dep, that dep's :c-includes -- the latter
 *     are required so the dep's vendored .c can find its own headers when
 *     compiled in the same cc invocation. Dep includes are an implementation
 *     detail; consumers should not rely on them being visible to their .tur
 *     inline-C, even though source-level aggregation makes them so here.
 *   - `sources`: absolute paths to the project's own :c-sources AND each
 *     :spices dep's :c-sources, so a consumer of a vendor spice links the
 *     vendor's hand-written C into the final binary.
 * g_no_auto_spice disables manifest discovery entirely (matching every other
 * auto-spice convenience). */
static void collect_spice_aux_c(const char *root, Buf *includes, Buf *sources) {
    if (g_no_auto_spice || !root) return;
    char mp[4096];
    if (!pkg_resolve_manifest_path(root, mp, sizeof(mp))) return;
    PkgManifest m; memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(mp, &m)) return;

    for (int i = 0; i < m.n_c_includes; i++)
        buf_printf(includes, " -I%s/%s", root, m.c_includes[i]);
    for (int i = 0; i < m.n_c_sources; i++)
        buf_printf(sources, " %s/%s", root, m.c_sources[i]);

    /* Propagate each :spices dep's :c-sources (resolved to the dep root). The
     * dep-dir resolution mirrors resolve_include_dirs_from_manifest above. */
    char spices_dir[4096];
    snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);
    for (int i = 0; i < m.n_spices; i++) {
        const PkgSpice *s = &m.spices[i];
        char dep_dir[4096];
        char *ws_path = s->path ? NULL
                                : pkg_workspace_member_path(root, s->name);
        if (ws_path) {
            snprintf(dep_dir, sizeof(dep_dir), "%s", ws_path);
            free(ws_path);
        } else if (s->path) {
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root, s->path);
        } else if (s->ref) {
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                     spices_dir, s->name, s->ref);
        } else {
            snprintf(dep_dir, sizeof(dep_dir), "%s/%s", spices_dir, s->name);
        }
        if (s->subdir) {
            char tmp[4096];
            snprintf(tmp, sizeof(tmp), "%s/%s", dep_dir, s->subdir);
            snprintf(dep_dir, sizeof(dep_dir), "%s", tmp);
        }
        char dmp[4096];
        if (!pkg_resolve_manifest_path(dep_dir, dmp, sizeof(dmp))) continue;
        PkgManifest dm; memset(&dm, 0, sizeof(dm));
        if (!pkg_manifest_read(dmp, &dm)) continue;
        for (int j = 0; j < dm.n_c_sources; j++)
            buf_printf(sources, " %s/%s", dep_dir, dm.c_sources[j]);
        for (int j = 0; j < dm.n_c_includes; j++)
            buf_printf(includes, " -I%s/%s", dep_dir, dm.c_includes[j]);
        pkg_manifest_free(&dm);
    }
    pkg_manifest_free(&m);
}

/* Resolve a project's include search path by walking up from `dir` to find
 * the enclosing build.tur, then delegating to
 * resolve_include_dirs_from_manifest with the project's own `src/` included.
 * No-op (leaves *out_n = 0) when there is no manifest.  Used by `tur test`
 * and `tur build <dir>`. */
static void resolve_project_include_dirs(const char *dir,
                                         const char ***out_dirs, int *out_n) {
    *out_dirs = NULL;
    *out_n    = 0;

    char abs_dir[4096];
    if (!realpath(dir, abs_dir)) {
        strncpy(abs_dir, dir, sizeof(abs_dir) - 1);
        abs_dir[sizeof(abs_dir) - 1] = '\0';
    }
    char *proj_root = find_project_root(abs_dir);
    if (!proj_root) return;

    char manifest_path[4096];
    if (!pkg_resolve_manifest_path(proj_root, manifest_path,
                                   sizeof(manifest_path))) {
        free(proj_root);
        return;
    }
    PkgManifest m;
    if (!pkg_manifest_read(manifest_path, &m)) {
        free(proj_root);
        return;
    }

    resolve_include_dirs_from_manifest(proj_root, &m, /*include_own_src=*/true,
                                       out_dirs, out_n);

    pkg_manifest_free(&m);
    free(proj_root);
}

static int decode_exit_status(int status) {
    if (status == -1) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int cmd_run(int argc, char **argv);   /* defined below; J1 fallback */
static int cmd_eval(const char *path, bool use_color,
                    char **extra_argv, int extra_argc, bool debug);
static int cmd_jit(int argc, char **argv);

#ifdef TUR_HAVE_JIT
/* S2 (findings 25): swap an emitted TU's fixed preamble for the committed
 * declarations region when this compiler still matches the committed
 * artifacts.  Returns true and fills `out` with
 * [hoisted user prefix][committed decls][program half]; false (out
 * untouched) on hash mismatch, missing marker, or TUR_JIT_NO_SPLIT=1.
 * Shared by cmd_jit and the J2 REPL image build.  See cmd_jit's call site
 * for the full rationale. */
/* Report WHY the split did not engage, under TUR_JIT_TIMING=1.
 *
 * The split fails closed -- a rejected hash just means the full preamble is
 * emitted, which is correct but slow -- so a disengage is invisible in every
 * observable except wall-clock.  That is how a hoisted inline-C `#include`
 * disabled the fast path for the entire httpd-*, image-*, and C-binding-spice
 * corpus without anyone noticing.  Note that the absence of TUR-W0071 is NOT
 * evidence the split engaged: W0071 means engaged-then-failed-to-compile, and a
 * never-engaged build is silent.  See
 * docs/archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md. */
static bool jit_split_reject(const char *why) {
    const char *v = getenv("TUR_JIT_TIMING");
    if (v && *v && strcmp(v, "0") != 0)
        fprintf(stderr, "TUR_JIT_TIMING\tsplit\tdisengaged\t%s\n", why);
    return false;
}

static bool jit_try_split_preamble(Buf *csrc, Buf *out) {
    const char *no_split = getenv("TUR_JIT_NO_SPLIT");
    if (no_split && *no_split && strcmp(no_split, "0") != 0)
        return jit_split_reject("TUR_JIT_NO_SPLIT set");
    if (!csrc->data) return jit_split_reject("no emitted source");
    Buf probe;
    buf_init(&probe);
    emit_rt_split_source(&probe);
    uint64_t cur = tur_hamt_hash_xxh64(probe.data, probe.len);
    buf_free(&probe);
    if (cur != tur_rt_split_hash)
        return jit_split_reject("preamble hash != committed artifact "
                                "(regenerate with tools/gen-runtime-split.py)");
    buf_putc(csrc, '\0');
    csrc->len--;   /* NUL-terminate for strstr, keep logical len */
    static const char pre_start[] = "/* generated by tur (phase 2) */\n";
    static const char pre_end[] =
        "/* ==== tur: end of fixed runtime preamble ==== */\n";
    const char *ps = strstr(csrc->data, pre_start);
    const char *pe = ps ? strstr(ps, pre_end) : NULL;
    if (!ps || !pe) return jit_split_reject("preamble markers not found");
    const char *after = pe + sizeof(pre_end) - 1;
    buf_write(out, csrc->data, (size_t)(ps - csrc->data));
    buf_write(out, tur_rt_split_decls, tur_rt_split_decls_len);
    buf_write(out, after, csrc->len - (size_t)(after - csrc->data));
    return true;
}

/* The engine's `#include "hamt.h"` (et al.) include path: the Turmeric tree
 * (dev checkout, walking up from the executable) or the installed SDK.
 * Fills inc0/inc1 (caller-owned, >= 4096 each) and incs[0..1]; returns the
 * count (0 when no root was found). */
static int jit_sdk_include_dirs(char *inc0, size_t cap0,
                                char *inc1, size_t cap1,
                                const char *incs[2]) {
    int n = 0;
    char root[4096] = "";
    const char *env = getenv("TUR_SDK_ROOT");
    if (env && *env) {
        snprintf(root, sizeof(root), "%s/share/turmeric", env);
        struct stat st;
        char probe[4200];
        snprintf(probe, sizeof(probe), "%s/src/runtime/hamt.h", root);
        if (stat(probe, &st) != 0) snprintf(root, sizeof(root), "%s", env);
    } else {
        char exe[4096] = "";
        if (get_exe_path(exe, sizeof(exe)) == 0) {
            char dir[4096];
            dir_of_path(exe, dir, sizeof(dir));
            for (int d = 0; d < 8; d++) {
                char probe[4200];
                struct stat st;
                snprintf(probe, sizeof(probe), "%s/src/runtime/hamt.h", dir);
                if (stat(probe, &st) == 0) { snprintf(root, sizeof(root), "%s", dir); break; }
                snprintf(probe, sizeof(probe),
                         "%s/share/turmeric/src/runtime/hamt.h", dir);
                if (stat(probe, &st) == 0) {
                    snprintf(root, sizeof(root), "%s/share/turmeric", dir);
                    break;
                }
                char *sl = strrchr(dir, '/');
                if (!sl || sl == dir) break;
                *sl = '\0';
            }
        }
    }
    if (root[0]) {
        snprintf(inc0, cap0, "%s/src", root);
        snprintf(inc1, cap1, "%s/src/runtime", root);
        incs[n++] = inc0;
        incs[n++] = inc1;
    }
    return n;
}
#endif /* TUR_HAVE_JIT */

/* S2 (jit-engine-plan, findings 19.4): dump the feature-complete single-file
 * runtime preamble -- every program-gated block on, rc/GC in archive mode,
 * ending at the preamble marker -- for the split-generation tool
 * (tools/gen-runtime-split.py), or with --hash just the xxHash64 of that
 * text.  The hash spelling here IS the JIT-time compare: both sides call
 * tur_hamt_hash_xxh64 over the same emission, so the recorded artifact hash
 * and cmd_jit's probe can never disagree on hash function or input framing.
 *
 * Archive mode is hard-set rather than probed so the generated text is a
 * property of the compiler, not of which build tree generated it; cmd_jit's
 * probe runs under real process state, and a JIT invocation with no runtime
 * archive (rc/GC emitted as definitions) therefore hashes differently and
 * falls back to full-preamble emission -- the guard is self-enforcing. */
static int cmd_emit_rt_split(int argc, char **argv) {
    bool hash_only = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--hash") == 0) { hash_only = true; continue; }
        fprintf(stderr, "usage: tur emit-rt-split [--hash]\n");
        return 2;
    }
    emit_set_rcgc_from_archive(true);
    Buf out; buf_init(&out);
    emit_rt_split_source(&out);
    if (hash_only) {
        printf("%016llx\n",
               (unsigned long long)tur_hamt_hash_xxh64(out.data, out.len));
    } else {
        fwrite(out.data, 1, out.len, stdout);
    }
    buf_free(&out);
    return 0;
}

/* J1 (docs/archive/jit-engine-plan.md section 3.2): `tur jit <file>` --
 * compile and execute in process via c2mir + MIR-gen, no cc subprocess, no
 * disk artifacts.  Front half is cmd_build's exactly: run_core_passes via
 * compile_to_c, then the same in-memory post-passes.  The back half hands the
 * buffer to tur_jit_execute (src/jit_engine.c, built under -DTUR_JIT=ON).
 *
 * Fallback (plan step 6): any engine-level failure -- c2mir rejecting a GNU
 * construct in user inline-C is the expected case -- prints a TUR-W and
 * delegates to cmd_run, which recompiles through cc.  Never a hard stop.
 *
 * Usage: tur jit [-I <dir>...] <file> [-- <args>...]  */
static int cmd_jit(int argc, char **argv) {
    /* engine-selection-plan P0: locate the input FIRST, before anything that
     * depends on the enclosing manifest.  The original reason was the
     * `:experiments [jit]` gate (which graduated away 2026-08-17); the
     * surviving one is `:reader-macros`, which was ignored by the same
     * ordering bug and is restored here. */
    const char *input = NULL;
    /* B4 (post-jit-benchmark-resurrection-plan): --timing-json <path> writes
     * {"compile_ms","run_ms","engine"} after the run, so a benchmark harness
     * can separate compile from run (chart A) and detect the cc fallback
     * instead of averaging it in.  The JIT knows both numbers exactly;
     * recovering them from outside is guesswork. */
    const char *timing_json = NULL;
    int passthrough_start = -1;
    int scan_end = argc;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { scan_end = i; break; }
    char **user_inc = NULL;
    int n_user_inc = parse_include_flags(scan_end, argv, 2, &user_inc);
    if (n_user_inc < 0) { free(user_inc); return 2; }
    for (int i = 2; i < scan_end; i++) {
        int c;
        if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
        if (strcmp(argv[i], "--timing-json") == 0 && i + 1 < scan_end) {
            timing_json = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--timing-json=", 14) == 0) {
            timing_json = argv[i] + 14;
            continue;
        }
        if (argv[i][0] != '-' && !input) input = argv[i];
    }
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) { passthrough_start = i + 1; break; }
    if (!input) {
        fprintf(stderr, "usage: tur jit [-I <dir>...] <file> [-- <args>...]\n");
        free(user_inc);
        return 2;
    }
    int jit_rm_n = 0;
    char **jit_rm = discover_manifest_reader_macros(input, &jit_rm_n);
    /* The `jit` experiment GRADUATED 2026-08-17 -- `tur jit` no longer needs
     * `--enable=jit`.  The BUILD-TIME gate below is the one that remains: a
     * default build vendors no MIR and therefore carries no engine. */
#ifndef TUR_HAVE_JIT
    (void)passthrough_start;   /* used only by the engine path below */
    (void)timing_json;
    fprintf(stderr,
            "tur: this build carries no JIT engine; reconfigure with "
            "-DTUR_JIT=ON\n"
            "     (vendors MIR at configure time -- see cmake/mir.cmake)\n");
    free_reader_macro_paths(jit_rm, jit_rm_n);
    free(user_inc);
    return 2;
#else

    refine_discharge_reset();
    g_emit_for_link = true;
    Buf csrc;
    buf_init(&csrc);
    struct timespec _tj0, _tj1;
    clock_gettime(CLOCK_MONOTONIC, &_tj0);
    int rc = compile_to_c(input, &csrc, (const char **)user_inc, n_user_inc,
                          (const char **)jit_rm, jit_rm_n);
    clock_gettime(CLOCK_MONOTONIC, &_tj1);
    /* Turmeric front-end time (read -> elaborate -> emit C); the engine's
     * own c2mir+link time is added below so compile_ms is invocation-to-
     * entry, the quantity chart A subtracts. */
    double front_ms = ((double)_tj1.tv_sec - (double)_tj0.tv_sec) * 1000.0 +
                      ((double)_tj1.tv_nsec - (double)_tj0.tv_nsec) / 1.0e6;
    free_reader_macro_paths(jit_rm, jit_rm_n);
    if (rc != 0) { buf_free(&csrc); free(user_inc); return rc; }
    hoist_tur_include_directives(&csrc);
    Buf autolink;
    buf_init(&autolink);
    scan_autolink_markers(&csrc, &autolink);

    /* S2 (findings 19.4 item 3): swap the fixed preamble for the committed
     * declarations region when this compiler still matches the committed
     * artifacts.  The runtime then stays host-resident (tur_rt_split.c is
     * linked into this executable and exported for the engine's dlsym
     * resolver), and c2mir compiles ~60% less fixed text per program.
     *
     * The guard: re-emit the all-gates preamble under CURRENT process state
     * and hash it against the recorded artifact hash.  Emitter drift, knob
     * drift (--backtrack-depth, experiments), and a missing runtime archive
     * all change that text, fail the compare, and keep the full preamble --
     * never wrong, just slower.  Probe cost is one in-memory emission,
     * microseconds against the ~90ms c2mir spend it can save.  Runs AFTER
     * compile_to_c: emit_rt_split_source resets per-TU codegen registries,
     * which must not disturb the program's own emission.
     *
     * The TU layout post-hoist is [hoisted user includes/code][preamble ...
     * marker][program half]; the splice replaces exactly the preamble
     * span, keeping any hoisted prefix.  TUR_JIT_NO_SPLIT=1 opts out. */
    Buf split_src;
    buf_init(&split_src);
    bool split_used = jit_try_split_preamble(&csrc, &split_src);

    /* Program argv: argv[0] = the source path (matches what a compiled binary
     * would see as its own name closely enough for *args*), then everything
     * after `--`. */
    int prog_argc = 1 + (passthrough_start > 0 ? argc - passthrough_start : 0);
    char **prog_argv = (char **)malloc(((size_t)prog_argc + 1) * sizeof(char *));
    if (!prog_argv) {
        buf_free(&csrc); buf_free(&split_src); buf_free(&autolink);
        free(user_inc); return 2;
    }
    prog_argv[0] = (char *)input;
    for (int i = 0; i < prog_argc - 1; i++)
        prog_argv[i + 1] = argv[passthrough_start + i];
    prog_argv[prog_argc] = NULL;

    /* Include path for `#include "hamt.h"` et al. */
    static char jit_inc0[4096], jit_inc1[4096];
    const char *jit_incs[2];
    int n_jit_incs = jit_sdk_include_dirs(jit_inc0, sizeof(jit_inc0),
                                          jit_inc1, sizeof(jit_inc1),
                                          jit_incs);

    int prog_rc = 0;
    int jrc;
    if (split_used) {
        /* Split first; if the split half fails to COMPILE or LINK, retry the
         * full TU in the engine before conceding to cc -- the hash guard
         * covers emitter drift but not, e.g., an export the host build
         * dropped, and the full TU is self-contained against that.  A RUN
         * failure is the program's own (a panic aborts identically either
         * way), so it is not retried. */
        jrc = tur_jit_execute(split_src.data, split_src.len,
                              autolink.len ? autolink.data : NULL,
                              jit_incs, n_jit_incs,
                              prog_argc, prog_argv, &prog_rc);
        if (jrc == TUR_JIT_ERR_COMPILE || jrc == TUR_JIT_ERR_LINK) {
            fprintf(stderr,
                    "tur: warning: TUR-W0071: split-runtime path failed to "
                    "%s; retrying with the full preamble\n",
                    jrc == TUR_JIT_ERR_COMPILE ? "compile" : "link");
            jrc = tur_jit_execute(csrc.data, csrc.len,
                                  autolink.len ? autolink.data : NULL,
                                  jit_incs, n_jit_incs,
                                  prog_argc, prog_argv, &prog_rc);
        }
    } else {
        jrc = tur_jit_execute(csrc.data, csrc.len,
                              autolink.len ? autolink.data : NULL,
                              jit_incs, n_jit_incs,
                              prog_argc, prog_argv, &prog_rc);
    }
    free(prog_argv);
    buf_free(&csrc);
    buf_free(&split_src);
    buf_free(&autolink);
    free(user_inc);
    if (jrc == TUR_JIT_OK) {
        if (timing_json) {
            double eng_compile_ms = 0.0, run_ms = 0.0;
            tur_jit_last_timings(&eng_compile_ms, &run_ms);
            FILE *tf = fopen(timing_json, "w");
            if (tf) {
                fprintf(tf,
                        "{\"compile_ms\": %.3f, \"run_ms\": %.3f, "
                        "\"engine\": \"jit\"}\n",
                        front_ms + eng_compile_ms, run_ms);
                fclose(tf);
            }
        }
        return prog_rc;
    }

    /* Plan step 6: clean per-program fallback to the cc path.  cmd_run parses
     * the same argv shape (it never looks at argv[1]), so delegate whole. */
    fprintf(stderr,
            "tur: warning: TUR-W0070: jit engine could not %s this program "
            "(see diagnostics above); falling back to the cc path\n",
            jrc == TUR_JIT_ERR_COMPILE ? "compile"
            : jrc == TUR_JIT_ERR_LINK  ? "link"
                                       : "run");
    if (timing_json) {
        /* B4 / plan section 3.3: a benchmark that silently falls back is
         * measuring `tur build` with extra steps -- make it detectable. */
        FILE *tf = fopen(timing_json, "w");
        if (tf) {
            fprintf(tf, "{\"engine\": \"cc-fallback\"}\n");
            fclose(tf);
        }
    }
    return cmd_run(argc, argv);
#endif /* TUR_HAVE_JIT */
}

#ifdef TUR_HAVE_JIT
/* ------------------------------------------------------------------ */
/* J2 (jit-engine-plan 3.3): the REPL's in-process spice build.        */
/* ------------------------------------------------------------------ */
/* Replaces the `tur build --shared` subprocess + dlopen with the MIR
 * engine, via the TurSpiceJitHook the loader consults.  The whole spice
 * is compiled as ONE in-memory TU through the well-tested single-file
 * path: a synthetic root module imports every source module, so imports
 * dedupe through the ordinary module machinery (a load-based root
 * duplicated any module that was also imported intra-spice).
 *
 * Module names come from each file's `(defmodule <name>` (filename stem
 * when absent), and files whose module name does not match their
 * filename are made importable through a SHADOW DIR of symlinks under
 * .tur-repl-cache/jit-mods/ -- module resolution is filename-based, and
 * on the --shared path a mismatched file was reachable only because each
 * file was compiled separately.
 *
 * v1 limits (recorded, not silent): transitive :spices deps are not
 * auto-appended (single-spice projects only -- the subprocess path
 * remains the default and handles them), and POSIX symlinks gate this
 * out of Windows along with the engine itself. */

/* Peek a source file's defmodule name into out (cap bytes).  Textual scan
 * of the first non-comment occurrence -- both `(defmodule x` and sweet-exp
 * `defmodule x` spellings.  Returns false when the file has none. */
static bool repl_jit_peek_module_name(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[4096];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';') continue;
        if (*p == '(') { p++; while (*p == ' ') p++; }
        if (strncmp(p, "defmodule", 9) != 0) continue;
        p += 9;
        if (*p != ' ' && *p != '\t') continue;
        while (*p == ' ' || *p == '\t') p++;
        size_t o = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ')' && *p != '\n'
               && *p != '\r' && o + 1 < cap)
            out[o++] = *p++;
        out[o] = '\0';
        found = o > 0;
    }
    fclose(f);
    return found;
}

struct repl_jit_mod {
    char *src_path;   /* absolute source file path */
    char *mod_name;   /* module name (may contain '/') */
};

static int repl_jit_scan_dir(const char *dir, struct repl_jit_mod **mods,
                             uint32_t *n, uint32_t *cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;   /* unreadable subdir: skip, the build would too */
    struct dirent *e;
    int rc = 0;
    while (rc == 0 && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[4600];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rc = repl_jit_scan_dir(path, mods, n, cap);
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".tur") != 0) continue;
        char modname[512];
        if (!repl_jit_peek_module_name(path, modname, sizeof(modname))) {
            snprintf(modname, sizeof(modname), "%.*s",
                     (int)(nl - 4), e->d_name);
        }
        if (*n == *cap) {
            uint32_t nc = *cap ? *cap * 2 : 8;
            struct repl_jit_mod *na =
                realloc(*mods, nc * sizeof(**mods));
            if (!na) { rc = -1; break; }
            *mods = na;
            *cap = nc;
        }
        (*mods)[*n].src_path = strdup(path);
        (*mods)[*n].mod_name = strdup(modname);
        (*n)++;
    }
    closedir(d);
    return rc;
}

static void repl_jit_mods_free(struct repl_jit_mod *mods, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        free(mods[i].src_path);
        free(mods[i].mod_name);
    }
    free(mods);
}

/* mkdir -p for the directory part of shadow/<modname>.tur. */
static void repl_jit_mkdirs_for(const char *shadow, const char *modname) {
    char path[4800];
    snprintf(path, sizeof(path), "%s/%s", shadow, modname);
    for (char *p = path + strlen(shadow) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
}

static int repl_jit_build(const char *build_dir, void **out_image,
                          char **out_manifest) {
    *out_image = NULL;
    *out_manifest = NULL;

    /* Spice root: the dir holding build.tur -- build_dir's parent when the
     * conventional src/ layout is in play, else build_dir itself. */
    char rootd[4300];
    snprintf(rootd, sizeof(rootd), "%s", build_dir);
    size_t rl = strlen(rootd);
    if (rl > 4 && strcmp(rootd + rl - 4, "/src") == 0) rootd[rl - 4] = '\0';

    struct repl_jit_mod *mods = NULL;
    uint32_t n_mods = 0, cap_mods = 0;
    if (repl_jit_scan_dir(build_dir, &mods, &n_mods, &cap_mods) != 0
        || n_mods == 0) {
        fprintf(stderr, "tur repl: jit: no .tur sources under %s\n",
                build_dir);
        repl_jit_mods_free(mods, n_mods);
        return -1;
    }

    /* Shadow dir: module-name -> file symlinks + the synthetic root. */
    char shadow[4400];
    snprintf(shadow, sizeof(shadow), "%s/.tur-repl-cache", rootd);
    mkdir(shadow, 0755);
    snprintf(shadow, sizeof(shadow), "%s/.tur-repl-cache/jit-mods", rootd);
    mkdir(shadow, 0755);

    Buf root_src;
    buf_init(&root_src);
    buf_puts(&root_src, "(defmodule __jit-root\n");
    int rc = 0;
    for (uint32_t i = 0; i < n_mods && rc == 0; i++) {
        repl_jit_mkdirs_for(shadow, mods[i].mod_name);
        char link[4900];
        snprintf(link, sizeof(link), "%s/%s.tur", shadow, mods[i].mod_name);
        unlink(link);
        if (symlink(mods[i].src_path, link) != 0) {
            fprintf(stderr, "tur repl: jit: symlink %s: %s\n", link,
                    strerror(errno));
            rc = -1;
            break;
        }
        buf_printf(&root_src, "  (import %s)\n", mods[i].mod_name);
    }
    buf_puts(&root_src, ")\n");
    repl_jit_mods_free(mods, n_mods);

    char root_file[4600];
    snprintf(root_file, sizeof(root_file), "%s/__jit_root.tur", shadow);
    if (rc == 0) {
        FILE *rf = fopen(root_file, "w");
        if (!rf || fwrite(root_src.data, 1, root_src.len, rf) != root_src.len) {
            fprintf(stderr, "tur repl: jit: cannot write %s\n", root_file);
            rc = -1;
        }
        if (rf) fclose(rf);
    }
    buf_free(&root_src);
    if (rc != 0) return -1;

    /* Compile the whole spice as one TU, capturing the exports manifest
     * from the same program.  Same emission posture as cmd_jit -- which
     * means COMPILED-mode elaboration: the REPL process runs with
     * g_interpret_mode=true (turi_env_new sets it), and inheriting that
     * here would select `#?(:turi ...)` branches into native code and
     * demote unknown names from hard errors to W0040 runtime-dispatch
     * warnings.  The subprocess build never saw the flag; clear it for
     * exactly the compile. */
    refine_discharge_reset();
    bool saved_efl = g_emit_for_link;
    bool saved_interp = g_interpret_mode;
    g_emit_for_link = true;
    g_interpret_mode = false;
    Buf csrc, manifest;
    buf_init(&csrc);
    buf_init(&manifest);
    const char *incs[1] = { shadow };
    g_manifest_sink = &manifest;
    g_emit_ffi_export_shims = true;   /* high-arity exports need __ffi shims */
    rc = compile_to_c(root_file, &csrc, incs, 1, NULL, 0);
    g_emit_ffi_export_shims = false;
    g_manifest_sink = NULL;
    g_emit_for_link = saved_efl;
    g_interpret_mode = saved_interp;
    if (rc != 0) {
        /* Same actionable wording as the subprocess path (run_build): the
         * user story -- fix the source, (reload) -- is identical. */
        fprintf(stderr,
                "tur repl: spice rebuild failed; fix the error above, then "
                "type (reload) at the prompt to retry.\n");
        buf_free(&csrc);
        buf_free(&manifest);
        return -1;
    }
    hoist_tur_include_directives(&csrc);
    Buf autolink;
    buf_init(&autolink);
    scan_autolink_markers(&csrc, &autolink);

    /* ffi-spices-integration-plan S1: the subprocess build injects the
     * spice's cmake-dep and :link-libs flags into cc's link line; the
     * in-process image's equivalent is the autolink string, whose -L/-l
     * entries jit_load_autolink dlopens RTLD_GLOBAL before MIR_link
     * resolves.  Without this, a spice that declares its C dependency the
     * recommended way (:cmake-deps / :link-libs, no __tur_autolink__
     * marker anywhere) compiled fine in-process and then failed to resolve
     * the library's symbols.  Vendored :c-sources cannot be MIR-linked
     * in-process at all -- fail the hook so the loader falls back to the
     * subprocess path, which compiles them into the .so. */
    {
        Buf cmk, auxi, auxs;
        buf_init(&cmk);
        buf_init(&auxi);
        buf_init(&auxs);
        char probe[4400];
        snprintf(probe, sizeof(probe), "%s/build.tur", rootd);
        collect_build_aux(probe, &cmk, &auxi, &auxs);
        bool have_aux_sources = auxs.len > 0;
        if (cmk.len > 0) {
            if (autolink.len > 0) {
                /* scan_autolink_markers NUL-terminated it; reopen. */
                autolink.len--;
            }
            buf_write(&autolink, cmk.data, cmk.len);
            buf_putc(&autolink, '\0');
        }
        buf_free(&cmk);
        buf_free(&auxi);
        buf_free(&auxs);
        if (have_aux_sources) {
            fprintf(stderr,
                    "tur repl: jit: this spice vendors C sources "
                    "(:c-sources), which the in-process engine cannot "
                    "link; using the subprocess build instead.\n");
            buf_free(&csrc);
            buf_free(&autolink);
            buf_free(&manifest);
            return -1;
        }
    }

    /* S2: same hash-gated preamble swap as cmd_jit -- a REPL reload is
     * exactly the loop the split exists for. */
    Buf split_src;
    buf_init(&split_src);
    bool split_used = jit_try_split_preamble(&csrc, &split_src);

    static char jinc0[4096], jinc1[4096];
    const char *jincs[2];
    int n_jincs = jit_sdk_include_dirs(jinc0, sizeof(jinc0),
                                       jinc1, sizeof(jinc1), jincs);

    TurJitImage *img = NULL;
    const Buf *use = split_used ? &split_src : &csrc;
    int jrc = tur_jit_compile_image(use->data, use->len,
                                    autolink.len ? autolink.data : NULL,
                                    jincs, n_jincs, &img);
    if (jrc != TUR_JIT_OK && split_used) {
        /* Same ladder as cmd_jit: the full TU is self-contained against a
         * hole the hash guard cannot see. */
        fprintf(stderr,
                "tur: warning: TUR-W0071: split-runtime path failed; "
                "retrying with the full preamble\n");
        jrc = tur_jit_compile_image(csrc.data, csrc.len,
                                    autolink.len ? autolink.data : NULL,
                                    jincs, n_jincs, &img);
    }
    buf_free(&csrc);
    buf_free(&split_src);
    buf_free(&autolink);
    if (jrc != TUR_JIT_OK) {
        fprintf(stderr,
                "tur repl: jit: engine could not %s the spice; fix the "
                "error above and type (reload) to retry.\n",
                jrc == TUR_JIT_ERR_COMPILE ? "compile" : "link");
        buf_free(&manifest);
        return -1;
    }

    buf_putc(&manifest, '\0');
    *out_manifest = strdup(manifest.data);
    buf_free(&manifest);
    *out_image = img;
    return 0;
}

static void *repl_jit_hook_sym(void *image, const char *mangled) {
    return tur_jit_image_sym((TurJitImage *)image, mangled);
}
static void repl_jit_hook_free(void *image) {
    tur_jit_image_free((TurJitImage *)image);
}
static const TurSpiceJitHook g_repl_jit_hook = {
    repl_jit_build, repl_jit_hook_sym, repl_jit_hook_free,
};
#endif /* TUR_HAVE_JIT */

/* engine-selection-plan E2/E3: delegate a resolved non-cc engine.
 * The subcommand arms keep their existing bodies (`cmd_jit` / `cmd_eval`);
 * this adapts `tur run`'s normalized "entry + includes + program args"
 * request to each.  Unsatisfiable configurations are HARD errors -- a
 * project that declares an engine has declared a semantic requirement,
 * and quietly running a different one is the worst available outcome
 * (the JIT's own runtime TUR-W0070 cc fallback is unchanged: that one is
 * a per-program capability miss with a warning attached, not a
 * misconfiguration). */
static int run_delegate_engine(const char *engine, const char *entry,
                               char **user_inc, int n_user_inc,
                               int argc, char **argv,
                               int passthrough_start) {
    if (getenv("TUR_VERBOSE") && *getenv("TUR_VERBOSE"))
        fprintf(stderr, "tur run: engine '%s' for %s\n", engine, entry);
    if (strcmp(engine, "interp") == 0) {
        /* The tree-walker discovers the enclosing spice itself (per-file
         * auto-spice), so user -I dirs are not threaded; program args after
         * `--` become *args*. */
        char **prog_argv = (passthrough_start >= 0) ? argv + passthrough_start
                                                    : NULL;
        int    prog_argc = (passthrough_start >= 0) ? argc - passthrough_start
                                                    : 0;
        return cmd_eval(entry, stderr_is_tty(), prog_argv, prog_argc,
                        /*debug=*/false);
    }
    /* jit */
#ifndef TUR_HAVE_JIT
    (void)user_inc; (void)n_user_inc;
    fprintf(stderr,
            "tur run: engine \"jit\" is configured, but this build carries "
            "no JIT engine\n"
            "     reconfigure with -DTUR_JIT=ON (vendors MIR at configure "
            "time -- see cmake/mir.cmake),\n"
            "     or override the engine: --engine cc / TUR_ENGINE=cc\n");
    return 2;
#else
    /* Rebuild a `tur jit` argv: {argv0, "jit", -I <d>..., entry, --, rest}.
     * cmd_jit performs its own manifest discovery (P0) and TUR-W0070 cc
     * fallback. */
    int cap = 3 + 2 * n_user_inc + 1 +
              (passthrough_start >= 0 ? 1 + (argc - passthrough_start) : 0);
    char **jargv = (char **)malloc((size_t)(cap + 1) * sizeof(char *));
    if (!jargv) return 2;
    int n = 0;
    jargv[n++] = argv[0];
    jargv[n++] = (char *)"jit";
    for (int i = 0; i < n_user_inc; i++) {
        jargv[n++] = (char *)"-I";
        jargv[n++] = user_inc[i];
    }
    jargv[n++] = (char *)entry;
    if (passthrough_start >= 0) {
        jargv[n++] = (char *)"--";
        for (int i = passthrough_start; i < argc; i++) jargv[n++] = argv[i];
    }
    jargv[n] = NULL;
    int rc = cmd_jit(n, jargv);
    free(jargv);
    return rc;
#endif
}

static int cmd_run(int argc, char **argv) {
    /* tur run [-I <dir>...] [--release] [--offline] [<file>] [-- <args>...] */
    bool        release           = false;
    bool        offline           = false;
    const char *explicit_file     = NULL;
    const char *engine_flag       = NULL;   /* engine-selection-plan E1 */
    int         passthrough_start = -1;

    /* SC2: collect -I flags up front (stops scanning at `--`, since
     * everything after is passthrough to the spawned program). */
    int  scan_end = argc;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { scan_end = i; break; }
    }
    char **user_inc = NULL;
    int    n_user_inc = parse_include_flags(scan_end, argv, 2, &user_inc);
    if (n_user_inc < 0) { free(user_inc); return usage_run(); }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            passthrough_start = i + 1;
            break;
        }
        int c;
        if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
        if (strcmp(argv[i], "--release") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--offline") == 0) {
            offline = true;
        } else if (strcmp(argv[i], "--engine") == 0 && i + 1 < scan_end) {
            /* engine-selection-plan E1: CLI rung of the ladder. */
            engine_flag = argv[++i];
        } else if (strncmp(argv[i], "--engine=", 9) == 0) {
            engine_flag = argv[i] + 9;
        } else if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
            if (!explicit_file) explicit_file = argv[i];
        }
    }
    (void)release; /* passed to compiler when --release build is supported */

    /* spice_inc_dirs: populated below during project-mode setup.
     * RUN_ENTRY captures these via the enclosing scope.  User-supplied
     * -I flags (user_inc above) are merged in with priority over
     * project-inferred dirs (first match wins in the elaborator's
     * include search). */
    const char **spice_inc_dirs = NULL;
    int          n_spice_inc_dirs = 0;

    /* RM4: reader-macro definition files preloaded for the entry file.
     * Populated below during project-mode setup from the manifest's
     * `:reader-macros [...]` entry; stays NULL/0 in single-file mode.
     * `rm_paths_owned` is the heap-allocated backing storage (freed at
     * RUN_ENTRY's exit and at all early-return paths via the macro). */
    char       **rm_paths_owned = NULL;
    const char **rm_paths       = NULL;
    int          n_rm_paths     = 0;

    /* LS2 workspace-resolver ctx: populated by the explicit-file
     * auto-append below.  Stays zero-init for project mode (the gate in
     * elab_toplevel.c then matches no slots and the warning path is
     * inert). */
    Ls2ResolverCtx run_ls2 = {0};

    /* Helper: build 'entry', exec with optional passthrough args.
     * Cleans up user_inc, spice_inc_dirs, and auto_src_run on every exit. */
#define RUN_ENTRY(entry_path) do {                                       \
        char out_path[512]; \
        snprintf(out_path, sizeof(out_path), "%s/tur-run-XXXXXX", tur_temp_dir()); \
        int _fd = mkstemp(out_path);                                     \
        if (_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); free_reader_macro_paths(rm_paths_owned, n_rm_paths); for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned); ls2_resolver_ctx_dispose(&run_ls2); return 2; } \
        close(_fd);                                                      \
        tur_exe_path(out_path, sizeof(out_path));                        \
        int _n_inc = n_user_inc + n_spice_inc_dirs;                      \
        const char **_inc = NULL;                                        \
        if (_n_inc > 0) {                                                \
            _inc = (const char **)malloc((size_t)_n_inc * sizeof(char *)); \
            for (int _i = 0; _i < n_user_inc; _i++) _inc[_i] = user_inc[_i]; \
            for (int _i = 0; _i < n_spice_inc_dirs; _i++) _inc[n_user_inc + _i] = spice_inc_dirs[_i]; \
        }                                                                \
        ls2_resolver_ctx_set(&run_ls2);                                  \
        int _rc = cmd_build((entry_path), out_path, _inc, _n_inc, NULL, rm_paths, n_rm_paths); \
        ls2_resolver_ctx_set(NULL);                                      \
        free(_inc);                                                      \
        if (_rc != 0) { unlink(out_path); for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]); free(spice_inc_dirs); free(user_inc); free_reader_macro_paths(rm_paths_owned, n_rm_paths); for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned); ls2_resolver_ctx_dispose(&run_ls2); return _rc; } \
        Buf _cmd; buf_init(&_cmd);                                       \
        buf_printf(&_cmd, TUR_SHQ "%s" TUR_SHQ, out_path);               \
        if (passthrough_start >= 0) {                                    \
            for (int _i = passthrough_start; _i < argc; _i++)           \
                buf_printf(&_cmd, " " TUR_SHQ "%s" TUR_SHQ, argv[_i]);  \
        }                                                                \
        buf_putc(&_cmd, '\0');                                           \
        int _sys = system(_cmd.data);                                    \
        buf_free(&_cmd);                                                  \
        unlink(out_path);                                                \
        for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]); \
        free(spice_inc_dirs);                                            \
        free(user_inc);                                                  \
        free_reader_macro_paths(rm_paths_owned, n_rm_paths);             \
        for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned);                                              \
        ls2_resolver_ctx_dispose(&run_ls2);                              \
        return decode_exit_status(_sys);                                 \
    } while (0)

    /* SC4+SC5: in explicit-file mode, auto-discover the file's enclosing
     * spice and add its `src/` AND every :spices dep's src/ to the
     * include path so intra-spice and cross-spice imports resolve
     * without explicit -I (matching `tur check`).  Project mode below
     * sets up its own spice_inc_dirs from build.tur, so auto-discovery
     * is skipped there to avoid double-adding the same paths. */
    char **auto_run_owned = NULL;
    int    n_auto_run_owned = 0;
    if (explicit_file && strcmp(explicit_file, "-") != 0) {
        auto_append_spice_includes(explicit_file, &user_inc, &n_user_inc,
                                   &auto_run_owned, &n_auto_run_owned,
                                   &run_ls2);
    }

    /* Single-file mode: explicit file provided, skip project lookup. */
    if (explicit_file) {
        /* E6: treat "-" as stdin -- buffer it into a temp .tur file first. */
        if (strcmp(explicit_file, "-") == 0) {
            char src_tmp[512];
            snprintf(src_tmp, sizeof(src_tmp), "%s/tur-stdin-XXXXXX.tur", tur_temp_dir());
            int src_fd = mkstemps(src_tmp, 4);
            if (src_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); return 2; }
            char ibuf[4096]; size_t nr;
            int ok = 1;
            while ((nr = fread(ibuf, 1, sizeof(ibuf), stdin)) > 0)
                if ((size_t)write(src_fd, ibuf, nr) != nr) { ok = 0; break; }
            close(src_fd);
            if (!ok) {
                unlink(src_tmp);
                fprintf(stderr, "tur: error reading stdin\n");
                free(user_inc);
                return 2;
            }
            char out_path[512];
            snprintf(out_path, sizeof(out_path), "%s/tur-run-XXXXXX", tur_temp_dir());
            int out_fd = mkstemp(out_path);
            if (out_fd < 0) { unlink(src_tmp); fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); return 2; }
            close(out_fd);
            tur_exe_path(out_path, sizeof(out_path));
            /* SC2: stdin mode never enters project setup, so spice_inc_dirs
             * is empty here; we can pass user_inc straight through. */
            int brc = cmd_build(src_tmp, out_path,
                                (const char **)user_inc, n_user_inc, NULL,
                                NULL, 0);
            unlink(src_tmp);
            if (brc != 0) { unlink(out_path); free(user_inc); return brc; }
            Buf run_cmd; buf_init(&run_cmd);
            buf_printf(&run_cmd, TUR_SHQ "%s" TUR_SHQ, out_path);
            if (passthrough_start >= 0)
                for (int i = passthrough_start; i < argc; i++)
                    buf_printf(&run_cmd, " " TUR_SHQ "%s" TUR_SHQ, argv[i]);
            buf_putc(&run_cmd, '\0');
            int sys = system(run_cmd.data);
            buf_free(&run_cmd);
            unlink(out_path);
            free(user_inc);
            return decode_exit_status(sys);
        }
        /* RM4: in explicit-file mode, walk up from the file to discover an
         * enclosing build.tur and apply its `:reader-macros [...]` if any.
         * Mirrors the auto-include discovery a few lines above. */
        {
            char *sroot = find_spice_root(explicit_file);
            if (sroot) {
                char mp[4096];
                (void)pkg_resolve_manifest_path(sroot, mp, sizeof(mp));
                PkgManifest sm; memset(&sm, 0, sizeof(sm));
                if (pkg_manifest_read(mp, &sm)) {
                    rm_paths_owned = resolve_manifest_reader_macros(
                        sroot, &sm, &n_rm_paths);
                    rm_paths = (const char **)rm_paths_owned;
                }
                pkg_manifest_free(&sm);
                free(sroot);
            }
        }
        /* engine-selection-plan E3: a resolved non-cc engine delegates to
         * that engine's own arm; "cc" continues into RUN_ENTRY unchanged. */
        {
            const char *eng = resolve_engine(explicit_file, engine_flag);
            if (!eng) {
                free(user_inc);
                free_reader_macro_paths(rm_paths_owned, n_rm_paths);
                ls2_resolver_ctx_dispose(&run_ls2);
                return 2;
            }
            if (strcmp(eng, "cc") != 0) {
                int drc = run_delegate_engine(eng, explicit_file,
                                              user_inc, n_user_inc,
                                              argc, argv, passthrough_start);
                free(user_inc);
                free_reader_macro_paths(rm_paths_owned, n_rm_paths);
                ls2_resolver_ctx_dispose(&run_ls2);
                return drc;
            }
        }
        RUN_ENTRY(explicit_file);
    }

    /* Project mode: walk up to find build.tur. */
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "tur run: cannot get current directory\n");
        return 2;
    }

    char *root = find_project_root(cwd);
    if (!root) {
        fprintf(stderr,
            "tur run: no build.tur found (searched up from '%s')\n"
            "  Create a project with `tur new <name>`, "
            "or pass a file directly: tur run src/main.tur\n",
            cwd);
        return 1;
    }

    /* Parse manifest for entry point configuration. */
    char manifest_path[4096];
    if (!pkg_resolve_manifest_path(root, manifest_path,
                                   sizeof(manifest_path))) {
        free(root);
        return 1;
    }
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    if (!pkg_manifest_read(manifest_path, &m)) { free(root); return 1; }

    /* RM4: resolve manifest reader-macros against the spice root and
     * thread them through RUN_ENTRY/cmd_build below. The allocated
     * string array is owned by cmd_run for the remainder of this call;
     * release sites already free user_inc / spice_inc_dirs and now also
     * free this. */
    rm_paths_owned = resolve_manifest_reader_macros(root, &m, &n_rm_paths);
    rm_paths       = (const char **)rm_paths_owned;

    /* Read lock file. */
    char lock_path[4096];
    snprintf(lock_path, sizeof(lock_path), "%s/tur.lock", root);
    PkgLockFile lock;
    memset(&lock, 0, sizeof(lock));
    lock.format_version = 1;
    pkg_lock_read(lock_path, &lock);

    /* Spice dependency handling. */
    if (m.n_spices > 0) {
        char spices_dir[4096];
        snprintf(spices_dir, sizeof(spices_dir), "%s/spices", root);

        if (offline) {
            /* Verify all required spices exist on disk. */
            bool missing = false;
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                if (s->path) continue; /* local path -- always present */
                /* LS4: workspace-sibling :spices entries are local-source
                 * (the sibling lives under the enclosing workspace), so
                 * they cannot be "missing" the way a never-fetched URL
                 * dep can be.  Skip them here. */
                if (pkg_is_workspace_member(root, s->name)) continue;
                char dep_dir[4096];
                if (s->ref)
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                else
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                struct stat _dstat;
                if (stat(dep_dir, &_dstat) != 0 || !S_ISDIR(_dstat.st_mode)) {
                    fprintf(stderr,
                        "tur run: --offline: spice '%s' not found at '%s'\n",
                        s->name, dep_dir);
                    missing = true;
                }
            }
            if (missing) {
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                free(root);
                return 1;
            }
        } else {
            /* Fetch any missing spices; verify SHA-256 of already-fetched ones. */
            bool need_fetch = false;
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                if (s->path) continue;
                /* LS4: workspace siblings never trigger a remote fetch and
                 * have no lockfile row to check; the resolver picks them
                 * up via the workspace walk below. */
                if (pkg_is_workspace_member(root, s->name)) continue;
                char dep_dir[4096];
                if (s->ref)
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                else
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                struct stat _dstat;
                if (stat(dep_dir, &_dstat) != 0 || !S_ISDIR(_dstat.st_mode)) {
                    need_fetch = true;
                } else {
                    /* Verify SHA-256 matches lock (if lock has entry). */
                    PkgLockEntry *le = pkg_lock_find(&lock, s->name, false);
                    if (le && le->sha256) {
                        char actual_sha[65];
                        if (pkg_sha256_dir(dep_dir, actual_sha) &&
                            strcmp(actual_sha, le->sha256) != 0) {
                            fprintf(stderr,
                                "tur run: integrity check failed for '%s'.\n"
                                "  Run `tur fetch --update` to re-download.\n",
                                s->name);
                            pkg_lock_free(&lock);
                            pkg_manifest_free(&m);
                            free(root);
                            return 1;
                        }
                    }
                }
            }
            if (need_fetch) {
                /* LS5: partial-fetch isolation -- a single broken URL dep
                 * must not poison the search path for healthy ones.
                 * pkg_fetch_all continues past per-dep failures (sets ok
                 * false but doesn't break), so the on-disk state for
                 * successfully-fetched deps is consistent.  Write whatever
                 * lock entries we have and proceed; if the entry file
                 * actually imports a symbol from the missing dep, the
                 * elaborator's "module not found" diagnostic fires for
                 * that import alone.  Files that only reference healthy
                 * deps keep compiling. */
                bool fetch_ok = pkg_fetch_all(root, &m, &lock, false);
                pkg_lock_write(lock_path, &lock);
                if (!fetch_ok) {
                    fprintf(stderr,
                        "tur run: one or more dependencies failed to "
                        "fetch; continuing with healthy deps on disk. "
                        "Imports referencing the missing dep(s) will "
                        "fail with 'module not found'.\n");
                }
            }
        }

        /* Build the spice include-path array from the manifest via the
         * shared resolver. `tur run` project mode does not add the project's
         * own `src/` here -- the entry file lives inside `src/`, so the
         * resolver already searches it -- whereas `tur test` / `tur build
         * <dir>` request it (their inputs sit outside `src/`). */
        resolve_include_dirs_from_manifest(root, &m, /*include_own_src=*/false,
                                           &spice_inc_dirs, &n_spice_inc_dirs);
    }

    /* CMake dependency handling: generate and build if cmake-deps present.
     * Walk the enclosing manifest's :spices block transitively so a
     * workspace sibling's :cmake-deps participate in this TU's build --
     * see docs/archive/history/transitive-cmake-deps-plan.md. */
    PkgCmakeDep *closure_deps = NULL;
    int          n_closure_deps = 0;
    if (!pkg_collect_transitive_cmake_deps(root, &m,
                                           /*include_workspace_siblings=*/true,
                                           &closure_deps, &n_closure_deps)) {
        fprintf(stderr, "tur run: transitive cmake-deps resolution failed\n");
        pkg_lock_free(&lock);
        pkg_manifest_free(&m);
        for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]);
        free(spice_inc_dirs);
        free(root);
        return 1;
    }
    if (n_closure_deps > 0) {
        /* Build a synthetic manifest aliasing `m`'s other fields and
         * swapping in the unioned cmake_deps so pkg_gen_cmake_deps and
         * pkg_cmake_build see the full set. */
        PkgManifest mu = m;
        mu.cmake_deps   = closure_deps;
        mu.n_cmake_deps = n_closure_deps;

        char cmake_lists[4096];
        snprintf(cmake_lists, sizeof(cmake_lists), "%s/cmake/CMakeLists.txt", root);
        struct stat _cmst;
        bool cmake_built = (stat(cmake_lists, &_cmst) == 0);
        if (!cmake_built) {
            if (!pkg_gen_cmake_deps(root, &mu) ||
                !pkg_cmake_build(root, &mu, &lock, NULL)) {
                fprintf(stderr, "tur run: cmake dependency build failed\n");
                pkg_cmake_deps_free(closure_deps, n_closure_deps);
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]);
                free(spice_inc_dirs);
                free(root);
                return 1;
            }
            pkg_lock_write(lock_path, &lock);
        }
    }
    pkg_cmake_deps_free(closure_deps, n_closure_deps);

    pkg_lock_free(&lock);

    /* Entry point resolution (from the plan):
     *   1. :entry key in build.tur  (not yet in PkgManifest -- future)
     *   2. src/main.tur
     *   3. single .tur file in src/ */
    char entry[4096];
    struct stat _st;
    snprintf(entry, sizeof(entry), "%s/src/main.tur", root);
    if (stat(entry, &_st) != 0) {
        char src_dir[4096];
        snprintf(src_dir, sizeof(src_dir), "%s/src", root);
        int n_files = 0;
        char **files = collect_tur_files(src_dir, &n_files);
        if (n_files == 1) {
            strncpy(entry, files[0], sizeof(entry) - 1);
            entry[sizeof(entry) - 1] = '\0';
        } else {
            fprintf(stderr,
                "tur run: cannot determine entry point\n"
                "  Expected %s/src/main.tur, "
                "or exactly one .tur file in %s/src/\n",
                root, root);
            free_tur_files(files, n_files);
            pkg_manifest_free(&m);
            free(root);
            return 1;
        }
        free_tur_files(files, n_files);
    }

    pkg_manifest_free(&m);
    free(root);

    /* engine-selection-plan E3: project-mode engine dispatch.  The entry is
     * now known, so a manifest `:engine "interp"` makes a bare `tur run`
     * tree-walk this project (and --engine / TUR_ENGINE override it). */
    {
        const char *eng = resolve_engine(entry, engine_flag);
        if (!eng) {
            for (int _i = 0; _i < n_spice_inc_dirs; _i++)
                free((char *)spice_inc_dirs[_i]);
            free(spice_inc_dirs);
            free(user_inc);
            free_reader_macro_paths(rm_paths_owned, n_rm_paths);
            return 2;
        }
        if (strcmp(eng, "cc") != 0) {
            int drc = run_delegate_engine(eng, entry, user_inc, n_user_inc,
                                          argc, argv, passthrough_start);
            for (int _i = 0; _i < n_spice_inc_dirs; _i++)
                free((char *)spice_inc_dirs[_i]);
            free(spice_inc_dirs);
            free(user_inc);
            free_reader_macro_paths(rm_paths_owned, n_rm_paths);
            return drc;
        }
    }

    RUN_ENTRY(entry);
#undef RUN_ENTRY
}

/* Per-test directives, read from a test file's leading comment lines:
 *
 *   ;; tur-test-flags: --strict-refine     -- extra compile flags for THIS test
 *   ;; tur-test-expect-error: TUR-W0372    -- this test must FAIL to compile and
 *                                             its diagnostics must contain this
 *                                             text; the run phase is skipped.
 *
 * A file with no directive behaves exactly as before.  This is what lets a
 * refined ecs test enforce its proof (`--strict-refine`, so an unproven crossing
 * is a hard error, not a warning) and lets an expected-fail negative be a real
 * test rather than documentation.  `--enable=refined` needs no flag here: the
 * file's `#lang turmeric refined` line enables it for that compile. */
typedef struct TestDirectives {
    bool strict_refine;
    char expect_error[128];   /* empty => not an expect-error test */
} TestDirectives;

static void parse_test_directives(const char *path, TestDirectives *out) {
    out->strict_refine = false;
    out->expect_error[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    int scanned = 0;
    while (scanned < 40 && fgets(line, sizeof(line), f)) {
        scanned++;
        const char *d;
        if ((d = strstr(line, "tur-test-flags:")) != NULL) {
            if (strstr(d, "--strict-refine")) out->strict_refine = true;
        }
        if ((d = strstr(line, "tur-test-expect-error:")) != NULL) {
            d += strlen("tur-test-expect-error:");
            while (*d == ' ' || *d == '\t') d++;
            size_t k = 0;
            while (*d && *d != '\n' && *d != '\r' && k + 1 < sizeof(out->expect_error))
                out->expect_error[k++] = *d++;
            while (k > 0 && (out->expect_error[k-1] == ' ' || out->expect_error[k-1] == '\t')) k--;
            out->expect_error[k] = '\0';
        }
    }
    fclose(f);
}

static int cmd_test(const char *dir) {
    int n_files = 0;
    char **tur_files = collect_tur_files_deep(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }

    qsort(tur_files, (size_t)n_files, sizeof(char *), compare_cstr_ptrs);

    /* Bug 3 fix: derive `-I` include paths from the nearest build.tur so
     * test files can `(import test/suite ...)`, `(import plutovg/surface ...)`,
     * etc. without the caller having to pass `-I` flags manually.
     *
     * Includes:
     *   - the project's own `src/` directory (so e.g. plutovg tests can
     *     import plutovg/surface);
     *   - for each `:spices` entry in build.tur, the resolved spice's
     *     `src/` directory (so plutovg can pull in test/assert, etc.).
     *
     * The cmake-deps include/link flags are already wired up inside
     * `cmd_build` via `pkg_cmake_manifest_read`, so we don't redo that.
     */
    const char **spice_inc_dirs = NULL;
    int          n_spice_inc_dirs = 0;
    resolve_project_include_dirs(dir, &spice_inc_dirs, &n_spice_inc_dirs);

    int passed = 0;
    int failed = 0;
    char **failed_files = (char **)calloc((size_t)n_files, sizeof(char *));
    if (!failed_files) {
        fprintf(stderr, "tur: oom\n");
        free_tur_files(tur_files, n_files);
        for (int j = 0; j < n_spice_inc_dirs; j++) free((char *)spice_inc_dirs[j]);
        free(spice_inc_dirs);
        return 2;
    }

    for (int i = 0; i < n_files; i++) {
        char out_path[512];
        snprintf(out_path, sizeof(out_path), "%s/tur-test-XXXXXX", tur_temp_dir());
        int fd = mkstemp(out_path);
        if (fd < 0) {
            fprintf(stderr, "tur: mkstemp failed for %s\n", tur_files[i]);
            failed_files[failed++] = tur_files[i];
            putchar('F');
            continue;
        }
        close(fd);
        tur_exe_path(out_path, sizeof(out_path));

        TestDirectives td;
        parse_test_directives(tur_files[i], &td);
        bool prev_strict = g_strict_refine;
        if (td.strict_refine) g_strict_refine = true;
        bool expect_fail = td.expect_error[0] != '\0';

        /* For an expect-error test, capture stderr so we can confirm the
         * front-end emitted the named diagnostic (not some unrelated failure). */
        int stderr_bak = -1;
        char cap_path[512] = {0};
        if (expect_fail) {
            snprintf(cap_path, sizeof(cap_path), "%s/tur-test-err-XXXXXX", tur_temp_dir());
            int cfd = mkstemp(cap_path);
            if (cfd >= 0) {
                fflush(stderr);
                stderr_bak = dup(fileno(stderr));
                dup2(cfd, fileno(stderr));
                close(cfd);
            } else {
                cap_path[0] = '\0';
            }
        }

        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);
        int build_rc = cmd_build(tur_files[i], out_path,
                                  spice_inc_dirs, n_spice_inc_dirs, NULL,
                                  (const char **)rm_p, rm_n);
        free_reader_macro_paths(rm_p, rm_n);

        if (expect_fail && stderr_bak >= 0) {
            fflush(stderr);
            dup2(stderr_bak, fileno(stderr));
            close(stderr_bak);
        }
        g_strict_refine = prev_strict;

        if (expect_fail) {
            /* Pass iff the build FAILED and the captured diagnostics name the
             * expected error -- so a negative test cannot pass on an unrelated
             * compile error. The run phase is skipped. */
            char *cap = NULL;
            bool found = false;
            if (cap_path[0]) {
                FILE *cf = fopen(cap_path, "r");
                if (cf) {
                    cap = (char *)malloc(65536);
                    size_t got = cap ? fread(cap, 1, 65535, cf) : 0;
                    if (cap) cap[got] = '\0';
                    fclose(cf);
                }
                unlink(cap_path);
            }
            if (cap) found = strstr(cap, td.expect_error) != NULL;
            unlink(out_path);
            if (build_rc != 0 && found) {
                passed++;
                putchar('.');
            } else {
                failed_files[failed++] = tur_files[i];
                putchar('F');
                fprintf(stderr,
                        "\nEXPECT-ERROR %s: wanted diagnostic '%s' with a failed "
                        "build; got build_rc=%d, diagnostic %s\n",
                        tur_files[i], td.expect_error, build_rc,
                        found ? "found" : "NOT found");
                if (cap && !found && cap[0]) fprintf(stderr, "%s\n", cap);
            }
            free(cap);
            continue;
        }

        int run_rc = 1;
        if (build_rc == 0) {
            /* Quoted: the temp dir is user-controlled (TMP/TMPDIR) and may
             * contain spaces -- "C:\Users\Foo Bar\AppData\Local\Temp" is a
             * perfectly ordinary Windows path. */
            Buf test_cmd; buf_init(&test_cmd);
            buf_printf(&test_cmd, TUR_SHQ "%s" TUR_SHQ, out_path);
            buf_putc(&test_cmd, '\0');
            int status = system(test_cmd.data);
            buf_free(&test_cmd);
            run_rc = decode_exit_status(status);
        }
        unlink(out_path);

        if (build_rc == 0 && run_rc == 0) {
            passed++;
            putchar('.');
        } else {
            failed_files[failed++] = tur_files[i];
            putchar('F');
        }
    }

    if (use_json_output) {
        printf("{\"total\":%d,\"passed\":%d,\"failed\":%d,\"tests\":[",
               n_files, passed, failed);
        bool first = true;
        for (int i = 0; i < n_files; i++) {
            bool is_fail = false;
            for (int j = 0; j < failed; j++) {
                if (failed_files[j] == tur_files[i]) { is_fail = true; break; }
            }
            if (!first) printf(",");
            first = false;
            char esc[1024];
            json_escape(tur_files[i], esc, sizeof(esc));
            printf("{\"file\":\"%s\",\"status\":\"%s\"}", esc, is_fail ? "fail" : "pass");
        }
        printf("]}\n");
    } else {
        putchar('\n');
        printf("%d tests, %d passed, %d failed\n", n_files, passed, failed);
        for (int i = 0; i < failed; i++) {
            printf("FAIL %s\n", failed_files[i]);
        }
    }

    free(failed_files);
    free_tur_files(tur_files, n_files);
    for (int j = 0; j < n_spice_inc_dirs; j++) free((char *)spice_inc_dirs[j]);
    free(spice_inc_dirs);
    return failed == 0 ? 0 : 1;
}

/* Type-check every .tur file under a directory (no codegen kept).
 * Mirrors cmd_test's discovery so `tur check src/` works inside a spice
 * without a per-file path. Resolves intra-spice include dirs from the
 * nearest build.tur, exactly like cmd_test. Returns 0 iff all files pass. */
static int cmd_check_dir(const char *dir) {
    int n_files = 0;
    char **tur_files = collect_tur_files_deep(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    qsort(tur_files, (size_t)n_files, sizeof(char *), compare_cstr_ptrs);

    const char **inc = NULL;
    int          n_inc = 0;
    resolve_project_include_dirs(dir, &inc, &n_inc);

    int failed = 0;
    if (use_json_output) diag_lsp_begin();
    for (int i = 0; i < n_files; i++) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);
        Buf out;
        buf_init(&out);
        int rc = compile_to_c(tur_files[i], &out, inc, n_inc,
                              (const char **)rm_p, rm_n);
        buf_free(&out);
        free_reader_macro_paths(rm_p, rm_n);
        if (rc != 0) failed++;
    }
    if (use_json_output) { diag_lsp_flush(stdout); diag_lsp_end(); }

    free_tur_files(tur_files, n_files);
    for (int j = 0; j < n_inc; j++) free((char *)inc[j]);
    free(inc);
    return failed == 0 ? 0 : 1;
}

/* Build a project from multiple .tur files. Generates .h and .c for each,
 * plus a _main.c that includes all headers. */
/* RP0: `shared` selects shared-library build (skip _main.c, link with
 * -fPIC -shared, default output `lib<dir>.so`). The host can then
 * dlopen the result and dlsym exported defns.
 * RP1: `manifest_path` overrides the default exports.manifest location
 * (`<out_path>.manifest`). NULL means use the default. Ignored unless
 * `shared` is true. */
/* Core multi-file build.  Takes ownership of `tur_files` (frees via
 * free_tur_files) and compiles every file to .h/.c, links them (plus a
 * generated _main.c in executable mode), and writes the ABI cache under
 * `dir`.  `inc`/`n_inc` are include search dirs threaded into every
 * compile_to_h / compile_to_implementation call so cross-module imports
 * resolve; they are borrowed (not freed here). */
/* Mangle a module name for use as a C header/impl base name.
 * '/' -> "__", '-' -> '_', other non-identifier chars -> '_'.
 *
 * Deliberately keeps the LEGACY (lossy) fold for on-disk filenames:
 * injectivity matters for linker-visible C symbols but not for filesystem
 * paths -- and over-long "_hy"/"_xHH" expansions hurt readability.
 * Binding symbols in the generated C use the injective scheme (via
 * raw_name_for_binding / tur_mangle_append); only the header/impl base
 * names produced here stay legacy. See docs/guides/name-mangling-guide.md. */
static void mangle_mod_basename(const char *name, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; name[i] && k + 2 < cap; i++) {
        char c = name[i];
        if (c == '/') { out[k++] = '_'; out[k++] = '_'; }
        else if (c == '-') { out[k++] = '_'; }
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_') { out[k++] = c; }
        else { out[k++] = '_'; }
    }
    out[k] = '\0';
}

/* Derive a module name for one source file.  When `src_root` is non-NULL and
 * the file lives under it, the qualified path relative to src_root (minus
 * `.tur`) is used -- e.g. <root>/src + src/app/util.tur -> "app/util" -- which
 * matches the conventional `(defmodule app/util ...)` name so cross-module
 * imports and the generated header filenames line up.  Otherwise the bare
 * filename basename (minus `.tur`) is used. */
static char *derive_module_name(const char *file, const char *src_root) {
    if (src_root && *src_root) {
        size_t rlen = strlen(src_root);
        if (strncmp(file, src_root, rlen) == 0) {
            const char *rel = file + rlen;
            while (*rel == '/') rel++;
            size_t len = strlen(rel);
            if (len >= 4 && strcmp(rel + len - 4, ".tur") == 0) len -= 4;
            char *m = (char *)malloc(len + 1);
            memcpy(m, rel, len);
            m[len] = '\0';
            return m;
        }
    }
    const char *base = basename_of(file);
    size_t len = strlen(base);
    if (len >= 4 && strcmp(base + len - 4, ".tur") == 0) len -= 4;
    char *m = (char *)malloc(len + 1);
    memcpy(m, base, len);
    m[len] = '\0';
    return m;
}

/* Return true if `path` contains a top-level `(defn main` definition (an
 * executable entry point), false otherwise.  Treats a sweet-expr bare
 * `defn main` form as matching too.  Used to auto-detect library mode. */
static bool file_has_main_defn(const char *path) {
    char *src = NULL;
    size_t len = 0;
    if (read_entire_file_quiet(path, &src, &len) != 0) return false;
    bool found = false;
    for (size_t i = 0; i + 4 < len && !found; i++) {
        /* Match "(defn main" or "defn main" (sweet-exp) at a word boundary */
        bool sexpr = (strncmp(src + i, "(defn main", 10) == 0);
        bool sweet  = (i == 0 || src[i-1] == '\n') &&
                      strncmp(src + i, "defn main", 9) == 0;
        if (sexpr || sweet) {
            size_t skip = sexpr ? 10 : 9;
            char next = (i + skip < len) ? src[i + skip] : '\0';
            if (next == ' ' || next == '\t' || next == '\n' || next == '[')
                found = true;
        }
    }
    free(src);
    return found;
}

/* Return true if `path` carries a `#[used]` attribute -- a defn explicitly
 * retained with external C linkage because it is reached only through its
 * mangled C symbol (cross-module inline-C bridge or by-address C-ABI callback).
 * The reader admits whitespace between `#[` and the name, so accept that too.
 * Used to disqualify the single-main whole-program build shortcut: that path
 * inlines only the entry module's transitive *Turmeric* import closure, so a
 * sibling module reachable solely via raw extern would be dropped and its
 * symbol would dangle at link time.  Falling through to separate compilation
 * compiles and links every project module instead. */
static bool file_has_used_attr(const char *path) {
    char *src = NULL;
    size_t len = 0;
    if (read_entire_file_quiet(path, &src, &len) != 0) return false;
    bool found = false;
    for (size_t i = 0; i + 6 < len && !found; i++) {
        if (src[i] != '#' || src[i + 1] != '[') continue;
        size_t j = i + 2;
        while (j < len && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j + 4 <= len && strncmp(src + j, "used", 4) == 0) {
            char next = (j + 4 < len) ? src[j + 4] : '\0';
            if (next == ' ' || next == '\t' || next == ']') found = true;
        }
    }
    free(src);
    return found;
}

/* used-attr-whole-program: scan each -I module-search dir for `.tur` modules
 * that carry a #[used] attribute and return their slash-separated module names
 * (path relative to the include dir, sans ".tur").  cmd_build force-loads
 * these on the single-file/whole-program path so a #[used] defn reached only
 * via a raw mangled C symbol (no `(import)`) is still emitted -- the same
 * retention `tur build <project>` gets from its separate-compilation fallback.
 *
 * The entry file's own module is excluded (it is elaborated as the program's
 * top-level forms; force-loading it as a module would double-define it).
 * #[used] is rare, so the common case is a cheap read-and-no-match per `.tur`
 * file; realpath is only paid for the few files that actually match.
 * Heap array; free via free_tur_files. */
static char **collect_used_attr_modules(const char *entry_path,
                                        const char **include_dirs,
                                        int n_include_dirs, int *n_out) {
    *n_out = 0;
    if (!include_dirs || n_include_dirs <= 0) return NULL;
    char *entry_real = (entry_path && entry_path[0]) ? realpath(entry_path, NULL)
                                                     : NULL;
    char **names = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < n_include_dirs; i++) {
        const char *dir = include_dirs[i];
        if (!dir || !*dir) continue;
        char **files = NULL;
        int nf = 0, capf = 0;
        collect_tur_recursive(dir, &files, &nf, &capf);
        size_t dlen = strlen(dir);
        for (int j = 0; j < nf; j++) {
            if (!file_has_used_attr(files[j])) continue;
            /* Exclude the entry's own module (compare canonical paths). */
            if (entry_real) {
                char *fr = realpath(files[j], NULL);
                bool is_entry = (fr && strcmp(fr, entry_real) == 0);
                free(fr);
                if (is_entry) continue;
            }
            /* Module name = path relative to the include dir, minus ".tur". */
            const char *rel = files[j];
            if (strncmp(rel, dir, dlen) == 0 && rel[dlen] == '/')
                rel += dlen + 1;
            else
                rel = basename_of(files[j]);
            size_t rlen = strlen(rel);
            if (rlen > 4 && strcmp(rel + rlen - 4, ".tur") == 0) rlen -= 4;
            if (rlen == 0) continue;
            char *mod = (char *)malloc(rlen + 1);
            if (!mod) { fprintf(stderr, "tur: oom\n"); abort(); }
            memcpy(mod, rel, rlen);
            mod[rlen] = '\0';
            bool dup = false;
            for (int k = 0; k < n; k++)
                if (strcmp(names[k], mod) == 0) { dup = true; break; }
            if (dup) { free(mod); continue; }
            if (n >= cap) {
                cap = cap ? cap * 2 : 4;
                names = (char **)realloc(names, (size_t)cap * sizeof(char *));
                if (!names) { fprintf(stderr, "tur: oom\n"); abort(); }
            }
            names[n++] = mod;
        }
        free_tur_files(files, nf);
    }
    free(entry_real);
    *n_out = n;
    return names;
}

/* n_own: number of files at the front of tur_files that belong to the
 * project itself and should be linked into the output.  Dep modules beyond
 * n_own are compiled only to generate headers (so importers can #include
 * them) but their .c files are excluded from the link step.  Pass n_files
 * (== n_own) when there are no dep-only files. */
static int cmd_build_multi_files(char **tur_files, int n_files,
                                 int n_own,
                                 const char *dir, const char *src_root,
                                 const char **file_src_roots,
                                 const char *out_path,
                                 bool shared, const char *manifest_path,
                                 const char **inc, int n_inc,
                                 const char *build_dir) {
    /* DEDUP-4b: an EXECUTABLE links here, so the rc<T>/GC runtime may come from
     * the archive rather than being replicated into every module TU.
     *
     * A `--shared` build must not: the archive is never injected into a shared
     * library's link line, so declaring-without-defining would leave the .so
     * with undefined rc_cb_alloc / rc_cb_alloc_struct -- a shared object
     * tolerates unresolved symbols at link time and only fails, or silently
     * binds to whatever the host exports, at dlopen.  A .so stays
     * self-contained on the DEDUP-3 owner-TU replica (already one instance per
     * library), which is also the right shape: a dlopened .so carrying its own
     * collector must not half-share one with its host.
     *
     * Caught by build-shared-rc-runtime in tests/run-build-project.sh, which
     * asserts exactly one owning definition of gc_all_blocks in the .so. */
    g_emit_for_link = !shared;
    /* build-output-directory-plan: every intermediate (.c/.h/_main.c/
     * tur_runtime.{c,h}) lands under `<build_dir>/obj/`; the final exe goes
     * to `<build_dir>/bin/<name>` and shared libs to `<build_dir>/lib/lib<name>.so`.
     * build_dir is required: callers must call resolve_build_dir() first. */
    char obj_dir[PATH_MAX + 8];
    snprintf(obj_dir, sizeof(obj_dir), "%s/obj", build_dir);

    char chosen_out[PATH_MAX + 32];
    if (!out_path) {
        /* Prefer the manifest's :name over the directory basename so a
         * workspace member built from `.` gets `lib<pkg-name>.so` rather
         * than `lib<workspace-cwd>.so` -- and so a single-spice build
         * from `.` cannot regress to `lib..so` if default_output_name's
         * realpath fallback ever fails. */
        char manifest_base[1024] = {0};
        {
            char *proj_root = find_project_root(dir);
            if (proj_root) {
                char mp[4096];
                (void)pkg_resolve_manifest_path(proj_root, mp, sizeof(mp));
                PkgManifest mm;
                memset(&mm, 0, sizeof(mm));
                if (pkg_manifest_read(mp, &mm) && mm.name && mm.name[0]) {
                    snprintf(manifest_base, sizeof(manifest_base),
                             "%s", mm.name);
                }
                pkg_manifest_free(&mm);
                free(proj_root);
            }
        }
        char base[1024];
        if (shared) {
            if (manifest_base[0]) {
                snprintf(base, sizeof(base), "%s", manifest_base);
            } else {
                default_output_name(dir, base, sizeof(base));
            }
            snprintf(chosen_out, sizeof(chosen_out),
                     "%s/lib/lib%s.so", build_dir, base);
        } else {
            if (manifest_base[0]) {
                snprintf(base, sizeof(base), "%s", manifest_base);
            } else {
                default_output_name(dir, base, sizeof(base));
            }
            snprintf(chosen_out, sizeof(chosen_out),
                     "%s/bin/%s", build_dir, base);
        }
        out_path = chosen_out;
    }

    /* Allocate arrays for .h and .c filenames */
    char **h_files = (char **)malloc(n_files * sizeof(char *));
    char **c_files = (char **)malloc(n_files * sizeof(char *));
    char **mod_names = (char **)malloc(n_files * sizeof(char *));
    if (!h_files || !c_files || !mod_names) { fprintf(stderr, "tur: oom\n"); return 2; }

    /* RP1: in shared mode, accumulate one exports.manifest line per
     * exported defn across all compiled modules, then write the file
     * once after the link succeeds. NULL in executable mode -- nothing
     * dlopens an executable, so the manifest would be unused. */
    Buf manifest;
    buf_init(&manifest);
    Buf *manifest_ptr = shared ? &manifest : NULL;

    /* Generate module names + the .h/.c basenames they map to.  In project
     * mode (src_root set) the module name is the qualified path under src/
     * (e.g. "app/util") and the generated files are named by its mangled form
     * ("app__util.h") so cross-module `#include "app__util.h"` resolves. */
    for (int i = 0; i < n_files; i++) {
        /* T3: cross-spice dep modules are named relative to their own dep
         * src/ (via file_src_roots[i]) so e.g. sib/api maps to sib__api.h,
         * matching the importer's generated `#include "sib__api.h"`.  Project
         * modules fall back to the single src_root. */
        const char *sr = (file_src_roots && file_src_roots[i])
                             ? file_src_roots[i] : src_root;
        mod_names[i] = derive_module_name(tur_files[i], sr);
        char mangled[512];
        mangle_mod_basename(mod_names[i], mangled, sizeof(mangled));
        size_t mlen = strlen(mangled);
        size_t prefix_len = strlen(obj_dir) + 1;  /* "obj_dir/" */
        size_t need = prefix_len + mlen + 3;
        h_files[i] = (char *)malloc(need);
        c_files[i] = (char *)malloc(need);
        snprintf(h_files[i], need, "%s/%s.h", obj_dir, mangled);
        snprintf(c_files[i], need, "%s/%s.c", obj_dir, mangled);
    }

    /* J6: Two-pass ABI specialization.
     * Pass 1: compile all modules with no forced specs; collect borrow specs.
     * Pass 2: recompile modules that own borrow specs from other modules. */
    BorrowSpecInfo **all_borrow = (BorrowSpecInfo **)calloc(n_files, sizeof(BorrowSpecInfo *));
    uint32_t *all_n_borrow = (uint32_t *)calloc(n_files, sizeof(uint32_t));
    if (!all_borrow || !all_n_borrow) { fprintf(stderr, "tur: oom\n"); return 2; }

    /* Pass 1: Compile each .tur file to .h and .c (no forced specs yet). */
    for (int i = 0; i < n_files; i++) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);

        Buf h_out;
        buf_init(&h_out);
        if (compile_to_h(tur_files[i], &h_out, mod_names[i], inc, n_inc,
                         (const char **)rm_p, rm_n, NULL, 0) != 0) {
            fprintf(stderr, "tur: failed to compile %s to header\n", tur_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&h_out, h_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", h_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&h_out);

        Buf c_out;
        buf_init(&c_out);
        if (compile_to_implementation(tur_files[i], &c_out, mod_names[i],
                                      inc, n_inc,
                                      (const char **)rm_p, rm_n,
                                      manifest_ptr,
                                      NULL, 0,
                                      &all_borrow[i], &all_n_borrow[i]) != 0) {
            fprintf(stderr, "tur: failed to compile %s to implementation\n", tur_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        /* Hoist `__tur_include__` directives to file scope (same treatment
         * as the single-file cmd_build path) -- required for spice modules
         * that include headers with top-level `static inline` declarations
         * such as mbedTLS. */
        hoist_tur_include_directives(&c_out);
        if (buf_to_path(&c_out, c_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", c_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&c_out);
        free_reader_macro_paths(rm_p, rm_n);
    }

    /* J6: Post-pass 1: build forced spec lists for each owner module. */
    ForcedAbiSpec **forced_for = (ForcedAbiSpec **)calloc(n_files, sizeof(ForcedAbiSpec *));
    uint32_t *n_forced_for = (uint32_t *)calloc(n_files, sizeof(uint32_t));
    uint32_t *cap_forced_for = (uint32_t *)calloc(n_files, sizeof(uint32_t));
    if (!forced_for || !n_forced_for || !cap_forced_for) { fprintf(stderr, "tur: oom\n"); return 2; }

    for (int i = 0; i < n_files; i++) {
        for (uint32_t bi = 0; bi < all_n_borrow[i]; bi++) {
            BorrowSpecInfo *bsi = &all_borrow[i][bi];
            if (!bsi->owning_module || !bsi->fn_symbol) continue;
            /* Find the owner module index. */
            int owner = -1;
            for (int j = 0; j < n_files; j++) {
                if (strcmp(mod_names[j], bsi->owning_module) == 0) { owner = j; break; }
            }
            if (owner < 0) continue;
            /* Check if this clone_name is already forced for owner. */
            bool dup = false;
            for (uint32_t fi = 0; fi < n_forced_for[owner]; fi++) {
                if (strcmp(forced_for[owner][fi].clone_name, bsi->clone_name) == 0) {
                    dup = true; break;
                }
            }
            if (dup) continue;
            /* Grow the forced array for the owner. */
            if (n_forced_for[owner] >= cap_forced_for[owner]) {
                uint32_t nc = cap_forced_for[owner] ? cap_forced_for[owner] * 2 : 4;
                ForcedAbiSpec *nf = (ForcedAbiSpec *)realloc(forced_for[owner],
                                                              nc * sizeof(ForcedAbiSpec));
                if (!nf) { fprintf(stderr, "tur: oom\n"); return 2; }
                forced_for[owner] = nf;
                cap_forced_for[owner] = nc;
            }
            ForcedAbiSpec *fs = &forced_for[owner][n_forced_for[owner]++];
            fs->clone_name = bsi->clone_name; /* borrow (valid until all_borrow freed) */
            fs->fn_symbol  = bsi->fn_symbol;
            fs->result_kind = bsi->result_kind;
            fs->n_args = bsi->n_args;
            for (uint8_t ai = 0; ai < bsi->n_args; ai++)
                fs->arg_kinds[ai] = bsi->arg_kinds[ai];
        }
    }

    /* J6: Pass 2: recompile owner modules that have forced specs. */
    for (int i = 0; i < n_files; i++) {
        if (n_forced_for[i] == 0) continue;

        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);

        Buf h_out;
        buf_init(&h_out);
        if (compile_to_h(tur_files[i], &h_out, mod_names[i], inc, n_inc,
                         (const char **)rm_p, rm_n,
                         forced_for[i], n_forced_for[i]) != 0) {
            fprintf(stderr, "tur: failed to recompile %s to header (pass 2)\n", tur_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&h_out, h_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s (pass 2)\n", h_files[i]);
            buf_free(&h_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&h_out);

        Buf c_out;
        buf_init(&c_out);
        if (compile_to_implementation(tur_files[i], &c_out, mod_names[i],
                                      inc, n_inc,
                                      (const char **)rm_p, rm_n,
                                      manifest_ptr,
                                      forced_for[i], n_forced_for[i],
                                      NULL, NULL) != 0) {
            fprintf(stderr, "tur: failed to recompile %s to implementation (pass 2)\n", tur_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        hoist_tur_include_directives(&c_out);
        if (buf_to_path(&c_out, c_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s (pass 2)\n", c_files[i]);
            buf_free(&c_out);
            free_reader_macro_paths(rm_p, rm_n);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); free(mod_names[j]); }
            free(h_files); free(c_files); free(mod_names);
            free(forced_for[i]); free(forced_for); free(n_forced_for); free(cap_forced_for);
            free(all_borrow); free(all_n_borrow);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&c_out);
        free_reader_macro_paths(rm_p, rm_n);
    }

    /* J5: Write .tur-abi-cache/index with ownership information.
     * J6: skipped entirely when the cache is disabled (--no-abi-cache /
     * TUR_NO_ABI_CACHE); the build is still correct without it. */
    if (!g_no_abi_cache) {
        /* build-output-directory-plan: .tur-abi-cache/ lives inside the build
         * dir; the dir's own auto-.gitignore (`*`) already covers it, so no
         * separate gitignore append is needed. */
        char cache_dir[PATH_MAX + 32];
        snprintf(cache_dir, sizeof(cache_dir), "%s/.tur-abi-cache", build_dir);
        struct stat st_cache;
        if (stat(cache_dir, &st_cache) != 0) mkdir(cache_dir, 0755);
        char idx_tmp[PATH_MAX + 48];
        snprintf(idx_tmp, sizeof(idx_tmp), "%s/index.tmp", cache_dir);
        FILE *cf = fopen(idx_tmp, "w");
        if (cf) {
            for (int i = 0; i < n_files; i++) {
                for (uint32_t bi = 0; bi < all_n_borrow[i]; bi++) {
                    BorrowSpecInfo *bsi = &all_borrow[i][bi];
                    if (!bsi->owning_module || !bsi->fn_symbol) continue;
                    /* clone_name, owning_module, fn_symbol, result_kind, n_args, arg_kinds... */
                    fprintf(cf, "%s\t%s\t%s\t%d\t%d",
                            bsi->clone_name, bsi->owning_module, bsi->fn_symbol,
                            (int)bsi->result_kind, (int)bsi->n_args);
                    for (uint8_t ai = 0; ai < bsi->n_args; ai++)
                        fprintf(cf, "\t%d", (int)bsi->arg_kinds[ai]);
                    fprintf(cf, "\n");
                }
            }
            fclose(cf);
            char idx_path[PATH_MAX + 48];
            snprintf(idx_path, sizeof(idx_path), "%s/index", cache_dir);
            rename(idx_tmp, idx_path);
        }
    }

    /* Free borrow and forced spec arrays. */
    for (int i = 0; i < n_files; i++) {
        for (uint32_t bi = 0; bi < all_n_borrow[i]; bi++) {
            free(all_borrow[i][bi].clone_name);
            free(all_borrow[i][bi].owning_module);
            free(all_borrow[i][bi].fn_symbol);
        }
        free(all_borrow[i]);
        free(forced_for[i]);
    }
    free(all_borrow); free(all_n_borrow);
    free(forced_for); free(n_forced_for); free(cap_forced_for);

    /* build-output-directory-plan: _main.c / tur_runtime.{c,h} all land in
     * `<build_dir>/obj/` next to the per-module .c/.h pairs. */
    char main_c_path[PATH_MAX + 32];
    char rt_c_path[PATH_MAX + 32];
    char rt_h_path[PATH_MAX + 32];
    snprintf(main_c_path, sizeof(main_c_path), "%s/_main.c", obj_dir);
    snprintf(rt_c_path,   sizeof(rt_c_path),   "%s/tur_runtime.c", obj_dir);
    snprintf(rt_h_path,   sizeof(rt_h_path),   "%s/tur_runtime.h", obj_dir);

    /* Generate _main.c (skipped in shared-library mode: the .so has no
     * entry point; the host invokes exported defns directly via dlsym). */
    if (!shared) {
        Buf main_c;
        buf_init(&main_c);
        if (generate_main_c(&main_c, (const char **)h_files, n_files, out_path) != 0) {
            fprintf(stderr, "tur: failed to generate _main.c\n");
            buf_free(&main_c);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&main_c, main_c_path) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", main_c_path);
            buf_free(&main_c);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&main_c);
    }

    /* project-mode-rc-runtime-preamble-missing (owner-TU design): every module
     * .h now #includes "tur_runtime.h" for the shared runtime (RcControlBlock,
     * rc_cb_alloc, tur_frame, the GC, ...).  Generate that header plus a single
     * owner TU tur_runtime.c -- it alone #defines TUR_RT_OWNER, so it alone
     * defines the runtime's file-scope globals (one GC registry / free queue /
     * panic + scheduler state) while every module TU carries static replicas of
     * the runtime functions that operate on those shared globals.
     *
     * DEDUP-3 (docs/archive/gc-cycle-collection-plan.md): the rc<T>/GC family is no longer
     * replicated -- its definitions sit inside the same TUR_RT_OWNER guard as
     * the globals, so the owner TU carries the one externally-linked copy of
     * the collector and the other module TUs see only prototypes.
     *
     * Linked into the output below; cleaned up alongside _main.c. */
    {
        Buf rt_h; buf_init(&rt_h);
        resolve_rcgc_from_archive();   /* DEDUP-4b: before emitting the header */
        emit_shared_runtime_header(&rt_h);
        Buf rt_c; buf_init(&rt_c);
        buf_puts(&rt_c, "/* generated by tur -- shared runtime owner TU */\n");
        buf_puts(&rt_c, "#define TUR_RT_OWNER 1\n");
        buf_puts(&rt_c, "#include \"tur_runtime.h\"\n");
        int rt_ok = (buf_to_path(&rt_h, rt_h_path) == 0 &&
                     buf_to_path(&rt_c, rt_c_path) == 0);
        buf_free(&rt_h); buf_free(&rt_c);
        if (!rt_ok) {
            fprintf(stderr, "tur: failed to write tur_runtime.{h,c}\n");
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
    }

    /* Compile everything together with cc */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    /* See the note on -fno-strict-aliasing and the GCC-14 warning policy above. */
    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags) cc_flags = "-O2 -std=c99 -Wall -fno-strict-aliasing";

    /* Collect cmake dep flags from cmake/spice-deps-manifest.json if present */
    Buf cmake_flags;
    buf_init(&cmake_flags);
    {
        char *proj_root = find_project_root(dir);
        if (proj_root) {
            char manifest_path[4096];
            snprintf(manifest_path, sizeof(manifest_path),
                     "%s/cmake/spice-deps-manifest.json", proj_root);
            PkgCmakeManifest cmake_manifest;
            if (pkg_cmake_manifest_read(manifest_path, &cmake_manifest)) {
                pkg_cmake_manifest_append_cc_flags(&cmake_manifest, &cmake_flags);
                pkg_cmake_manifest_free(&cmake_manifest);
            }
            free(proj_root);
        }
    }

    /* spices-c-sources-plan: collect the project manifest's :c-includes (as
     * -I flags, visible to both the aux .c compile and the spice's inline-C)
     * and :c-sources (vendored hand-written C, compiled as additional
     * translation units and linked into this binary). Paths are resolved
     * relative to the manifest directory. */
    Buf aux_includes; buf_init(&aux_includes);
    Buf aux_sources;  buf_init(&aux_sources);
    {
        char *proj_root = find_project_root(dir);
        if (proj_root) {
            collect_spice_aux_c(proj_root, &aux_includes, &aux_sources);
            free(proj_root);
        }
    }

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s", cc, cc_flags);
    /* Vendored C include dirs come early so both the generated module .c
     * (carrying inline-C) and the aux .c sources can find their headers. */
    if (aux_includes.len > 0) buf_puts(&cmd, aux_includes.data);
    /* RP0: shared-library link gets -fPIC -shared and skips _main.c.
     * Position-independent code is required on Linux/macOS for symbols
     * loaded via dlopen; -shared tells the driver to emit a .so/.dylib
     * instead of an executable. On macOS, -undefined dynamic_lookup allows
     * cross-library symbols (e.g. httpd in tourist) to resolve at load time. */
    if (shared) {
        buf_puts(&cmd, " -fPIC -shared");
#ifdef __APPLE__
        buf_puts(&cmd, " -undefined dynamic_lookup");
#endif
    }
    buf_printf(&cmd, " -o %s", out_path);
    /* build-output-directory-plan: every generated .c includes its sibling .h
     * by bare name (`#include "modname.h"`), so the obj dir must be on the
     * include path even when it's not cwd. */
    buf_printf(&cmd, " -I%s", obj_dir);
    /* Add _main.c first (executable mode only) */
    if (!shared) buf_printf(&cmd, " %s", main_c_path);
    /* The shared-runtime owner TU (defines the runtime globals once). */
    buf_printf(&cmd, " %s", rt_c_path);
    /* Add own .c files only (dep-only files beyond n_own supply headers but
     * are not linked into this output -- they ship as separate libraries). */
    for (int i = 0; i < n_own; i++) {
        buf_printf(&cmd, " %s", c_files[i]);
    }
    /* spices-c-sources-plan: vendored .c sources compiled alongside the
     * spice's own translation units, before -lm so they can resolve math
     * symbols against it. */
    if (aux_sources.len > 0) buf_puts(&cmd, aux_sources.data);
    buf_free(&aux_includes);
    buf_free(&aux_sources);
    /* Append cmake dep flags (-I/-L/-l). */
    if (cmake_flags.len > 0) buf_puts(&cmd, cmake_flags.data);
    buf_free(&cmake_flags);
    /* Always link libm last. Pure spices can reference libm symbols
     * (sqrt/fabs/sin/cos/...) directly from inline-C, and static spice deps
     * (e.g. plutovg) reference them without carrying -lm themselves. GNU ld
     * resolves archives left-to-right, so -lm must come AFTER any
     * -l<staticlib> flags or the math symbols go unresolved -- hence it is
     * appended last. On Linux an unused -lm is a harmless no-op; on macOS
     * libm lives in libSystem so -lm is a no-op there too. Linking it
     * unconditionally means libm is usable from a pure spice without forcing
     * a fake cmake-dep just to pull it in. */
    /* GCC 14 / Apple clang 15+ promoted -Wincompatible-pointer-types and
     * -Wint-conversion from warnings to hard errors.  The generated C used to
     * trip both -- carrier<->concrete representation straddles (void*<->int64_t)
     * -- tracked under docs/archive/history/codegen-gcc14-permerrors.md and
     * docs/archive/macos-clang-int-conversion-hard-error.md.  Every straddle is
     * now bridged at emit time (String returns, cloneable-frame call args,
     * cps->direct spawn/void* params, closure-env void* fields, and __ps_N
     * binder-init crossings), and the whole fixture tree emits 0
     * -Wint-conversion / -Wincompatible-pointer-types hard errors, so the two
     * downgrades are removed -- a NEW straddle now fails the build (as intended)
     * instead of hiding behind the macOS CI red.
     *
     * -Wno-error=implicit-function-declaration used to be appended here and at
     * the two sibling cc invocations.  It is gone from all three: the two things
     * it was covering are fixed at the source -- the undeclared
     * `tur_hamt_hash_xxh64` call (now declared in the preamble) and the
     * `__tur_include__` hoist emitting code payloads ahead of directive payloads
     * (hoist_tur_include_directives now buckets the two).  An implicit
     * declaration is UB, and suppressing it here is what let a wrong-code bug
     * ride all the way to a second backend on another platform; it now fails the
     * build, as intended.
     *
     * Do NOT re-add it to paper over a new implicit declaration -- fix the
     * declaration.  And do not verify any change in this area with `emit-c`
     * output or a plain `tests/run.sh`: neither exercises the hoist path, and
     * both reported a false all-clear on exactly this question.  Use
     * tools/jit-spike/sweep-turjit.sh.  See findings 21.2/21.3 and
     * docs/archive/history/hoisted-inline-c-precedes-includes.md. */
    buf_puts(&cmd, " -lm");
#ifdef _WIN32
    /* The emitted runtime uses pthread_mutex_t/pthread_cond_t and select().
     * On glibc both live in libc; MinGW puts pthreads in winpthreads and
     * select() in Winsock, so they must be linked explicitly.  This is a LINK
     * decision about the host toolchain, not codegen, so keying it off the
     * host is correct -- the generated C itself stays portable. */
    buf_puts(&cmd, " -lpthread -lws2_32 -lshlwapi");
#endif
    /* Ensure null termination before passing to system(). */
    buf_putc(&cmd, '\0');
    int sys_rc = system(cmd.data);
    buf_free(&cmd);

    /* Clean up temp files */
    for (int i = 0; i < n_files; i++) { free(h_files[i]); free(c_files[i]); free(mod_names[i]); }
    free(h_files); free(c_files); free(mod_names);
    free_tur_files(tur_files, n_files);
    /* build-output-directory-plan: leave the intermediates in <build_dir>/obj/
     * so re-runs can ccache them and so users can inspect generated C. The
     * build dir is .gitignore'd on creation, so they don't leak into VCS. */
    (void)main_c_path; (void)rt_c_path; (void)rt_h_path;

    /* Windows: the linker may have appended ".exe" to a -o with no extension.
     * Put the binary back where the caller asked for it.  (Shared builds pass
     * an out_path that already carries an extension, so this is inert there.) */
    if (sys_rc == 0 && tur_settle_exe_output(out_path) != 0) {
        fprintf(stderr, "tur: could not place the linked binary at '%s'\n", out_path);
        return 2;
    }

    if (sys_rc != 0) {
        buf_free(&manifest);
        fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }

    /* RP1: write exports.manifest alongside the .so. Default location is
     * `<out_path>.manifest` (lives next to the library); --manifest <path>
     * overrides. The host (REPL / FFI dispatcher) reads this file to map
     * `<module>/<defn>` -> mangled C symbol + type signature. */
    if (shared) {
        char default_mp[2048];
        if (!manifest_path) {
            snprintf(default_mp, sizeof(default_mp), "%s.manifest", out_path);
            manifest_path = default_mp;
        }
        FILE *mf = fopen(manifest_path, "wb");
        if (!mf) {
            fprintf(stderr, "tur: cannot open %s for write: %s\n",
                    manifest_path, strerror(errno));
            buf_free(&manifest);
            return 2;
        }
        if (manifest.len > 0 &&
            fwrite(manifest.data, 1, manifest.len, mf) != manifest.len) {
            fprintf(stderr, "tur: failed writing %s\n", manifest_path);
            fclose(mf);
            buf_free(&manifest);
            return 2;
        }
        fclose(mf);
    }
    buf_free(&manifest);
    return 0;
}

/* Bare-directory build: glob the directory's top-level .tur files and build
 * them.  Used for ad-hoc multi-file directories with no build.tur manifest. */
static int cmd_build_multi(const char *dir, const char *out_path, bool shared,
                           const char *manifest_path, const char *cli_build_dir) {
    int n_files;
    char **tur_files = collect_tur_files_deep(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    char *build_dir = resolve_build_dir(dir, cli_build_dir);
    if (!build_dir) { free_tur_files(tur_files, n_files); return 2; }
    /* `dir` is the module root for the files just collected, so put it on the
     * include path: now that the walk is recursive, `src/demo/lib.tur` is in
     * the set, and the sibling that does `(import demo/lib)` has to be able to
     * resolve it.  Finding the files and being unable to link them is only half
     * a fix -- and `tur build src/` is what the `module not found` diagnostic
     * tells the user to run.  Project mode passes its own resolved path here;
     * this is the bare-directory equivalent. */
    const char *self_inc[1] = { dir };
    int rc = cmd_build_multi_files(tur_files, n_files, n_files, dir, dir, NULL,
                                   out_path, shared, manifest_path, self_inc, 1,
                                   build_dir);
    free(build_dir);
    return rc;
}

/* Manifest-driven project build: `tur build <dir>` where <dir>/build.tur
 * exists.  Collects the project's module files (recursively under `src/`),
 * resolves the include search path from the manifest (own src/ + each
 * `:spices` dep's src/), merges in any user `-I` dirs (which take priority),
 * and builds the whole module set.  Mirrors the project resolution `tur run`
 * and the test runner already perform. */
static int cmd_build_project(const char *root_in, const char *out_path,
                             bool shared, const char *manifest_path,
                             const char **user_inc, int n_user_inc,
                             const char *cli_build_dir) {
    /* Resolve `root_in` to an absolute path so transitive-dep walking can
     * climb out via `:path "../sibling"` references.  When `tur build .` is
     * run from a spice directory, `root_in` is "." and the dep-resolution
     * fallback walks up cannot reach the actual sibling.  realpath() fixes
     * that with no behavioral change for already-absolute callers. */
    char root_buf[4096];
    const char *root = root_in;
    if (root_in && root_in[0] != '/') {
        if (realpath(root_in, root_buf)) root = root_buf;
    }

    int n_files = 0;
    char **tur_files = collect_project_src_files(root, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr,
                "tur: no .tur source files found under '%s/src' (or '%s')\n",
                root, root);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    /* Deterministic order so generated artifacts and the ABI cache are
     * reproducible across runs. */
    qsort(tur_files, (size_t)n_files, sizeof(char *), compare_cstr_ptrs);

    /* spices-c-sources-plan: validate the manifest up front. A manifest that
     * fails to parse -- e.g. an invalid :c-sources entry (missing file, wrong
     * extension, absolute path) -- is a hard build error, not a silently
     * ignored read. Doing this here means the parser's diagnostics translate
     * into a failed build (non-zero exit) instead of the build proceeding as
     * though the offending keys were absent. */
    {
        char mpath[4096];
        if (pkg_resolve_manifest_path(root, mpath, sizeof(mpath))) {
            PkgManifest vm; memset(&vm, 0, sizeof(vm));
            bool vok = pkg_manifest_read(mpath, &vm);
            pkg_manifest_free(&vm);
            if (!vok) {
                fprintf(stderr, "tur: invalid manifest '%s'\n", mpath);
                free_tur_files(tur_files, n_files);
                return 1;
            }
        }
    }

    /* Transitive cmake-deps autobuild.  Walks the manifest's `:spices`
     * closure (but NOT every workspace sibling -- see
     * docs/archive/history/tur-build-cmake-deps-workspace-overreach.md) and gen +
     * builds the union of their `:cmake-deps` into `<root>/cmake/`, mirroring
     * what cmd_run does for `tur run`.  Without this, a spice that imports
     * the json modules (which pull in yyjson) generates headers fine but
     * fails at cc with `yyjson.h not found`.  Skipped when
     * `cmake/CMakeLists.txt` already exists so reruns are cheap. */
    {
        char mpath[4096];
        if (pkg_resolve_manifest_path(root, mpath, sizeof(mpath))) {
            PkgManifest dm; memset(&dm, 0, sizeof(dm));
            if (pkg_manifest_read(mpath, &dm)) {
                PkgCmakeDep *closure = NULL;
                int n_closure = 0;
                if (pkg_collect_transitive_cmake_deps(
                        root, &dm,
                        /*include_workspace_siblings=*/false,
                        &closure, &n_closure)
                    && n_closure > 0) {
                    char cmake_lists[4096];
                    snprintf(cmake_lists, sizeof(cmake_lists),
                             "%s/cmake/CMakeLists.txt", root);
                    struct stat _cmst;
                    bool already_built = (stat(cmake_lists, &_cmst) == 0);
                    if (!already_built) {
                        char lock_path[4096];
                        snprintf(lock_path, sizeof(lock_path),
                                 "%s/tur.lock", root);
                        PkgLockFile lock;
                        memset(&lock, 0, sizeof(lock));
                        lock.format_version = 1;
                        pkg_lock_read(lock_path, &lock);
                        PkgManifest mu = dm;
                        mu.cmake_deps   = closure;
                        mu.n_cmake_deps = n_closure;
                        if (pkg_gen_cmake_deps(root, &mu)
                            && pkg_cmake_build(root, &mu, &lock, NULL)) {
                            pkg_lock_write(lock_path, &lock);
                        } else {
                            fprintf(stderr,
                                "tur build: cmake dependency build failed\n");
                            pkg_cmake_deps_free(closure, n_closure);
                            pkg_lock_free(&lock);
                            pkg_manifest_free(&dm);
                            free_tur_files(tur_files, n_files);
                            return 1;
                        }
                        pkg_lock_free(&lock);
                    }
                }
                pkg_cmake_deps_free(closure, n_closure);
                pkg_manifest_free(&dm);
            }
        }
    }

    /* Auto-detect library mode: if no source file defines a `main` entry
     * point, build as a shared library (.so) rather than an executable.
     * This lets `tur build <spice-root>` work without an explicit --shared
     * flag -- spices are pure libraries and have no main. */
    if (!shared) {
        bool has_main = false;
        for (int i = 0; !has_main && i < n_files; i++)
            has_main = file_has_main_defn(tur_files[i]);
        if (!has_main) shared = true;
    }

    /* src_root used to derive qualified module names ("app/util") from file
     * paths; matches the prefix collect_project_src_files builds paths from. */
    char src_root[4096];
    snprintf(src_root, sizeof(src_root), "%s/src", root);

    /* Validate that every module declared in the manifest's :exports has a
     * backing source file. The build set itself stays the full src/ scan (so
     * internal, non-exported modules still compile); this check only catches
     * manifest/source drift -- a declared export with no file -- and fails
     * loudly rather than silently shipping an incomplete library. */
    {
        char mpath[4096];
        (void)pkg_resolve_manifest_path(root, mpath, sizeof(mpath));
        PkgManifest em;
        if (pkg_manifest_read(mpath, &em)) {
            int missing = 0;
            for (int i = 0; i < em.n_exports; i++) {
                const char *e = em.exports[i];
                size_t el = strlen(e);
                char cand[4096];
                if (el >= 4 && strcmp(e + el - 4, ".tur") == 0)
                    snprintf(cand, sizeof(cand), "%s/%s", root, e);
                else
                    snprintf(cand, sizeof(cand), "%s/%s.tur", src_root, e);
                struct stat es;
                if (stat(cand, &es) != 0 || !S_ISREG(es.st_mode)) {
                    fprintf(stderr,
                            "tur: build.tur declares export '%s' but no source "
                            "file exists at '%s'\n", e, cand);
                    missing++;
                }
            }
            pkg_manifest_free(&em);
            if (missing) {
                free_tur_files(tur_files, n_files);
                return 1;
            }
        }
    }

    const char **proj_inc = NULL;
    int          n_proj_inc = 0;
    resolve_project_include_dirs(root, &proj_inc, &n_proj_inc);

    /* Merge user -I dirs (priority, first) with project-inferred dirs. */
    int n_inc = n_user_inc + n_proj_inc;
    const char **inc = NULL;
    if (n_inc > 0) {
        inc = (const char **)malloc((size_t)n_inc * sizeof(char *));
        if (!inc) {
            fprintf(stderr, "tur: oom\n");
            for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
            free(proj_inc);
            free_tur_files(tur_files, n_files);
            return 2;
        }
        for (int i = 0; i < n_user_inc; i++) inc[i] = user_inc[i];
        for (int i = 0; i < n_proj_inc; i++) inc[n_user_inc + i] = proj_inc[i];
    }

    /* Whole-program executable route (project-mode-rc-runtime-preamble-missing).
     * Separate compilation (cmd_build_multi_files, below) emits one TU per
     * module and omits the inline C runtime, so rc<T>/frame/effects fail to
     * link.  For an executable there is exactly one reachable entry, so we can
     * instead build that entry module *single-file*: compile_to_c -> emit_program
     * inlines every transitively-imported module into one TU carrying the full
     * runtime (the same path `tur build <file>` and the whole test suite
     * exercise).  Shared-library (.so) builds keep separate compilation -- they
     * have no single entry and link their deps separately. */
    if (!shared) {
        const char *entry = NULL;
        int n_main = 0;
        for (int i = 0; i < n_files; i++) {
            if (file_has_main_defn(tur_files[i])) { entry = tur_files[i]; n_main++; }
        }
        /* #[used] disqualifies the shortcut: a non-entry module retained for C
         * linkage (raw-extern bridge / by-address callback) is, by construction,
         * NOT in the entry's Turmeric import closure, so whole-program inlining
         * would drop it and its mangled symbol would dangle.  Fall through to
         * separate compilation, which compiles and links every project module. */
        bool nonentry_used = false;
        if (n_main == 1) {
            for (int i = 0; i < n_files; i++) {
                if (tur_files[i] != entry && file_has_used_attr(tur_files[i])) {
                    nonentry_used = true;
                    break;
                }
            }
        }
        /* Reroute only the unambiguous single-main case; a project with more
         * than one `main` falls through to the existing separate-compilation
         * path unchanged (no behavioral regression). */
        if (n_main == 1 && !nonentry_used) {
            int    n_rm = 0;
            char **rm   = discover_manifest_reader_macros(entry, &n_rm);
            /* build-output-directory-plan: when no -o was given, anchor the
             * executable inside the project's build dir so the single-main
             * fast path matches the separate-compilation default. */
            char *bd = NULL;
            char synth_out[PATH_MAX + 32];
            const char *eff_out = out_path;
            if (!out_path) {
                bd = resolve_build_dir(root, cli_build_dir);
                if (bd) {
                    char base[1024];
                    char mp[4096];
                    (void)pkg_resolve_manifest_path(root, mp, sizeof(mp));
                    PkgManifest mm; memset(&mm, 0, sizeof(mm));
                    if (pkg_manifest_read(mp, &mm) && mm.name && mm.name[0]) {
                        snprintf(base, sizeof(base), "%s", mm.name);
                    } else {
                        default_output_name(root, base, sizeof(base));
                    }
                    pkg_manifest_free(&mm);
                    snprintf(synth_out, sizeof(synth_out),
                             "%s/bin/%s", bd, base);
                    eff_out = synth_out;
                }
            }
            int rc = cmd_build(entry, eff_out, inc, n_inc, NULL,
                               (const char **)rm, n_rm);
            free(bd);
            free_reader_macro_paths(rm, n_rm);
            free(inc);
            for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
            free(proj_inc);
            free_tur_files(tur_files, n_files);
            return rc;
        }
    }

    /* T3: pull each resolved :spices dep's modules into the build set so a
     * cross-spice `(import dep/mod)` links under separate compilation.  The
     * include path (proj_inc) already lets the importer type-check against the
     * dep's source, but without also compiling+linking the dep's modules the
     * generated `#include "dep__mod.h"` has no backing file and cc fails.
     *
     * Dep modules are named relative to their own dep `src/` (via the per-file
     * src-root array) so e.g. `sib/api` maps to `sib__api.h`, matching the
     * importer's emitted include.  A dep module whose qualified name collides
     * with one already in the set (a project module, or an earlier dep) is
     * skipped, keeping the project's own copy.
     *
     * Skipped in shared-library mode: a `.so` links its deps separately and
     * must not absorb their modules or accumulate their exports into the
     * library's manifest.  Transitive deps-of-deps are out of scope (the
     * resolved include path only covers this project's direct `:spices`). */
    char       **all_files = tur_files;  /* realloc'd as dep modules append */
    int          n_all     = n_files;
    int          cap_all   = n_files;
    const char **all_roots = (const char **)malloc(
        (size_t)(cap_all > 0 ? cap_all : 1) * sizeof(char *));
    const char **dep_dirs  = NULL;
    int          n_dep_dirs = 0;
    if (!all_roots) {
        fprintf(stderr, "tur: oom\n");
        free(inc);
        for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
        free(proj_inc);
        free_tur_files(tur_files, n_files);
        return 2;
    }
    for (int i = 0; i < n_files; i++) all_roots[i] = src_root;

    /* Always collect dep modules: in executable mode they are linked in;
     * in shared-library mode they are compiled to headers only (so the
     * generated #include "dep__mod.h" in the project's C files resolves)
     * but their .c files are NOT linked into the output -- n_files tracks
     * the boundary.  Transitive deps-of-deps remain out of scope. */
    {
        char mpath[4096];
        (void)pkg_resolve_manifest_path(root, mpath, sizeof(mpath));
        PkgManifest dm;
        if (pkg_manifest_read(mpath, &dm)) {
            resolve_include_dirs_from_manifest(root, &dm, /*include_own_src=*/false,
                                               &dep_dirs, &n_dep_dirs);
            pkg_manifest_free(&dm);
        }

        /* seen-set of qualified module names already in the build set, seeded
         * with the project's own modules so dep modules can't shadow them. */
        int    n_seen = 0, cap_seen = n_files > 0 ? n_files : 1;
        char **seen = (char **)malloc((size_t)cap_seen * sizeof(char *));
        if (seen) {
            for (int i = 0; i < n_files; i++)
                seen[n_seen++] = derive_module_name(tur_files[i], src_root);
        }

        for (int k = 0; seen && k < n_dep_dirs; k++) {
            char **df = NULL; int dn = 0, dc = 0;
            collect_tur_recursive(dep_dirs[k], &df, &dn, &dc);
            qsort(df, (size_t)dn, sizeof(char *), compare_cstr_ptrs);
            for (int j = 0; j < dn; j++) {
                char *mn = derive_module_name(df[j], dep_dirs[k]);
                bool dup = false;
                for (int s = 0; s < n_seen; s++)
                    if (strcmp(seen[s], mn) == 0) { dup = true; break; }
                if (dup) { free(mn); free(df[j]); continue; }
                if (n_seen >= cap_seen) {
                    cap_seen *= 2;
                    char **ns = (char **)realloc(seen, (size_t)cap_seen * sizeof(char *));
                    if (!ns) { free(mn); free(df[j]); continue; }
                    seen = ns;
                }
                seen[n_seen++] = mn;
                if (n_all >= cap_all) {
                    cap_all = cap_all > 0 ? cap_all * 2 : 8;
                    all_files = (char **)realloc(all_files,
                                                 (size_t)cap_all * sizeof(char *));
                    all_roots = (const char **)realloc(all_roots,
                                                 (size_t)cap_all * sizeof(char *));
                }
                all_files[n_all]  = df[j];        /* transfer ownership */
                all_roots[n_all]  = dep_dirs[k];  /* borrowed until build returns */
                n_all++;
            }
            free(df);  /* shell only; strings transferred or freed above */
        }
        for (int s = 0; s < n_seen; s++) free(seen[s]);
        free(seen);
    }

    /* n_own = project's own modules (link these); n_all - n_own = dep modules
     * compiled only for header generation (shared mode skips their link). */
    int n_own = shared ? n_files : n_all;
    char *build_dir = resolve_build_dir(root, cli_build_dir);
    int rc;
    if (!build_dir) {
        rc = 2;
    } else {
        rc = cmd_build_multi_files(all_files, n_all, n_own, root, src_root,
                                   all_roots, out_path, shared, manifest_path,
                                   inc, n_inc, build_dir);
        free(build_dir);
    }

    free(all_roots);
    for (int k = 0; k < n_dep_dirs; k++) free((char *)dep_dirs[k]);
    free(dep_dirs);
    free(inc);
    for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
    free(proj_inc);
    return rc;
}

/* tur-link-and-build-split-plan Phase 3: derive the `.link` sidecar path for an
 * object -- strip a trailing ".o" and append ".link"; otherwise append ".link"
 * to the whole name.  Also used to derive the intermediate ".c" path (with a
 * ".c" suffix) for `tur compile`. */
static void obj_sibling_path(const char *obj, const char *suffix,
                             char *out, size_t cap) {
    size_t n = strlen(obj);
    if (n >= 2 && obj[n - 2] == '.' && obj[n - 1] == 'o') n -= 2;
    snprintf(out, cap, "%.*s%s", (int)n, obj, suffix);
}

/* Write one sidecar field line.  Handles both NUL-terminated buffers (autolink)
 * and non-terminated ones (cmake/aux) by writing exact content bytes minus a
 * single trailing NUL. */
static void sidecar_write_field(FILE *f, const char *key, const Buf *b) {
    fputs(key, f);
    if (b && b->len > 0) {
        size_t n = b->len;
        if (b->data[n - 1] == '\0') n--;
        if (n > 0) fwrite(b->data, 1, n, f);
    }
    fputc('\n', f);
}

/* tur-link-and-build-split-plan Phase 3a-A: write the `.link` sidecar next to a
 * compiled object.  It records the fully resolved link flags so `tur link` can
 * reproduce the link without re-running the frontend -- the single source of
 * truth for link-flag drift (plan section 7). */
static int write_link_sidecar(const char *path, const Buf *autolink,
                              bool needs_asan, const Buf *cmake_flags,
                              const Buf *aux_sources) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs("# tur link sidecar v1\n", f);
    fprintf(f, "asan: %d\n", needs_asan ? 1 : 0);
    sidecar_write_field(f, "autolink:", autolink);
    sidecar_write_field(f, "cmake:", cmake_flags);
    sidecar_write_field(f, "auxsrc:", aux_sources);
    fclose(f);
    return 0;
}

/* Append `val` to `dst` (NUL-terminating) only if that exact value has not been
 * appended before, tracked via the caller's seen-list.  Keeps `tur link` from
 * duplicating identical sidecar content when several objects of one program
 * each carry the same resolved flags (which would re-link a runtime .c twice
 * and fail with multiple-definition). */
static void sidecar_accum_unique(Buf *dst, char ***seen, int *n_seen,
                                 const char *val) {
    if (!val || !*val) return;
    for (int i = 0; i < *n_seen; i++)
        if (strcmp((*seen)[i], val) == 0) return;
    *seen = (char **)realloc(*seen, sizeof(char *) * (size_t)(*n_seen + 1));
    (*seen)[(*n_seen)++] = tur_strdup(val);
    buf_puts(dst, val);
}

/* Read one object's `.link` sidecar, accumulating its fields into the caller's
 * link-flag buffers.  ASan is OR-accumulated; the string fields are unioned via
 * sidecar_accum_unique.  Missing sidecar is not an error (a bare .o may have
 * none). */
static void read_link_sidecar(const char *path, Buf *autolink, bool *needs_asan,
                              Buf *cmake_flags, Buf *aux_sources,
                              char ***seen_al, int *n_seen_al,
                              char ***seen_cm, int *n_seen_cm,
                              char ***seen_ax, int *n_seen_ax) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = '\0';
        if (strncmp(line, "asan:", 5) == 0) {
            if (atoi(line + 5) != 0) *needs_asan = true;
        } else if (strncmp(line, "autolink:", 9) == 0) {
            sidecar_accum_unique(autolink, seen_al, n_seen_al, line + 9);
        } else if (strncmp(line, "cmake:", 6) == 0) {
            sidecar_accum_unique(cmake_flags, seen_cm, n_seen_cm, line + 6);
        } else if (strncmp(line, "auxsrc:", 7) == 0) {
            sidecar_accum_unique(aux_sources, seen_ax, n_seen_ax, line + 7);
        }
    }
    fclose(f);
}

/* tur-link-and-build-split-plan Phase 3a-0: `tur compile <in.tur> -o <out.o>`.
 * Lowers one Turmeric source to an object file (the emit-c lowering fused with
 * `cc -c`) plus a `.link` sidecar carrying the resolved link flags.  This is
 * the cacheable half of the compile/link pipeline -- ccache hits on unchanged
 * input.  `emit-c` is untouched; this is the additive fused step. */
static int cmd_compile(const char *input, const char *out_obj,
                       const char **include_dirs, int n_include_dirs,
                       const char **reader_macro_paths,
                       int n_reader_macro_paths) {
    /* Frontend: lower to C, force-loading any #[used] sibling modules just as
     * cmd_build does (single-file whole-program TU). */
    Buf csrc;
    buf_init(&csrc);
    int n_used_mods = 0;
    char **used_mods = collect_used_attr_modules(input, include_dirs,
                                                 n_include_dirs, &n_used_mods);
    UsedModulesCtx used_ctx = { (const char **)used_mods, n_used_mods };
    if (n_used_mods > 0) used_modules_ctx_set(&used_ctx);
    int rc = compile_to_c(input, &csrc, include_dirs, n_include_dirs,
                          reader_macro_paths, n_reader_macro_paths);
    if (n_used_mods > 0) used_modules_ctx_set(NULL);
    free_tur_files(used_mods, n_used_mods);
    if (rc != 0) { buf_free(&csrc); return rc; }
    hoist_tur_include_directives(&csrc);

    /* Resolve the object path (default: <name>.o beside cwd). */
    char obj_buf[1024];
    if (!out_obj) {
        char base[512];
        default_output_name(input, base, sizeof(base));
        snprintf(obj_buf, sizeof(obj_buf), "%s.o", base);
        out_obj = obj_buf;
    }

    /* Write the generated C next to the object (deterministic -> ccache-able). */
    char cpath[1100];
    obj_sibling_path(out_obj, ".c", cpath, sizeof(cpath));
    FILE *tf = fopen(cpath, "wb");
    if (!tf || fwrite(csrc.data, 1, csrc.len, tf) != csrc.len) {
        fprintf(stderr, "tur compile: cannot write generated C to '%s'\n", cpath);
        if (tf) fclose(tf);
        buf_free(&csrc);
        return 2;
    }
    fclose(tf);

    Buf autolink;
    buf_init(&autolink);
    scan_autolink_markers(&csrc, &autolink);
    buf_free(&csrc);

    /* Phase 2/3c: --runtime=lib swaps bare runtime sources for libturi.a so the
     * sidecar records the -lturi link and the object never carries the runtime
     * TUs.  cmd_compile is native-only, so no wasm guard is needed. */
    apply_runtime_lib_mode(&autolink);

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    const char *cc_flags = getenv("TUR_CC_FLAGS");
    if (!cc_flags || !*cc_flags) {
        cc_flags = g_emit_debug_lines
            ? "-g -Og -std=c99 -Wall -fno-strict-aliasing"
            : "-O2 -std=c99 -Wall -fno-strict-aliasing";
    }

    Buf cmake_flags;  buf_init(&cmake_flags);
    Buf aux_includes; buf_init(&aux_includes);
    Buf aux_sources;  buf_init(&aux_sources);
    collect_build_aux(input, &cmake_flags, &aux_includes, &aux_sources);

    bool needs_asan = false;
    resolve_autolink_flags(&autolink, cc_flags, &needs_asan);

    /* The cacheable `cc -c`.  Include dirs come from the spice aux includes,
     * cmake -I, the autolink -I tokens (e.g. -Isrc/runtime), and any explicit
     * -I.  -l/-L and bare .c sources are link-time only and stay out of here. */
    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s", cc, cc_flags);
    if (aux_includes.len > 0) buf_puts(&cmd, aux_includes.data);
    if (cmake_flags.len > 0)  buf_puts(&cmd, cmake_flags.data);
    append_include_tokens(&cmd, autolink.len > 0 ? autolink.data : NULL);
    for (int i = 0; i < n_include_dirs; i++) {
        if (include_dirs[i] && include_dirs[i][0])
            buf_printf(&cmd, " -I%s", include_dirs[i]);
    }
    buf_printf(&cmd, " -c %s -o %s", cpath, out_obj);
    buf_putc(&cmd, '\0');
    int sys_rc = system(cmd.data);
    buf_free(&cmd);
    buf_free(&aux_includes);

    if (sys_rc != 0) {
        fprintf(stderr, "tur compile: cc -c invocation failed (status %d)\n", sys_rc);
        buf_free(&autolink);
        buf_free(&cmake_flags);
        buf_free(&aux_sources);
        return 2;
    }

    /* Emit the .link sidecar carrying the resolved link flags. */
    char side[1100];
    obj_sibling_path(out_obj, ".link", side, sizeof(side));
    if (write_link_sidecar(side, &autolink, needs_asan, &cmake_flags,
                           &aux_sources) != 0) {
        fprintf(stderr, "tur compile: cannot write link sidecar '%s'\n", side);
        buf_free(&autolink);
        buf_free(&cmake_flags);
        buf_free(&aux_sources);
        return 2;
    }

    buf_free(&autolink);
    buf_free(&cmake_flags);
    buf_free(&aux_sources);
    return 0;
}

/* tur-link-and-build-split-plan Phase 3a: `tur link <obj-or-source>... -o <out>`.
 * Links precompiled objects (+ their `.link` sidecars) and/or .c sources into an
 * executable (or a shared library with --shared).  Reuses link_command_run --
 * the same link path `tur build` drives -- so the two cannot drift. */
static int cmd_link(const char *out, const char **inputs, int n_inputs,
                    bool shared, const char *extra_link_flags) {
    if (n_inputs == 0) {
        fprintf(stderr, "tur link: no input objects/sources\n");
        return 1;
    }

    /* Join the input paths and gather link flags from each object's sidecar. */
    Buf inputs_joined; buf_init(&inputs_joined);
    Buf autolink;      buf_init(&autolink);
    Buf cmake_flags;   buf_init(&cmake_flags);
    Buf aux_sources;   buf_init(&aux_sources);
    bool needs_asan = false;
    char **seen_al = NULL; int n_seen_al = 0;
    char **seen_cm = NULL; int n_seen_cm = 0;
    char **seen_ax = NULL; int n_seen_ax = 0;

    for (int i = 0; i < n_inputs; i++) {
        if (inputs_joined.len > 0) buf_putc(&inputs_joined, ' ');
        buf_puts(&inputs_joined, inputs[i]);
        /* A .o (or bare object) may carry a sidecar; look it up. */
        char side[1100];
        obj_sibling_path(inputs[i], ".link", side, sizeof(side));
        read_link_sidecar(side, &autolink, &needs_asan, &cmake_flags,
                          &aux_sources, &seen_al, &n_seen_al,
                          &seen_cm, &n_seen_cm, &seen_ax, &n_seen_ax);
    }
    buf_putc(&inputs_joined, '\0');
    for (int i = 0; i < n_seen_al; i++) free(seen_al[i]);
    for (int i = 0; i < n_seen_cm; i++) free(seen_cm[i]);
    for (int i = 0; i < n_seen_ax; i++) free(seen_ax[i]);
    free(seen_al); free(seen_cm); free(seen_ax);

    /* User-supplied extra link flags fold into the autolink buffer (which
     * link_command_run appends verbatim after the inputs). */
    if (extra_link_flags && *extra_link_flags) {
        if (autolink.len > 0) buf_putc(&autolink, ' ');
        buf_puts(&autolink, extra_link_flags);
    }
    /* NUL-terminate the accumulated flag buffers so link_command_run's `%s` /
     * buf_puts reads stop cleanly. */
    if (autolink.len > 0)    buf_putc(&autolink, '\0');
    if (cmake_flags.len > 0) buf_putc(&cmake_flags, '\0');
    if (aux_sources.len > 0) buf_putc(&aux_sources, '\0');

    /* --shared rides on cc_flags (position-independent shared object). */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    const char *base_flags = getenv("TUR_CC_FLAGS");
    if (!base_flags || !*base_flags) base_flags = "-O2 -fno-strict-aliasing";
    Buf cc_flags; buf_init(&cc_flags);
    buf_puts(&cc_flags, base_flags);
    if (shared) buf_puts(&cc_flags, " -shared -fPIC");
    buf_putc(&cc_flags, '\0');

    char out_buf[1024];
    if (!out) {
        default_output_name(inputs[0], out_buf, sizeof(out_buf));
        out = out_buf;
    }

    int link_rc = link_command_run(cc, cc_flags.data, inputs_joined.data,
                                   NULL, &aux_sources, &autolink, needs_asan,
                                   &cmake_flags, NULL, 0, out);
    buf_free(&inputs_joined);
    buf_free(&autolink);
    buf_free(&cmake_flags);
    buf_free(&aux_sources);
    buf_free(&cc_flags);
    return link_rc;
}

static int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* tur format [--check|--diff] [file]
 * Read source from file (or stdin if no file given), format it, and write to
 * stdout.  --check: exit 1 if file is not already formatted (no output).
 * --diff: print unified diff if file would change; exit 1 if changed. */
static int cmd_format(const char *path, bool check_only, bool diff_mode) {
    char  *src = NULL;
    size_t len = 0;

    if (path) {
        if (read_entire_file(path, &src, &len) != 0) return 2;
    } else {
        /* Read from stdin */
        size_t cap = 4096;
        src = (char *)malloc(cap);
        if (!src) { fprintf(stderr, "tur: oom\n"); return 2; }
        len = 0;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = (char *)realloc(src, cap);
                if (!tmp) { free(src); fprintf(stderr, "tur: oom\n"); return 2; }
                src = tmp;
            }
            src[len++] = (char)c;
        }
        src[len] = '\0';
    }

    SourceFile file = {0};
    file.path        = path ? path : "<stdin>";
    file.src         = src;
    file.len         = len;
    file.file_id     = 0;
    file.reader_type = READER_TURMERIC;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    /* RM4 follow-up: preload manifest reader-macros so `tur format`
     * doesn't fail to parse files that use `#foo{...}` invocations
     * registered in the spice's build.tur. Stdin-mode (path == NULL)
     * has no enclosing file to walk up from -- skip. */
    ReaderMacroRegistry reader_macros_reg;
    reader_macros_init(&reader_macros_reg, &arena);
    /* Transitive-RM decision #2: batch compile is strict -- duplicate
     * `(reader-macros/define ...)` is a hard error. REPL leaves this
     * flag false on env->reader_macros for iterative-redefinition UX. */
    reader_macros_reg.strict = true;
    if (path) {
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(path, &rm_n);
        for (int i = 0; i < rm_n; ++i) {
            if (reader_macros_load_file(&arena, &st, rm_p[i],
                                        &reader_macros_reg) != 0) {
                free_reader_macro_paths(rm_p, rm_n);
                symtab_free(&st); arena_free(&arena); free(src);
                return 1;
            }
        }
        free_reader_macro_paths(rm_p, rm_n);
    }

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file,
                                          &reader_macros_reg, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 1;
    } else {
        FmtOptions opts = {0};
        opts.indent_width = 2;
        opts.line_width   = 80;
        opts.src          = src;
        opts.src_len      = len;

        Buf out;
        buf_init(&out);
        if (fmt_print(&out, forms, nforms, opts) != 0) {
            fprintf(stderr, "tur: fmt_print failed\n");
            rc = 1;
        } else if (check_only) {
            /* Exit 1 if already-formatted output differs from input */
            bool same = (out.len == len) && (memcmp(out.data, src, len) == 0);
            if (!same) {
                if (path) fprintf(stderr, "tur: %s is not formatted\n", path);
                rc = 1;
            }
        } else if (diff_mode) {
            bool same = (out.len == len) && (memcmp(out.data, src, len) == 0);
            if (!same) {
                /* Write original and formatted to temp files, run diff -u. */
                char orig_tmp[512];
                snprintf(orig_tmp, sizeof(orig_tmp), "%s/tur-fmt-orig-XXXXXX", tur_temp_dir());
                int orig_fd = mkstemp(orig_tmp);
                if (orig_fd >= 0) {
                    ssize_t _wr1 = write(orig_fd, src, len); (void)_wr1;
                    close(orig_fd);
                }
                char new_tmp[512];
                snprintf(new_tmp, sizeof(new_tmp), "%s/tur-fmt-new-XXXXXX", tur_temp_dir());
                int new_fd = mkstemp(new_tmp);
                if (new_fd >= 0) {
                    ssize_t _wr2 = write(new_fd, out.data, out.len); (void)_wr2;
                    close(new_fd);
                }
                const char *label = path ? path : "<stdin>";
                char diff_cmd[8192];
                /* -L flag supported by both GNU diff and BSD diff (macOS) */
                snprintf(diff_cmd, sizeof(diff_cmd),
                         "diff -u -L '%s' -L '%s' '%s' '%s'",
                         label, label, orig_tmp, new_tmp);
                int diff_rc = system(diff_cmd);
                unlink(orig_tmp);
                unlink(new_tmp);
                /* diff exits 1 when files differ, 0 when same */
                if (diff_rc != 0) rc = 1;
            }
        } else {
            buf_to_file(&out, stdout);
        }
        buf_free(&out);
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* tur parse-check <a> <b>
 * Read two files and compare the form trees they produce. The first file is
 * read as turmeric s-expressions, the second as sweet-exp, unless an explicit
 * `#lang` directive on either file overrides the forced reader. Used by
 * tools/check-guide-pairs.py to verify a guide's turmeric+sweet-exp toggle
 * pair reads to the same AST.
 *
 * Exit codes: 0 = ASTs equal, 1 = mismatch, 2 = read/parse error. */

/* parse-check canonicalization: a simple type annotation reads as an
 * `F_TYPE_ANN` wrapping the type when written spaced (`: int`) but as a plain
 * `F_KEYWORD` when written unspaced (`:int`). Both elaborate identically, so
 * for AST-equality purposes fold `F_TYPE_ANN(sym)` down to `F_KEYWORD(sym)`.
 * Compound annotations (`: (fn [:int] :int)`, `: list<int>`) keep their
 * `F_TYPE_ANN` wrapper -- both toggle siblings produce it, so they still match.
 * Applied to both sides before form_print, so a guide's traditional `[] : int`
 * and its sweet-exp `[] :int` compare equal. */
static void parse_check_canon(SymbolTable *st, Form *f) {
    if (!f) return;
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_MAP: case F_SET:
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE:
        case F_UNQUOTE_SPLICING: case F_TYPE_ANN: case F_CONTRACT_TYPE:
        case F_READER_COND: case F_RANGE_VAR:
        case F_MAP_LITERAL: case F_SET_LITERAL: case F_ROW_LITERAL:
            for (uint32_t i = 0; i < f->as.list.len; i++)
                parse_check_canon(st, f->as.list.items[i]);
            break;
        default:
            break;
    }
    /* Fold `: T` (F_TYPE_ANN wrapping a single atom) to the `:T` keyword form.
     * The inner is F_SYM for ordinary type names, but a literal for the built-in
     * type names nil/true/false, which the reader produces as F_NIL/F_BOOL. */
    if (f->tag == F_TYPE_ANN && f->as.list.len == 1) {
        const Form  *inner = f->as.list.items[0];
        const Symbol *kw   = NULL;
        switch (inner->tag) {
            case F_SYM:  kw = inner->as.sym; break;
            case F_NIL:  kw = symtab_intern(st, strslice("nil", 3)); break;
            case F_BOOL: kw = inner->as.b ? symtab_intern(st, strslice("true", 4))
                                          : symtab_intern(st, strslice("false", 5));
                         break;
            default:     break;
        }
        if (kw) {
            f->tag    = F_KEYWORD;
            f->as.sym = kw;
        }
    }
}

/* Read `path`, select the reader (forced unless a `#lang` directive is
 * present), parse all top-level forms, and serialize each with form_print into
 * `out` (one per line). Returns 0 on success, non-zero on read/parse error. */
static int parse_check_read(const char *path, ReaderType forced, Buf *out) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return 2;

    /* Honour an explicit `#lang` directive; otherwise force by position.
     * detect_lang leaves out_rest == src when no directive is present, and
     * otherwise advances past the directive so the reader does not choke on
     * the leading '#'. */
    const char *rest = src;
    size_t      rest_len = len;
    ReaderType  detected = detect_lang(src, len, &rest, &rest_len);
    ReaderType  reader   = forced;
    if (rest != src) {
        /* A `#lang` directive was present -- honour it and use the
         * directive-stripped source. */
        if (!reader_type_is_implemented(detected)) {
            fprintf(stderr, "tur: %s: unknown or unimplemented #lang directive\n",
                    path);
            free(src);
            return 2;
        }
        reader = detected;
    } else {
        /* No directive: read the whole source with the forced reader. */
        rest     = src;
        rest_len = len;
    }

    /* Fresh diagnostic slate per file -- see compile_to_c.  cmd_check_dir
     * loops parse_check_read in-process, so a stale `had_error_` from an
     * earlier file must not leak into this one's `diag_had_error()` gate. */
    diag_reset();

    SourceFile file = {0};
    file.path        = path;
    file.src         = rest;
    file.len         = rest_len;
    file.file_id     = 0;
    file.reader_type = reader;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file, NULL, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = 2;
    } else {
        for (uint32_t i = 0; i < nforms; i++) {
            parse_check_canon(&st, forms[i]);
            form_print(out, forms[i]);
            buf_putc(out, '\n');
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

static int cmd_parse_check(const char *path_a, const char *path_b) {
    Buf sa, sb;
    buf_init(&sa);
    buf_init(&sb);

    int rc;
    if (parse_check_read(path_a, READER_TURMERIC, &sa) != 0 ||
        parse_check_read(path_b, READER_SWEET, &sb) != 0) {
        rc = 2;
    } else if (sa.len == sb.len &&
               (sa.len == 0 || memcmp(sa.data, sb.data, sa.len) == 0)) {
        rc = 0;
    } else {
        fprintf(stderr, "tur: parse-check mismatch between %s and %s\n",
                path_a, path_b);
        fprintf(stderr, "--- %s ---\n%.*s", path_a, (int)sa.len,
                sa.data ? sa.data : "");
        fprintf(stderr, "--- %s ---\n%.*s", path_b, (int)sb.len,
                sb.data ? sb.data : "");
        rc = 1;
    }

    buf_free(&sa);
    buf_free(&sb);
    return rc;
}

/* ---------------------------------------------------------------------------
 * tur fmt -- formatter with directory walking and in-place writes
 * ---------------------------------------------------------------------------
 *
 * Modes:
 *   default (no --check/--diff/--stdout/--stdin):
 *     format files in-place; print names of changed files to stderr
 *   --check / --dry-run:
 *     exit 1 if any file would change; print names to stderr
 *   --diff:
 *     print unified diff for each file that would change; exit 1 if any
 *   --stdout <file>:
 *     format one file and write to stdout; do not modify the file
 *   --stdin [--lang <lang>]:
 *     read from stdin, format, write to stdout
 *
 * Exit codes: 0=ok, 1=would-change (--check) or I/O error, 2=CLI/parse error
 */

/* Directories to skip when walking */
static bool fmt_skip_dir(const char *name) {
    return strcmp(name, "build")          == 0
        || strcmp(name, ".git")           == 0
        || strcmp(name, ".tur-cache")     == 0
        || strcmp(name, ".turnb-cache")   == 0
        || strcmp(name, ".tur-repl-cache")== 0;
}

static bool fmt_is_tur_file(const char *name) {
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".tur") == 0) return true;
    if (n >= 10 && strcmp(name + n - 10, ".tur.sweet") == 0) return true;
    return false;
}


/* Core: read src, parse, format, return formatted Buf.
 * Returns 0 on success with *out populated, -1 on error.
 *
 * The pipeline itself lives in fmt.c so the LSP's textDocument/formatting
 * handler -- which is linked into tur_core, not into main.c -- can reach the
 * same code instead of shelling out to this binary. */
static int fmt_format_source(const char *path_label, const char *src, size_t len,
                              ReaderType rtype, Buf *out) {
    return fmt_format_buffer(path_label, src, len, rtype, out);
}

typedef enum {
    FMT_MODE_INPLACE,
    FMT_MODE_CHECK,
    FMT_MODE_DIFF,
    FMT_MODE_STDOUT,
} FmtMode;

/* Process one file. Returns 0=unchanged, 1=changed/would-change, -1=error. */
static int fmt_process_file(const char *path, ReaderType force_lang,
                             FmtMode mode) {
    char  *src = NULL;
    size_t len = 0;
    if (read_entire_file(path, &src, &len) != 0) return -1;

    ReaderType rtype = reader_type_from_extension(path);

    Buf out;
    int rc = fmt_format_source(path, src, len, rtype, &out);
    if (rc != 0) { free(src); return -1; }

    bool changed = (out.len != len) || (memcmp(out.data, src, len) != 0);

    if (!changed) {
        free(src);
        buf_free(&out);
        return 0;
    }

    /* File would change */
    switch (mode) {
        case FMT_MODE_CHECK:
            fprintf(stderr, "tur fmt: %s\n", path);
            free(src);
            buf_free(&out);
            return 1;

        case FMT_MODE_DIFF: {
            char orig_tmp[512];
            snprintf(orig_tmp, sizeof(orig_tmp), "%s/tur-fmt-orig-XXXXXX", tur_temp_dir());
            char new_tmp[512];
            snprintf(new_tmp, sizeof(new_tmp), "%s/tur-fmt-new-XXXXXX", tur_temp_dir());
            int ofd = mkstemp(orig_tmp);
            if (ofd >= 0) {
                ssize_t _w1 = write(ofd, src, len); (void)_w1;
                close(ofd);
            }
            free(src);
            int nfd = mkstemp(new_tmp);
            if (nfd >= 0) {
                ssize_t _w2 = write(nfd, out.data, out.len); (void)_w2;
                close(nfd);
            }
            char diff_cmd[8192];
            snprintf(diff_cmd, sizeof(diff_cmd),
                     "diff -u -L '%s' -L '%s' '%s' '%s'",
                     path, path, orig_tmp, new_tmp);
            int diff_rc = system(diff_cmd);
            unlink(orig_tmp);
            unlink(new_tmp);
            buf_free(&out);
            return (diff_rc != 0) ? 1 : 0;
        }

        case FMT_MODE_STDOUT:
            free(src);
            buf_to_file(&out, stdout);
            buf_free(&out);
            return 0;

        case FMT_MODE_INPLACE:
            free(src);
            if (buf_to_path(&out, path) != 0) {
                fprintf(stderr, "tur fmt: cannot write '%s': %s\n",
                        path, strerror(errno));
                buf_free(&out);
                return -1;
            }
            fprintf(stderr, "tur fmt: reformatted %s\n", path);
            buf_free(&out);
            return 1;
    }
    free(src);
    buf_free(&out);
    return 0;
}

/* Walk a directory, processing all .tur/.tur.sweet files.
 * Returns count of changed files, or -1 if an error occurred. */
static int fmt_walk(const char *path, ReaderType force_lang, FmtMode mode,
                    int *err_count) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "tur fmt: cannot stat '%s': %s\n", path, strerror(errno));
        (*err_count)++;
        return 0;
    }

    int changed = 0;

    if (S_ISREG(st.st_mode)) {
        if (!fmt_is_tur_file(path)) return 0;
        int r = fmt_process_file(path, force_lang, mode);
        if (r < 0) (*err_count)++;
        else if (r > 0) changed++;
        return changed;
    }

    if (!S_ISDIR(st.st_mode)) return 0;

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "tur fmt: cannot open dir '%s': %s\n", path, strerror(errno));
        (*err_count)++;
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* skip hidden (incl. .git) */
        if (fmt_skip_dir(ent->d_name)) continue;

        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

        /* tur_dirent_is_* fold in the stat() fallback for filesystems (and
         * Windows) that do not report d_type, so the DT_UNKNOWN arm this used
         * to carry -- a verbatim copy of both branches below -- is gone. */
        if (tur_dirent_is_dir(path, ent)) {
            changed += fmt_walk(child, force_lang, mode, err_count);
        } else if (tur_dirent_is_reg(path, ent) && fmt_is_tur_file(ent->d_name)) {
            int r = fmt_process_file(child, force_lang, mode);
            if (r < 0) (*err_count)++;
            else if (r > 0) changed++;
        }
    }
    closedir(d);
    return changed;
}

static int usage_fmt(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur fmt [paths...]                   format .tur/.tur.sweet files in place\n"
        "  tur fmt --check [paths...]           exit 1 if any file would change\n"
        "  tur fmt --dry-run [paths...]         alias for --check\n"
        "  tur fmt --diff [paths...]            print unified diff of changes\n"
        "  tur fmt --stdout <file>              format file and print to stdout\n"
        "  tur fmt --stdin [--lang <dialect>]   format stdin and print to stdout\n"
        "\n"
        "  Paths may be files or directories.  Defaults to current directory.\n"
        "  Skips:  build/  .git/  .tur-cache/  .turnb-cache/  .tur-repl-cache/\n"
        "\n"
        "  Dialects for --lang:  turmeric (default)  sweet-exp  curly-infix  neoteric\n"
        "  (tursweet is a deprecated alias for sweet-exp)\n"
        "\n"
        "Exit codes:\n"
        "  0   all files already formatted (or successfully written)\n"
        "  1   --check: at least one file would change; or I/O error\n"
        "  2   CLI / parse error\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int cmd_fmt(int argc, char **argv) {
    bool check_only   = false;
    bool diff_mode    = false;
    bool stdin_mode   = false;
    const char *stdout_file = NULL;
    ReaderType  force_lang  = READER_TURMERIC;
    bool        lang_set    = false;

    /* Collect non-flag arguments as paths */
    char **paths  = NULL;
    int    npaths = 0, cap_paths = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return usage_fmt();

        if (strcmp(argv[i], "--check") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--diff") == 0) {
            diff_mode = true;
        } else if (strcmp(argv[i], "--stdin") == 0) {
            stdin_mode = true;
        } else if (strcmp(argv[i], "--stdout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tur fmt: --stdout requires a file argument\n");
                return 2;
            }
            stdout_file = argv[++i];
        } else if (strcmp(argv[i], "--lang") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tur fmt: --lang requires a dialect argument\n");
                return 2;
            }
            const char *lang = argv[++i];
            if (strcmp(lang, "turmeric") == 0 || strcmp(lang, "tur") == 0) {
                force_lang = READER_TURMERIC;
            } else if (strcmp(lang, "sweet-exp") == 0 || strcmp(lang, "sweet") == 0 || strcmp(lang, "tursweet") == 0) {
                force_lang = READER_SWEET;
            } else if (strcmp(lang, "curly-infix") == 0) {
                force_lang = READER_CURLY_INFIX;
            } else if (strcmp(lang, "neoteric") == 0) {
                force_lang = READER_NEOTERIC;
            } else {
                fprintf(stderr, "tur fmt: unknown dialect '%s'\n", lang);
                return 2;
            }
            lang_set = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "tur fmt: unknown flag '%s'\n", argv[i]);
            return 2;
        } else {
            if (npaths >= cap_paths) {
                cap_paths = cap_paths ? cap_paths * 2 : 8;
                paths = (char **)realloc(paths, (size_t)cap_paths * sizeof(char *));
                if (!paths) { fprintf(stderr, "tur: oom\n"); return 1; }
            }
            paths[npaths++] = argv[i];
        }
    }

    /* Validate flag combinations */
    if ((check_only && diff_mode) || (check_only && stdin_mode) ||
        (diff_mode  && stdin_mode)) {
        fprintf(stderr, "tur fmt: incompatible flags\n");
        free(paths);
        return 2;
    }
    if (stdout_file && (check_only || diff_mode || stdin_mode || npaths > 0)) {
        fprintf(stderr, "tur fmt: --stdout cannot be combined with other modes or paths\n");
        free(paths);
        return 2;
    }
    if (lang_set && !stdin_mode && !stdout_file) {
        fprintf(stderr, "tur fmt: --lang is only meaningful with --stdin or --stdout\n");
        free(paths);
        return 2;
    }

    /* --stdin mode: read from stdin, format, write to stdout */
    if (stdin_mode) {
        size_t cap = 4096;
        char *src = (char *)malloc(cap);
        if (!src) { fprintf(stderr, "tur: oom\n"); free(paths); return 1; }
        size_t len = 0;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = (char *)realloc(src, cap);
                if (!tmp) { free(src); fprintf(stderr, "tur: oom\n"); free(paths); return 1; }
                src = tmp;
            }
            src[len++] = (char)c;
        }
        src[len] = '\0';

        Buf out;
        int rc = fmt_format_source("<stdin>", src, len, force_lang, &out);
        free(src);
        free(paths);
        if (rc != 0) return 1;
        buf_to_file(&out, stdout);
        buf_free(&out);
        return 0;
    }

    /* --stdout <file> mode: format one file, write to stdout */
    if (stdout_file) {
        char *src = NULL; size_t len = 0;
        if (read_entire_file(stdout_file, &src, &len) != 0) { free(paths); return 1; }
        ReaderType rtype = lang_set ? force_lang : reader_type_from_extension(stdout_file);
        Buf out;
        int rc = fmt_format_source(stdout_file, src, len, rtype, &out);
        free(src);
        free(paths);
        if (rc != 0) return 1;
        buf_to_file(&out, stdout);
        buf_free(&out);
        return 0;
    }

    /* Default to cwd when no paths given */
    if (npaths == 0) {
        paths = (char **)realloc(paths, sizeof(char *));
        paths[0] = ".";
        npaths = 1;
    }

    FmtMode mode = check_only ? FMT_MODE_CHECK
                 : diff_mode  ? FMT_MODE_DIFF
                 :              FMT_MODE_INPLACE;

    int changed = 0, errors = 0;
    for (int i = 0; i < npaths; i++)
        changed += fmt_walk(paths[i], force_lang, mode, &errors);

    free(paths);

    if (errors > 0) return 1;
    if (changed > 0 && check_only) return 1;
    if (changed > 0 && diff_mode)  return 1;
    return 0;
}

/* The wk_register_* / native_contract_* natives now live in
 * src/turi/interpreter_natives.c (tur_core); their declarations come in via
 * "turi/interpreter_natives.h" above. */

/* Debugger Phase 3: optional hook fired by cmd_eval (in debug mode) once the
 * interpreter env + debugger are constructed but before the program is armed
 * and run.  The DAP launch path uses it to flush staged breakpoints and install
 * the DAP pause / condition handlers.  NULL for the plain `tur debug` REPL. */
typedef struct {
    void (*on_ready)(TuriEnv *env, void *ud);
    void  *ud;
} EvalHooks;

/* Phase S0: tur repl — interactive read-eval-print loop. */
/* Phase INT-1: run a .tur file through the tree-walking interpreter.
 * extra_argv/extra_argc are the arguments after the file path, exposed
 * to the script as *args* (a cons-cell list of C-string pointers). */
static int cmd_eval_h(const char *path, bool use_color,
                      char **extra_argv, int extra_argc, bool debug,
                      const EvalHooks *hooks) {
    g_interpret_mode = true;
    turi_init(use_color);
    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "tur: failed to create interpreter environment\n");
        return 1;
    }
    /* File-eval (`tur --interpret <file>`) is a batch compile of a single
     * file, not an interactive session: a reader macro registered twice with
     * a differing template is the same "two modules both register #json"
     * footgun the compiled entry points reject (TURI_FIXTURES errors/
     * reader-macros-strict-collision).  Make this env's reader-macro registry
     * strict so the collision is a hard error, matching the compiled path.
     * The REPL (turi/repl.c) leaves the default strict=false so its src_acc
     * replay and iterative redefinition stay smooth.  Safe here because the
     * preloaded stdlib registers no reader macros and the user file is parsed
     * in a single pass (no self-replay of its own `reader-macros/define`). */
    env->reader_macros->strict = true;
    /* A debug session used to opt OUT of incremental elaboration here, because
     * the DAP server resolves each frame's source from diag_file_path(
     * span.file_id) and the incremental path lost that provenance -- a
     * post-stepOut frame came back as `?:19` with no `source` object, and the
     * conditional breakpoint never fired.  The cause was the file REGISTRY, not
     * the spans: diag_reset() clears it every turn, and the incremental path
     * reuses previously-parsed Forms rather than re-running their `(load ...)`
     * splices, so the files those Forms name were never re-registered.  Fixed
     * at the source (diag_files_save / diag_files_restore in turi_eval_impl),
     * so debug sessions keep the incremental path.
     * See docs/archive/incremental-elab-loses-span-file-provenance.md. */
    /* Pre-detect the user file's #lang so the prelude loads under the SAME
     * reader.  Otherwise the user file's `#lang sweet-exp` (etc.) flips
     * env->reader_type mid-stream, and turi_eval_impl discards the accumulated
     * prelude (src_acc) to avoid re-parsing it under the new reader -- dropping
     * every preloaded stdlib defn, so e.g. a `#map{...}` literal under
     * `#lang sweet-exp` fails "unknown function or operator 'hamt-of'".  By
     * setting the reader first, the prelude is parsed under the file's reader
     * from the start (plain s-expr parses under every reader variant) and the
     * user file's directive then matches env->reader_type -- no reset fires.
     * Scoped to the file-eval entry point; the REPL keeps its protective reset
     * for genuine interactive reader switches. */
    {
        FILE *pf = fopen(path, "rb");
        if (pf) {
            char head[512];
            size_t hn = fread(head, 1, sizeof(head) - 1, pf);
            fclose(pf);
            head[hn] = '\0';
            const char *rest = head; size_t rest_len = hn;
            LangLayerSet layers = 0;
            ReaderType rt = detect_lang_layered(head, hn, &rest, &rest_len,
                                                &layers, NULL, NULL);
            if (rest != head && reader_type_is_implemented(rt)) {
                env->reader_type = rt;
                /* Pre-seed the layer set too so the prelude and the user file
                 * read under the same layers (lang-layers-plan L1); turi_eval
                 * unions the authoritative set again when it strips the
                 * directive. */
                env->lang_layers = layers;
            }
        }
    }
    /* Preload macros.tur so that and/or/when/cond/for etc. are available.
     * This is the minimum stdlib needed for any real Turmeric program to work.
     *
     * TI8.b (turi-parity-post-v1-plan): load it via a `(load ...)` form rather
     * than turi_eval_file().  turi_eval_file concatenates the source into the
     * single <eval> blob (file_id 0); macros.tur carries `(defmodule tur/macros
     * ...)`, so any user fixture that *also* declares a defmodule then collided
     * with it under "only one defmodule is allowed per file" (both forms shared
     * file_id 0, defeating the per-file reset).  The `(load ...)` path assigns
     * macros.tur its own file_id, so the defmodule-per-file boundary reset
     * fires and a user defmodule no longer conflicts with the preloaded one.
     *
     * Runtime contracts (contract.tur) are loaded right after macros.tur (and,
     * like it, in its own turi_eval so it lands at the very front of the
     * accumulated source).  contract.tur exports macros (assert!/require!/
     * ensure!/invariant! + their -msg! variants); the Phase M7 promotion in
     * elaborate_program nulls a tur/-prefixed module's macros'
     * defining_module_name -- making them globally visible without an explicit
     * import -- only for modules within the stdlib-prefix boundary, which
     * (because load-expansion shifts form indices) effectively covers the
     * earliest-loaded modules.  Loading contract.tur up front, next to
     * macros.tur, keeps assert!/require!/... in that promoted region so user
     * code sees them bare under --interpret.  Its inline-C tur-contract-check /
     * tur-contract-check-inv bodies are overridden by native_contract_check[_inv]
     * below; without this preload the elaborator never sees a tur-contract-check
     * binding and silently drops every :pre/:post/:type check (a silent
     * miscompile).
     *
     * turi_env_preload_macros (src/turi/preload.c) is the shared helper the WASM
     * REPL also calls, so the two entry points cannot drift. */
    turi_env_preload_macros(env, resolve_stdlib_root());
    /* Inject typed stubs so the elaborator knows the signatures of the native
     * functions used by benchmark scripts and the carrier-list ops.  The native
     * shims registered below replace these no-op closures at runtime.  Factored
     * into turi_env_preload_native_stubs (src/turi/preload.c) so the REPL shares
     * the exact same set and cannot drift (the missing stubs there were the
     * `(list-head (cons ...)) => nil` REPL bug). */
    turi_env_preload_native_stubs(env);
    /* TI8.b/W1 (turi-interpreter-gap-closure-plan): preload the typeclass-stub
     * + typed-collection stdlib set that the compiled path auto-loads in
     * compile_to_c().  Without it the interpreter cannot resolve Cons/Option/
     * Result struct types, the Eq/Clone/Hash/HKT typeclasses, or the
     * map-new/vec-eq?/tcons/... collection functions -- so every fixture that
     * touches the typed stdlib errored under --interpret while passing when
     * compiled.
     *
     * Loaded via (load ...) (not turi_eval_file): several of these modules carry
     * their own (defmodule ...), and only the (load ...) preprocessing assigns
     * each file a distinct file_id so the per-file one-defmodule reset fires
     * (same root cause as the macros.tur fix above).  Loaded AFTER the benchmark
     * stub block (with the 6 overlapping stubs dropped) so the real module defns
     * own those names, and BEFORE wk_register_stdlib_natives so the native shims,
     * registered last, override any inline-C module body the interpreter cannot
     * execute.  Reader-backed modules (json/schema/sym) follow the same -X gates
     * as the compiled path.
     *
     * The array (safe, typeclass-*, hamt/set/map, vec/slice, option/result,
     * pair/tuple/list, grid/zipper, mutmap, unique, sym) and its batched-load
     * shape now live in turi_env_preload_collections (src/turi/preload.c), the
     * shared helper the WASM REPL also calls so the two entry points cannot
     * drift.  contract.tur is NOT in that list -- it is loaded up front next to
     * macros.tur (Phase M7 macro promotion is order-sensitive; see that load
     * site).  The inline-C bodies of these modules (map/set ops, contract
     * checks, mutmap, ...) are overridden by the native_* shims registered below
     * (wk_register_stdlib_natives et al.), which is why this preload runs BEFORE
     * that registration.  See docs/archive/history/turi-map-set-hamt-interpreter-gap.md
     * and docs/archive/history/web-repl-missing-stdlib-preload.md. */
    turi_env_preload_collections(env, resolve_stdlib_root());
    /* JR0/RD (turi-json-schema-interpreter-plan, Layers 1-2): auto-load
     * json.tur, then schema.tur on top of it, so the #json(...) reader-macro
     * lowering's json node constructors (json/encode|decode|type|get) and the
     * #json-str<T>(...) typed-decode reader family resolve under --interpret.
     * The loaded inline-C bodies are overridden by the layout-exact natives
     * registered in wk_register_{json,schema}_natives below.  Loaded BEFORE the
     * native overrides so those still win.  These two modules are therefore
     * unconditionally preloaded under --interpret (they are gaps only relative
     * to the static prelude[] above; docs/artifacts/turi-preload-carve-out.txt). */
    {
        extern bool g_turi_stdlib_preload;   /* stdlib-owned class marking */
        g_turi_stdlib_preload = true;
        char pb[4096];
        tur_stdlib_path("json.tur", pb, sizeof(pb));
        char load_form[4200];
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", pb);
        TuriValue sv = turi_eval(env, load_form);
        (void)sv;
    }
    {
        char pb[4096];
        tur_stdlib_path("schema.tur", pb, sizeof(pb));
        char load_form[4200];
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", pb);
        TuriValue sv = turi_eval(env, load_form);
        (void)sv;
    }
    {
        extern bool g_turi_stdlib_preload;
        g_turi_stdlib_preload = false;   /* preload done: user turns follow */
    }
    /* Pin the accumulated preload (prelude + json/schema above) so an inline
     * `#lang` directive in the evaluated program truncates src_acc back to here
     * rather than emptying it (web-repl-lang-switch-drops-stdlib). */
    turi_env_pin_prelude(env);
    /* Register the full interpreter native override set (stdlib inline-C
     * shims, contracts, safe/typeclass/comonad/mutex/future/bytes/taskgroup/
     * chan/backtrack/proc-fs/serial/sym/seq/json/schema).  Relocated into
     * src/turi/interpreter_natives.c so the WASM REPL and `tur repl` register
     * the same block and cannot drift.  Must run AFTER turi_env_preload_*. */
    turi_env_register_interpreter_natives(env);
    /* Build *args* as a cons-cell list of C-string pointers. */
    {
        typedef struct { int64_t value; int64_t next; } TurCons;
        int64_t args_list = 0;
        for (int i = extra_argc - 1; i >= 0; i--) {
            TurCons *c = (TurCons *)malloc(sizeof(TurCons));
            c->value = (int64_t)(intptr_t)extra_argv[i];
            c->next  = args_list;
            args_list = (int64_t)(intptr_t)c;
        }
        TuriValue args_val = {0};
        args_val.tag    = TURI_INT;
        args_val.as_int = args_list;
        turi_env_set(env, "*args*", args_val);
    }
    /* Set module_base_dir so (import ...) resolves relative to the script. */
    {
        const char *slash = strrchr(path, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - path);
            char *dpath = (char *)malloc(dlen + 1);
            memcpy(dpath, path, dlen);
            dpath[dlen] = '\0';
            env->module_base_dir = dpath;
        }
    }
    /* Debugger Phase 2: attach the debugger before top-level eval so the
     * (break) builtin resolves while the file is read; it stays UNARMED so
     * prelude + top-level forms run without stopping.  We arm it right before
     * main() below. */
    /* The (break) builtin is always available under the interpreter: a no-op
     * with no debugger attached, a pause trigger under `tur debug`.  Register
     * it before top-level eval so `(break)` resolves either way. */
    turi_debug_register_break_builtin(env);
    if (debug)
        turi_debug_enable(env, stdin, stdout);
    /* Phase 3 (DAP): now that the env + debugger exist, let the embedder stage
     * breakpoints and install its pause handler before any program node runs. */
    if (debug && hooks && hooks->on_ready)
        hooks->on_ready(env, hooks->ud);
    /* The user file is brought in via `(load "path")` rather than
     * turi_eval_file's concatenate-into-<eval> path: a loaded file gets its own
     * file_id and keeps its real path + 1-based line numbers, so diagnostics,
     * breakpoints (`break <line>`), source listings, and stack frames resolve
     * against the user's source instead of the synthetic <eval> blob (which
     * offsets every line by the preloaded prelude).
     *
     * incremental-elab-loses-span-file-provenance: this used to be the debug
     * path only, and plain `--interpret` went through turi_eval_file -- so a
     * type error in the user's file was reported as `<eval>:64:10` instead of
     * `file.tur:3:10`.  That half of the report was never actually about
     * incremental elaboration (TUR_NO_INCREMENTAL_ELAB=1 reproduced it
     * identically); it is the eval-blob concatenation, which both paths shared.
     * Routing both through `(load ...)` fixes it at the source.
     *
     * This became possible only once the load splicer learned to honour a
     * loaded file's inline `#lang` directive (load-ignores-inline-lang-directive,
     * elab_toplevel.c) -- before that, switching the entry file onto this route
     * broke every `#lang`/sweet-exp fixture that did not also carry a
     * dialect-bearing extension. */
    TuriValue result;
    {
        char load_form[4200];
        snprintf(load_form, sizeof load_form, "(load \"%s\")", path);
        result = turi_eval(env, load_form);
    }
    int rc = 0;
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        /* parse/elaboration errors are already printed by the diagnostic system */
        if (msg && strcmp(msg, "parse error") != 0 &&
                   strcmp(msg, "elaboration error") != 0) {
            fprintf(stderr, "tur: %s\n", msg);
        }
        rc = 1;
    } else {
        TuriValue main_fn = turi_env_get(env, "main");
        if (main_fn.tag == TURI_CLOSURE) {
            if (debug) turi_debug_arm(env);
            TuriValue r = turi_call(env, main_fn, NULL, 0);
            /* A runtime error (or uncaught throw) raised while running main was
             * previously swallowed -- main exited non-zero with no message.
             * Print it to stderr, mirroring the top-level error path above, so
             * `tur --interpret` surfaces the diagnostic (e.g. an unknown call
             * head) instead of failing silently. */
            if (r.tag == TURI_ERROR) {
                const char *msg = turi_error_message(r);
                if (msg && strcmp(msg, "parse error") != 0 &&
                           strcmp(msg, "elaboration error") != 0) {
                    fprintf(stderr, "tur: %s\n", msg);
                }
                rc = 1;
            }
            else if (r.tag == TURI_THROW) {
                /* TuriThrow is opaque here; print a generic notice (the inner
                 * message lives in eval.c) and fail non-zero. */
                fprintf(stderr, "tur: uncaught exception\n");
                rc = 1;
            }
            else if (r.tag == TURI_INT) rc = (int)r.as_int;
        }
        turi_run_pending_defers(env);
    }
    turi_env_free(env);
    return rc;
}

/* Back-compat wrapper: the common case with no embedder hooks. */
static int cmd_eval(const char *path, bool use_color,
                    char **extra_argv, int extra_argc, bool debug) {
    return cmd_eval_h(path, use_color, extra_argv, extra_argc, debug, NULL);
}

/* Debugger Phase 3: DAP launch glue.  The DAP server calls dap_launch_cb once
 * the client has finished configuration; it runs the program under the
 * interpreter debugger (cmd_eval_h, debug mode) and wires the DAP pause /
 * conditional handlers in via the on_ready hook before the program is armed. */
static void dap_on_ready_cb(TuriEnv *env, void *ud) {
    dap_begin_session(ud, env);   /* ud is the opaque DapState* */
}

static int dap_launch_cb(const char *program, char **args, int n_args,
                         void *state, void *ud) {
    (void)ud;
    EvalHooks hooks = { dap_on_ready_cb, state };
    /* use_color=false: stdout is the (captured) debuggee channel; diagnostics go
     * to stderr without colour, matching `tur lsp` / `tur mcp`. */
    return cmd_eval_h(program, /*use_color=*/false, args, n_args,
                      /*debug=*/true, &hooks);
}

static int cmd_dap(void) {
    diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
    return dap_server_run(STDIN_FILENO, STDOUT_FILENO, dap_launch_cb, NULL);
}

/* E3: tur eval '<expr>' — evaluate an inline expression and print result. */
static int cmd_eval_expr(const char *expr, bool use_color) {
    g_interpret_mode = true;
    turi_init(use_color);
    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "tur: failed to create interpreter environment\n");
        return 1;
    }
    TuriValue result = turi_eval(env, expr);
    int rc = 0;
    if (turi_is_error(result)) {
        const char *msg = turi_error_message(result);
        if (msg && strcmp(msg, "parse error") != 0 &&
                   strcmp(msg, "elaboration error") != 0) {
            fprintf(stderr, "tur: %s\n", msg);
        }
        rc = 1;
    } else if (result.tag != TURI_NIL) {
        char repr[512];
        turi_value_repr(repr, sizeof(repr), result);
        printf("%s\n", repr);
    }
    turi_env_free(env);
    return rc;
}

static int cmd_repl(bool watch_mode) {
    /* UCH0 (diagnose-unbound-call-heads-plan): the REPL is an interpreter
     * entry point -- flag it so the INT-1 reader conditional picks :turi and
     * so elab_call keeps the runtime-native dispatch fallback (UCH1). */
    g_interpret_mode = true;

    /* Resolve (and export via TUR_STDLIB_DIR) the stdlib root so turi_repl_run's
     * shared preload can (load ...) the prelude even when the REPL is launched
     * from outside the repo tree. */
    resolve_stdlib_root();

    /* UC-3 (user-config-experiments-plan): honor the user-level experiments
     * file for the interactive REPL, unless the enclosing project's build.tur
     * declares its own :experiments (which suppresses it).  CLI --enable=
     * (already applied in the global flag pass) still wins.  We read the
     * manifest only to decide suppression; the REPL's manifest-experiment
     * behavior is otherwise unchanged. */
    {
        char cwd[4096];
        char *root = getcwd(cwd, sizeof(cwd)) ? find_project_root(cwd) : NULL;
        char mp[4096];
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (root && pkg_resolve_manifest_path(root, mp, sizeof(mp))
            && pkg_manifest_read(mp, &m)) {
            apply_user_config_experiments(&m);
        } else {
            apply_user_config_experiments(NULL);
        }
        pkg_manifest_free(&m);
        free(root);
    }

    return turi_repl_run(watch_mode);
}

/* ---------------------------------------------------------------------------
 * Tier 3: tur worker — persistent fixture evaluator for the test suite.
 *
 * Reads fixture directory paths from stdin (one per line, blank = stop).
 * For each fixture, evaluates it using the interpreter in a forked child
 * (for isolation and stdout/stderr capture), compares outputs against
 * expected files, then writes a 4-line result file to $RESULTS_DIR and
 * prints PASS/FAIL/SKIP to stdout for live progress.
 *
 * The parent process persists across fixtures; only tur binary itself is
 * validated by syspolicyd at startup (N times for N workers), not once per
 * fixture.
 * --------------------------------------------------------------------------- */

/* drop-x-flags-plan: every -X<name> below is an accept-and-warn no-op as
 * of v0.24.0.  The flags are still recognized for one full minor line so
 * downstream build.tur files keep compiling unchanged; each one emits
 * TUR-W0050 instead of enabling anything.  The features themselves are
 * unconditionally on (see src/runtime/globals.c). */
static bool is_known_deprecated_x_flag(const char *s) {
    static const char *const names[] = {
        "-Xlinear", "-Xsubstructural", "-Xunique-types", "-Xgadt",
        "-Xunion-types", "-Xintersection-types", "-Xeffect-types",
        "-Xcontracts", "-Xsessions", "-Xdynamic-vars", "-Xcallcc",
        "-Xsized-types", "-Xdata-literals", "-Xjson-reader",
        "-Xschema-reader", "-Xsymbols",
    };
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        if (strcmp(s, names[k]) == 0) return true;
    }
    return false;
}

static void warn_deprecated_x_flag(const char *name) {
    fprintf(stderr, "warning [TUR-W0050]: %s is no longer needed; "
                    "the feature is on by default\n", name);
}

/* XF1 (experimental-flag-mechanism-plan): enable each comma-separated name in
 * a `--enable=<a,b,c>` value (or a manifest :experiments list).  An unknown
 * name is a hard error (TUR-E0310) so typos surface immediately.  Returns
 * true on success, false (after printing TUR-E0310) on the first unknown
 * name. `src` records the origin for the `tur experiments` source column. */
static bool enable_experiment_list(const char *list, ExperimentSource src) {
    if (!list || !*list) return true;
    char copy[1024];
    strncpy(copy, list, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    char *tok = strtok(copy, ",");
    while (tok) {
        /* trim leading whitespace */
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok) {
            if (!experiment_enable(tok, src)) {
                fprintf(stderr,
                        "error [TUR-E0310]: unknown experiment '%s'; "
                        "run 'tur experiments' for the list\n", tok);
                return false;
            }
        }
        tok = strtok(NULL, ",");
    }
    return true;
}

/* Apply flags string (e.g. "-Xgadt -Xlinear") to global compiler flags. */
static void wk_apply_flags(const char *flags_str) {
    if (!flags_str || !*flags_str) return;
    char copy[512];
    strncpy(copy, flags_str, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    char *tok = strtok(copy, " \t");
    while (tok) {
        if      (is_known_deprecated_x_flag(tok))             { /* TUR-W0050: silently accept in worker; fixtures get cleaned up separately. */ }
        else if (strcmp(tok, "--unsafe-stats")      == 0) { g_lint_unsafe_enabled = true; g_unsafe_stats_enabled = true; }
        else if (strcmp(tok, "--strict-effects")    == 0) g_strict_effects           = true;
        else if (strcmp(tok, "--strict-refine")     == 0) g_strict_refine            = true;
        else if (strcmp(tok, "--dump-effects")      == 0) g_dump_effects             = true;
        else if (strcmp(tok, "--dump-write-frames") == 0) g_dump_write_frames        = true;
        else if (strcmp(tok, "--dump-cps-coloring") == 0) g_dump_cps_coloring        = true;
        else if (strcmp(tok, "--dump-cps")          == 0) g_dump_cps                 = true;
        else if (strcmp(tok, "--dump-mono-specs")   == 0) g_dump_mono_specs          = true;
        else if (strcmp(tok, "--dump-cps-mono")     == 0) g_dump_cps_mono            = true;
        else if (strcmp(tok, "--dump-sizes")        == 0) g_dump_sizes               = true;
        else if (strcmp(tok, "--emit-abi-trace")    == 0) g_emit_abi_trace           = true;
        else if (strcmp(tok, "--lint-effects")      == 0) g_lint_effects             = true;
        else if (strcmp(tok, "--lint-unsafe")       == 0) { g_lint_unsafe_enabled = true; g_unsafe_warn_nested = true; }
        else if (strncmp(tok, "--lint-unsafe-max-lines=", 24) == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_max_lines = (uint32_t)atoi(tok + 24);
        }
        else if (strcmp(tok, "--lint-unsafe-doc")   == 0) { g_lint_unsafe_enabled = true; }
        else if (strcmp(tok, "--require-unsafe-docs")== 0) { g_lint_unsafe_enabled = true; g_unsafe_require_safety = true; }
        else if (strcmp(tok, "--lint-unsafe-nested")== 0) { g_lint_unsafe_enabled = true; }
        else if (strcmp(tok, "--lint-inline-c-unsafe") == 0) g_lint_inline_c_unsafe = true;
        else if (strcmp(tok, "--Werror=inline-c-narrow-params") == 0 ||
                 strcmp(tok, "-Werror=inline-c-narrow-params") == 0) g_werror_inline_c_narrow_params = true;
        else if (strncmp(tok, "--enable=", 9) == 0) {
            /* XF1: parent already validated the names (TUR-E0310); the worker
             * just re-applies them so gated features elaborate identically. */
            (void)enable_experiment_list(tok + 9, XF_SRC_CLI);
        }
        tok = strtok(NULL, " \t");
    }
}

/* Write a 4-line result file matching the format read by run.sh. */
static void wk_write_result(const char *results_dir, const char *kind,
                             const char *name, const char *detail,
                             const char *log_file) {
    char id[1024];
    snprintf(id, sizeof(id), "%s-%s", kind, name);
    for (char *p = id; *p; p++)
        if (*p == '/' || *p == ' ') *p = '_';
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s.result", results_dir, id);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n%s\n%s\n", kind, name, detail ? detail : "", log_file ? log_file : "");
    fclose(f);
}

/* Drain two read-end pipe fds concurrently into Bufs.
 * Closes both fds on return. */
#ifndef _WIN32
/*
 * Fixture worker (`tur worker`) -- forks a child per fixture so a crashing
 * fixture cannot take the worker process down with it, then polls the child's
 * stdout/stderr pipes.
 *
 * fork(2) and poll(2) have no Windows counterpart.  Doing this properly means
 * CreateProcess plus overlapped pipe reads -- a real port, not a shim.  It is
 * also purely a test-harness fast path: `tur build`, `emit-c` and `check` never
 * touch it.  So it is compiled out here, and cmd_worker() below reports that it
 * is unavailable; tests/run.sh then falls back to spawning `tur` once per
 * fixture, which is slower but correct.
 */
static void wk_drain_pipes(int fd_out, int fd_err, Buf *out_buf, Buf *err_buf) {
    struct pollfd pfds[2];
    pfds[0].fd = fd_out; pfds[0].events = POLLIN;
    pfds[1].fd = fd_err; pfds[1].events = POLLIN;
    int done_out = 0, done_err = 0;
    while (!done_out || !done_err) {
        pfds[0].fd = done_out ? -1 : fd_out;
        pfds[1].fd = done_err ? -1 : fd_err;
        pfds[0].revents = 0; pfds[1].revents = 0;
        int r = poll(pfds, 2, 1000);
        if (r < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; i++) {
            if (pfds[i].fd < 0) continue;
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                char tmp[8192];
                ssize_t n = read(pfds[i].fd, tmp, sizeof(tmp));
                if (n > 0) {
                    Buf *b = (i == 0) ? out_buf : err_buf;
                    buf_write(b, tmp, (size_t)n);
                } else {
                    if (i == 0) done_out = 1; else done_err = 1;
                }
            }
        }
    }
    close(fd_out); close(fd_err);
}

#endif /* _WIN32 -- wk_drain_pipes */

/* -------------------------------------------------------------------------
 * Native HAMT wrappers for the interpreter (Tier 3).
 * These replace the nil stubs registered by EX_EXTERN_C so that HAMT
 * operations actually work during interpreter evaluation.
 * ---------------------------------------------------------------------- */
#include "runtime/hamt.h"
#include "runtime/image.h"

/* ---------------------------------------------------------------------------
 * AI6 (application-image-dumps-plan): `tur image-info` / `tur image-verify`.
 *
 * These inspect/validate an application image file (see src/runtime/image.h)
 * without resuming it. Images are loaded by the *application* binary, not by
 * `tur`, so build-stamp validation here is against a binary the user names
 * explicitly as the second argument; with no binary, verify checks only the
 * structural header (magic/version/CRC) and prints the embedded stamp.
 * --------------------------------------------------------------------------- */
static void tur_image_print_stamp(const uint8_t stamp[32]) {
    for (int i = 0; i < 32; i++) printf("%02x", stamp[i]);
}

static int cmd_image_info(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "tur image-info: cannot open '%s'\n", path); return 1; }
    TurImageHeader h;
    TurImageError e = tur_image_read_header(f, &h);
    fclose(f);
    if (e != IMAGE_OK) {
        fprintf(stderr, "tur image-info: %s: %s\n", path, tur_image_strerror(e));
        return 1;
    }
    printf("image:        %s\n", path);
    printf("magic:        TURI (0x%08x)\n", h.magic);
    printf("version:      %u\n", h.version);
    printf("build-stamp:  "); tur_image_print_stamp(h.build_stamp); printf("\n");
    printf("payload-len:  %llu bytes\n", (unsigned long long)h.payload_len);
    printf("created:      %llu ns since epoch\n", (unsigned long long)h.created_unix_ns);
    printf("globals-off:  %llu\n", (unsigned long long)h.globals_offset);
    printf("flags:        0x%08x\n", h.flags);
    printf("header-crc32: 0x%08x\n", h.header_crc32);
    return 0;
}

static int cmd_image_verify(const char *path, const char *binary) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "tur image-verify: cannot open '%s'\n", path); return 2; }
    TurImageHeader h;
    TurImageError e = tur_image_read_header(f, &h);
    fclose(f);
    if (e != IMAGE_OK) {
        fprintf(stderr, "FAIL: %s: %s\n", path, tur_image_strerror(e));
        return 2;
    }
    if (binary) {
        uint8_t want[32];
        if (!tur_image_sha256_file(binary, want)) {
            fprintf(stderr, "tur image-verify: cannot read binary '%s'\n", binary);
            return 2;
        }
        if (memcmp(want, h.build_stamp, 32) != 0) {
            fprintf(stderr, "FAIL: build-stamp mismatch (image not produced by '%s')\n", binary);
            return 1;
        }
        printf("OK: header valid and build-stamp matches '%s'\n", binary);
        return 0;
    }
    printf("OK: header valid (magic/version/CRC). build-stamp: ");
    tur_image_print_stamp(h.build_stamp); printf("\n");
    printf("note: pass a loader binary as the 2nd arg to verify the build-stamp.\n");
    return 0;
}



/* Run the interpreter on 'input' in a forked child.
 * Returns child exit code; writes captured stdout and stderr into out and err.
 * Caller frees *out and *err. */
#ifndef _WIN32  /* fork-based fixture worker; see the note above */

static int wk_eval_fixture(const char *input, const char *flags_str,
                            const char *stdin_path, int timeout_secs,
                            char **out, char **err) {
    int pout[2], perr[2];
    if (pipe(pout) < 0 || pipe(perr) < 0) { *out = NULL; *err = NULL; return 1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(pout[0]); close(pout[1]);
        close(perr[0]); close(perr[1]);
        *out = NULL; *err = NULL; return 1;
    }

    if (pid == 0) {
        /* Child: redirect fds, apply flags, evaluate. */
        close(pout[0]); close(perr[0]);
        dup2(pout[1], STDOUT_FILENO);
        dup2(perr[1], STDERR_FILENO);
        close(pout[1]); close(perr[1]);

        if (stdin_path) {
            int fdin = open(stdin_path, O_RDONLY);
            if (fdin >= 0) { dup2(fdin, STDIN_FILENO); close(fdin); }
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }

        wk_apply_flags(flags_str);
        if (timeout_secs > 0) alarm((unsigned)timeout_secs);
        g_interpret_mode = true;

        TuriEnv *env = turi_env_new();
        /* Pre-load stdlib files so the elaborator has all standard definitions.
         * We skip contract.tur because it conflicts with the native stubs below.
         * hamt.tur and map.tur are needed for ^persistent map lowering (Phase P3).
         * safe.tur is needed for bounds-checked array ops. */
        {
            /* Basenames only — resolved at use time via tur_stdlib_path. */
            static const char *preload[] = {
                "macros.tur",
                "safe.tur",
                "hamt.tur",
                /* stdlib-hkt-consolidation T1: HKT class stubs so option.tur /
                 * result.tur HKT instances resolve in the worker eval path. */
                "typeclass-functor.tur",
                "typeclass-applicative.tur",
                "typeclass-alternative.tur",
                "typeclass-monad.tur",
                "typeclass-monaderror.tur",
                "typeclass-bifunctor.tur",
                /* Bug-5 follow-up: result.tur preloaded so ok/ok?/ok-val are
                 * globally available in the worker eval path too. */
                "result.tur",
                /* Phase TM0/TC1/TC2: typed parameterized collection stdlib files. */
                "map.tur",
                "vec.tur",
                "slice.tur",
                "option.tur",
                "result.tur",
                "pair.tur",
                "tuple.tur",
                "list.tur",
                "grid.tur",
                "zipper.tur",
                "set.tur",
                "mutmap.tur",
                NULL
            };
            for (int pi = 0; preload[pi]; pi++) {
                char path_buf[4096];
                tur_stdlib_path(preload[pi], path_buf, sizeof(path_buf));
                TuriValue sv = turi_eval_file(env, path_buf);
                (void)sv;
            }
        }
        /* Inject typed stubs for contract runtime helpers so the elaborator
         * knows their signatures.  Native functions override the closures at
         * runtime, but the elaborator uses the declared return type. */
        {
            TuriValue sv2 = turi_eval(env,
                "(defn contract-enabled? [] :bool true)\n"
                "(defn tur-contract-check [condition :bool msg :cstr] :void nil)\n"
                "(defn tur-contract-check-inv [obj :int pred :int msg :cstr] :void nil)");
            (void)sv2;
        }
        /* Register native implementations of contract runtime helpers.
         * These replace the inline-C bodies (which return nil in interpreter
         * mode) so that assertions actually check their conditions. */
        turi_env_register_native(env, "tur-contract-check",
                                 native_contract_check, NULL);
        turi_env_register_native(env, "tur-contract-check-inv",
                                 native_contract_check_inv, NULL);
        turi_env_register_native(env, "contract-enabled?",
                                 native_contract_enabled, NULL);
        /* Register native safe.tur stdlib implementations. */
        wk_register_safe_natives(env);
        /* Register native overrides for typeclass instance methods with inline-C bodies. */
        wk_register_typeclass_natives(env);
        /* Register native overrides for common stdlib inline-C patterns. */
        wk_register_stdlib_natives(env);
        /* Set module_base_dir to the fixture directory so that (import ...)
         * forms resolve sibling .tur files correctly. */
        {
            const char *slash = strrchr(input, '/');
            if (slash) {
                size_t dlen = (size_t)(slash - input);
                char *dpath = (char *)malloc(dlen + 1);
                memcpy(dpath, input, dlen);
                dpath[dlen] = '\0';
                env->module_base_dir = dpath;
            }
        }
        TuriValue v = turi_eval_file(env, input);
        int exit_code = 0;
        if (v.tag == TURI_ERROR) {
            exit_code = 1;
        } else {
            /* Fixtures that define (defn main [] :int ...) need an explicit call.
             * Use turi_call to invoke the closure directly, bypassing re-elaboration
             * so macros from the initial eval remain available. */
            TuriValue main_fn = turi_env_get(env, "main");
            if (main_fn.tag == TURI_CLOSURE) {
                TuriValue result = turi_call(env, main_fn, NULL, 0);
                if (result.tag == TURI_ERROR) {
                    exit_code = 1;
                } else if (result.tag == TURI_INT) {
                    exit_code = (int)result.as_int;
                }
            }
            /* Fire any module-level or top-level defers that were registered
             * outside of functions (e.g. bare (defer ...) at module scope). */
            turi_run_pending_defers(env);
        }
        turi_env_free(env);
        /* Print unsafe stats if enabled (matches compiler pipeline output). */
        if (g_unsafe_stats_enabled) {
            fprintf(stderr, "unsafe stats: %u blocks, %u total forms\n",
                    g_unsafe_block_count, g_unsafe_total_lines);
        }
        fflush(NULL);
        _exit(exit_code);
    }

    /* Parent: collect output. */
    close(pout[1]); close(perr[1]);
    Buf out_buf, err_buf;
    buf_init(&out_buf); buf_init(&err_buf);
    wk_drain_pipes(pout[0], perr[0], &out_buf, &err_buf);

    int ws = 0;
    waitpid(pid, &ws, 0);
    int rc = WIFEXITED(ws) ? WEXITSTATUS(ws) : 1;

    buf_putc(&out_buf, '\0');
    buf_putc(&err_buf, '\0');
    *out = out_buf.data;
    *err = err_buf.data;
    return rc;
}

/* Run compile_to_c in a forked child, capturing the generated C.
 * Returns 0 on success; sets *gen_c to malloc'd string (caller frees). */
static int wk_emit_c_fixture(const char *input, const char *flags_str, char **gen_c) {
    int pout[2];
    if (pipe(pout) < 0) { *gen_c = NULL; return 1; }

    pid_t pid = fork();
    if (pid < 0) { close(pout[0]); close(pout[1]); *gen_c = NULL; return 1; }

    if (pid == 0) {
        close(pout[0]);
        dup2(pout[1], STDOUT_FILENO);
        close(pout[1]);
        /* Suppress diagnostics in this child. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        wk_apply_flags(flags_str);
        diag_init(false);

        Buf cbuf; buf_init(&cbuf);
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        int rc = compile_to_c(input, &cbuf, NULL, 0,
                              (const char **)rm_p, rm_n);
        free_reader_macro_paths(rm_p, rm_n);
        if (rc == 0 && cbuf.data) {
            fwrite(cbuf.data, 1, cbuf.len, stdout);
        }
        buf_free(&cbuf);
        fflush(NULL);
        _exit(rc);
    }

    close(pout[1]);
    Buf cbuf; buf_init(&cbuf);
    char tmp[8192];
    ssize_t nr;
    while ((nr = read(pout[0], tmp, sizeof(tmp))) > 0)
        buf_write(&cbuf, tmp, (size_t)nr);
    close(pout[0]);

    int ws = 0;
    waitpid(pid, &ws, 0);
    int rc = WIFEXITED(ws) ? WEXITSTATUS(ws) : 1;
    buf_putc(&cbuf, '\0');
    *gen_c = cbuf.data;
    return rc;
}

/* Check that every non-empty line of expected_stderr_path appears in actual. */
static bool wk_check_stderr_substrings(const char *actual,
                                        const char *expected_path,
                                        char *missing_out, size_t missing_cap) {
    char *expected = NULL; size_t exp_len = 0;
    if (read_entire_file(expected_path, &expected, &exp_len) != 0) return true;
    bool ok = true;
    char *line = expected;
    while (line && *line && ok) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\r' || line[ll-1] == ' ')) line[--ll] = '\0';
        if (*line && !strstr(actual, line)) {
            snprintf(missing_out, missing_cap, "%s", line);
            ok = false;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(expected);
    return ok;
}

/* Tier 3: persistent fixture worker loop.
 * Each invocation handles an entire batch of fixtures without spawning a new
 * tur binary (no syspolicyd hit per fixture). */
static int cmd_worker(void) {
    const char *results_dir = getenv("RESULTS_DIR");
    if (!results_dir || !*results_dir) {
        fprintf(stderr, "tur worker: RESULTS_DIR not set\n");
        return 2;
    }
    const char *tsan_env = getenv("TUR_TSAN");
    bool tsan_active = tsan_env && strcmp(tsan_env, "1") == 0;

    turi_init(false);

    char linebuf[4096];
    while (fgets(linebuf, sizeof(linebuf), stdin)) {
        size_t n = strlen(linebuf);
        while (n > 0 && (linebuf[n-1] == '\n' || linebuf[n-1] == '\r')) linebuf[--n] = '\0';
        if (n == 0) continue;

        const char *dir = linebuf;
        const char *fixture_prefix = "tests/fixtures/";
        const char *name_part = (strncmp(dir, fixture_prefix, strlen(fixture_prefix)) == 0)
            ? dir + strlen(fixture_prefix) : dir;
        char name[512];
        strncpy(name, name_part, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        /* Find input file. */
        char input[4096];
        snprintf(input, sizeof(input), "%s/input.tur", dir);
        struct stat st;
        if (stat(input, &st) != 0) {
            snprintf(input, sizeof(input), "%s/%s.tur", dir, basename_of(dir));
            if (stat(input, &st) != 0) {
                printf("SKIP %s\n", name); fflush(stdout); continue;
            }
        }

        /* Skip if TSAN marker present and TSAN not active. */
        char marker[4096];
        snprintf(marker, sizeof(marker), "%s/requires.tsan", dir);
        if (stat(marker, &st) == 0 && !tsan_active) {
            wk_write_result(results_dir, "PASS", name, "(tsan-skipped)", "");
            printf("PASS %s\n", name); fflush(stdout); continue;
        }

        /* requires.compiled: cannot run in interpreter. */
        snprintf(marker, sizeof(marker), "%s/requires.compiled", dir);
        if (stat(marker, &st) == 0) {
            /* Print SKIP so the shell coordinator can route to compiled path. */
            printf("SKIP %s requires-compiled\n", name); fflush(stdout); continue;
        }

        /* Read fixture flags. */
        char flags[256] = "";
        {
            char fp[4096];
            snprintf(fp, sizeof(fp), "%s/flags", dir);
            FILE *ff = fopen(fp, "r");
            if (ff) {
                if (fgets(flags, sizeof(flags), ff)) {
                    size_t fl = strlen(flags);
                    while (fl > 0 && (flags[fl-1] == '\n' || flags[fl-1] == '\r')) flags[--fl] = '\0';
                }
                fclose(ff);
            }
        }

        /* Read per-fixture timeout (default 10, 0 = unlimited). */
        int fixture_timeout = 10;
        {
            char tp[4096];
            snprintf(tp, sizeof(tp), "%s/expected.timeout", dir);
            FILE *tf = fopen(tp, "r");
            if (tf) {
                char ts[32] = "";
                if (fgets(ts, sizeof(ts), tf)) { int tv = atoi(ts); if (tv >= 0) fixture_timeout = tv; }
                fclose(tf);
            }
        }

        /* Detect optional stdin file. */
        char stdin_path[4096];
        snprintf(stdin_path, sizeof(stdin_path), "%s/input.stdin", dir);
        bool has_stdin = (stat(stdin_path, &st) == 0);

        /* Check for codegen snapshot. */
        char expected_c_path[4096];
        snprintf(expected_c_path, sizeof(expected_c_path), "%s/expected.c", dir);
        bool needs_codegen = (stat(expected_c_path, &st) == 0);

        /* --- Codegen snapshot check (in a forked child for isolation) --- */
        const char *fail_detail = NULL;
        char log_path[4096] = "";
        if (needs_codegen) {
            char *gen_c = NULL;
            int emit_rc = wk_emit_c_fixture(input, flags, &gen_c);
            if (emit_rc != 0) {
                fail_detail = "emit-c failed";
            } else {
                char *expected_c = NULL; size_t exp_len = 0;
                if (read_entire_file(expected_c_path, &expected_c, &exp_len) == 0) {
                    if (strcmp(gen_c, expected_c) != 0) fail_detail = "codegen mismatch";
                    free(expected_c);
                }
            }
            free(gen_c);
            if (fail_detail) {
                char id[512];
                snprintf(id, sizeof(id), "happy-%s", name);
                for (char *p = id; *p; p++) if (*p == '/' || *p == ' ') *p = '_';
                snprintf(log_path, sizeof(log_path), "%s/%s.log", results_dir, id);
                FILE *lf = fopen(log_path, "w");
                if (lf) { fprintf(lf, "FAIL %s -- %s\n", name, fail_detail); fclose(lf); }
                wk_write_result(results_dir, "FAIL", name, fail_detail, log_path);
                printf("FAIL %s -- %s\n", name, fail_detail); fflush(stdout);
                continue;
            }
        }

        /* --- Interpreter evaluation --- */
        char *actual_out = NULL, *actual_err = NULL;
        int eval_rc = wk_eval_fixture(input, flags, has_stdin ? stdin_path : NULL,
                                       fixture_timeout, &actual_out, &actual_err);

        /* Compare stdout. */
        if (!fail_detail) {
            char exp_stdout_path[4096];
            snprintf(exp_stdout_path, sizeof(exp_stdout_path), "%s/expected.stdout", dir);
            if (stat(exp_stdout_path, &st) == 0) {
                char *expected_out = NULL; size_t exp_len = 0;
                if (read_entire_file(exp_stdout_path, &expected_out, &exp_len) == 0) {
                    if (strcmp(actual_out ? actual_out : "", expected_out) != 0)
                        fail_detail = "stdout mismatch";
                    free(expected_out);
                }
            }
        }

        /* Compare exit code. */
        if (!fail_detail) {
            char exp_exit_path[4096];
            snprintf(exp_exit_path, sizeof(exp_exit_path), "%s/expected.exit", dir);
            if (stat(exp_exit_path, &st) == 0) {
                char *econtent = NULL; size_t elen = 0;
                if (read_entire_file(exp_exit_path, &econtent, &elen) == 0) {
                    while (elen > 0 && (econtent[elen-1] == '\n' || econtent[elen-1] == '\r'
                                        || econtent[elen-1] == ' ')) econtent[--elen] = '\0';
                    if (strcmp(econtent, "nonzero") == 0) {
                        if (eval_rc == 0) fail_detail = "expected nonzero exit";
                    } else {
                        int expected_exit = atoi(econtent);
                        if (eval_rc != expected_exit) fail_detail = "exit code mismatch";
                    }
                    free(econtent);
                }
            } else {
                /* Default expected exit = 0 */
                if (eval_rc != 0) fail_detail = "exit code mismatch";
            }
        }

        /* Check stderr substrings. */
        if (!fail_detail) {
            char exp_stderr_path[4096];
            snprintf(exp_stderr_path, sizeof(exp_stderr_path), "%s/expected.stderr", dir);
            if (stat(exp_stderr_path, &st) == 0) {
                char missing[512] = "";
                if (!wk_check_stderr_substrings(actual_err ? actual_err : "",
                                                 exp_stderr_path, missing, sizeof(missing)))
                    fail_detail = "stderr mismatch";
            }
        }

        /* Write result and print progress. */
        if (fail_detail) {
            char id[512];
            snprintf(id, sizeof(id), "happy-%s", name);
            for (char *p = id; *p; p++) if (*p == '/' || *p == ' ') *p = '_';
            snprintf(log_path, sizeof(log_path), "%s/%s.log", results_dir, id);
            FILE *lf = fopen(log_path, "w");
            if (lf) {
                fprintf(lf, "FAIL %s -- %s\n", name, fail_detail);
                if (actual_out) fprintf(lf, "actual stdout:\n%s\n", actual_out);
                if (actual_err) fprintf(lf, "actual stderr:\n%s\n", actual_err);
                fclose(lf);
            }
            wk_write_result(results_dir, "FAIL", name, fail_detail, log_path);
            printf("FAIL %s -- %s\n", name, fail_detail);
        } else {
            wk_write_result(results_dir, "PASS", name, "", "");
            printf("PASS %s\n", name);
        }
        fflush(stdout);

        free(actual_out);
        free(actual_err);
    }
    return 0;
}

#else  /* _WIN32 */

static int cmd_worker(void) {
    fprintf(stderr,
            "tur worker: not supported on Windows (requires fork/poll).\n"
            "  Run the fixture suite without the worker fast path.\n");
    return 1;
}

#endif /* _WIN32 */

/* GS-M2: scan $PATH for `tur-*` executables and print them as an
 * "External commands:" block beneath the built-in usage. Built-in
 * subcommands always win at dispatch time, so we filter any `tur-<name>`
 * that shadows a built-in to avoid misleading the user. Within $PATH,
 * the first hit for each name wins (matching exec() resolution). */
static void list_external_subcommands(void) {
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return;

    static const char *const builtins[] = {
        "build", "compile", "link",
        "emit-c", "emit-h", "emit-cmake", "run", "repl", "worker",
        "eval", "doc", "explain", "test", "check", "format", "fmt",
        "parse-check", "audit-spans", "debug", "dap", "lsp-lite",
        "init", "add", "add-cmake", "fetch",
        "install", "uninstall", "list", "upgrade",
        NULL,
    };

    char **names = NULL;
    int n_names = 0, cap_names = 0;

    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg = colon ? (size_t)(colon - p) : strlen(p);
        if (seg > 0 && seg < 3500) {
            char dir[4096];
            snprintf(dir, sizeof(dir), "%.*s", (int)seg, p);
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (strncmp(de->d_name, "tur-", 4) != 0) continue;
                    const char *cmd = de->d_name + 4;
                    if (!*cmd) continue;
                    for (const char *q = cmd; *q; q++) {
                        if (!(isalnum((unsigned char)*q) || *q == '-' || *q == '_'))
                            goto skip;
                    }
                    {
                        bool shadowed = false;
                        for (int i = 0; builtins[i]; i++) {
                            if (strcmp(cmd, builtins[i]) == 0) { shadowed = true; break; }
                        }
                        if (shadowed) continue;
                    }
                    {
                        char full[8192];
                        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
                        if (access(full, X_OK) != 0) continue;
                    }
                    {
                        bool dup = false;
                        for (int i = 0; i < n_names; i++) {
                            if (strcmp(names[i], cmd) == 0) { dup = true; break; }
                        }
                        if (dup) continue;
                    }
                    if (n_names == cap_names) {
                        int nc = cap_names ? cap_names * 2 : 8;
                        char **nn = (char **)realloc(names, (size_t)nc * sizeof(char *));
                        if (!nn) { closedir(d); goto cleanup; }
                        names = nn;
                        cap_names = nc;
                    }
                    names[n_names++] = strdup(cmd);
                  skip: ;
                }
                closedir(d);
            }
        }
        if (!colon) break;
        p = colon + 1;
    }

    if (n_names == 0) goto cleanup;

    for (int i = 1; i < n_names; i++) {
        char *cur = names[i];
        int j = i;
        while (j > 0 && strcmp(names[j - 1], cur) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = cur;
    }

    fprintf(stderr, "\nexternal commands (tur-* on $PATH):\n");
    for (int i = 0; i < n_names; i++)
        fprintf(stderr, "  tur %s\n", names[i]);

cleanup:
    for (int i = 0; i < n_names; i++) free(names[i]);
    free(names);
}

/* Top-level subcommands, in canonical spelling. Kept in one place so
 * resolve_command() can prefix-match against them; the actual dispatch
 * chain in main() still uses strcmp against these same names. */
static const char *const CANONICAL_COMMANDS[] = {
    "emit-c", "emit-h", "emit-cmake", "check", "audit-spans",
    "lsp", "mcp", "dap", "lsp-lite",
    "build", "compile", "link", "run", "repl", "worker", "interpret", "debug",
    "eval", "doc", "image-info", "image-verify", "explain",
    "format", "fmt", "parse-check", "test",
    "new", "init", "add", "add-cmake", "fetch",
    "install", "uninstall", "list", "upgrade", "experiments", "lang-layers",
    NULL,
};

/* Sentinel: `tok` is a prefix of more than one canonical command. */
static const char *const COMMAND_AMBIGUOUS = (const char *)(intptr_t)-1;

/* Resolve a bare argv[1] token to a canonical command name.
 *   - exact match wins outright.
 *   - unique prefix -> canonical.
 *   - >1 prefix hits -> COMMAND_AMBIGUOUS.
 *   - flags (leading '-') and 0 hits -> NULL. */
static const char *resolve_command(const char *tok) {
    if (!tok || !*tok || tok[0] == '-') return NULL;
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++) {
        if (strcmp(tok, CANONICAL_COMMANDS[i]) == 0) return CANONICAL_COMMANDS[i];
    }
    size_t n = strlen(tok);
    const char *hit = NULL;
    for (int i = 0; CANONICAL_COMMANDS[i] != NULL; i++) {
        if (strncmp(tok, CANONICAL_COMMANDS[i], n) == 0) {
            if (hit != NULL) return COMMAND_AMBIGUOUS;
            hit = CANONICAL_COMMANDS[i];
        }
    }
    return hit;
}

static int usage(void) {
    fprintf(stderr,
        "tur: the Turmeric compiler (v" TUR_VERSION ")\n"
        "\n"
        "usage:\n"
        "  tur build <file.tur> [-o <out>]    build a single file\n"
        "  tur build <dir> [-o <out>]         build all .tur files in directory\n"
        "  tur compile <file.tur> -o <out.o>  lower a .tur to an object (+ .link sidecar)\n"
        "  tur link <obj/src>... -o <out>     link objects (+ .link sidecars) into an exe\n"
        "  tur emit-c <input.tur>            print the generated C to stdout\n"
        "  tur emit-h <input.tur>            print the generated header to stdout\n"
        "  tur run <input.tur>               build + execute a single file\n"
        "  tur repl                          interactive REPL (Phase S1)\n"
        "  tur worker                        persistent fixture evaluator (Tier 3, reads dirs from stdin)\n"
        "  tur interpret <file.tur>          run a file through the tree-walking interpreter\n"
        "  tur debug <file.tur>              run a file under the interactive debugger\n"
        "  tur dap                           Debug Adapter Protocol server (JSON-RPC/stdio) for editors\n"
        "  tur lsp-lite                      lightweight completion/calltip/doc backend (NDJSON/stdio)\n"
        "  tur eval '<expr>'                 evaluate an inline expression\n"
        "  tur doc <symbol>                  print documentation for a builtin or special form\n"
        "  tur explain <TUR-E####|snippet>   explain a diagnostic code or snippet errors\n"
        "  tur test <dir>                    run all .tur files in a directory\n"
        "  tur check <input.tur>             type-check only, no codegen (phase 8)\n"
        "  tur format [--check|--diff] [file.tur]   format source (stdin if no file given)\n"
        "  tur fmt [--check|--diff|--dry-run] [paths...]  format in place with dir walking\n"
        "  tur parse-check <a> <b>           exit 0 if both files read to the same AST\n"
        "  tur experiments                   list experimental features (--enable=<name>)\n"
        "  tur lang-layers                   list the `#lang` layers a file may request\n"
        "  tur completion <zsh|bash>         print a shell completion script\n"
        "\n"
        "package management (Spice, Phase PKG-1):\n"
        "  tur init [--bin|--lib] <name>     create a new project\n"
        "  tur add <url> [--ref <tag>]       add a Turmeric spice\n"
        "  tur add <path> --path             add a local spice\n"
        "  tur add --workspace <name>        assert a workspace sibling (no manifest entry)\n"
        "  tur add-cmake <url> [--ref <tag>] add a C/CMake dependency\n"
        "  tur fetch [--update|--dry-run|--refetch]  download/update spices (--refetch bypasses system pkgs)\n"
        "  tur emit-cmake [--output-dir <d>] generate CMakeLists.txt + config for CMake consumers\n"
        "  tur install <url> [--ref <ref>]   install a spice binary globally\n"
        "  tur install <path> --path         install a local spice binary globally\n"
        "  tur uninstall <name>              remove a globally installed spice\n"
        "  tur list [--verbose|--outdated]   list globally installed spices\n"
        "  tur upgrade <name>...|--all       upgrade globally installed spices\n");
    fprintf(stderr,
        "\n"
        "emit-c flags:\n"
        "  tur emit-c <file>                 compile a .tur file to C (stdout)\n"
        "  tur emit-c --output-dir <dir> <files...>  compile each .tur to <dir>/<mod>.h + .c\n"
        "\n"
        "global flags:\n"
        "  --enable=<name>[,<name>...]      turn on an experimental feature; see 'tur experiments'\n"
        "  --no-color                       disable colored diagnostics\n"
        "  --json                           structured JSON output (tur doc, tur test, tur check)\n"
        "  --json-diagnostics               output diagnostics as JSON (phase 8)\n"
        "  --explain <TUR-E####>            print explanation for a diagnostic code (HKT-P5)\n"
        "  --explain <snippet>              compile code snippet and explain errors (phase 8)\n"
        "  --dump-kinds                     dump kind annotations after kind-check (HKT-P6)\n"
        "  --strict-effects                 warn on unannotated effectful functions (ER1)\n"
        "  --strict-refine                  hard-fail refinement obligations the solver cannot prove\n"
        "  --dump-effects                   print inferred effect row for each defn (ER6)\n"
        "  --dump-write-frames              print the checked verdict for each `#writes` frame (G1)\n"
        "  --dump-cps-coloring              print whole-program may-capture coloring per defn (CPS1)\n"
        "  --dump-cps                       print the ANF/CPS IR for each colored defn (CPS2)\n"
        "  --lint-effects                   advisory warnings for unannotated effectful functions (ER6)\n"
        "  --backtrack-depth <N>            cap run-backtrack at N results (0=unlimited) (Phase B5)\n"
        "  --dump-clone-plan                dump cloneable capture plan after CPS (Phase B5)\n"
        "  --dump-cps-coloring              dump CPS coloring (colored/uncolored) per top-level defn (CPS1)\n"
        "  --cps-path                       emit CPS wrappers for colored functions (CPS3)\n"
        "  --emit-abi-trace                 print the resolved ABI path per call site during emit-c (Phase I)\n"
        "  --no-abi-cache                   disable the persistent cross-module ABI cache (.tur-abi-cache/) (Phase J6)\n"
        "  --panic-abort                   all panics call abort() directly (Phase R5)\n"
        "  --panic-trace                   print scope chain on panic (Phase R6)\n"
        "  --warn-unused-result             warn on discarded result values (Phase R6)\n"
        "  --no-warn-unused-result          disable --warn-unused-result (Phase R6)\n"
        "  --lint-panic                     lint panic/must! usage (Phase R6)\n"
        "  --no-contracts                   strip contract checks; predicates not evaluated (Phase C2)\n"
        "  --dump-sizes                     print inferred size index per sized-GADT constructor (SZ8)\n"
        "  --keep-contracts                 retain contract checks in release builds (CT3)\n"
        "  -X<name>                         recognized for backwards compatibility; all language\n"
        "                                   features previously gated by -X flags are now on by default.\n"
        "                                   Each -X<name> emits TUR-W0050 and is otherwise ignored.\n"
        "                                   See docs/guides/compiler-flags-guide.md for the removal list.\n");
    list_external_subcommands();
    return 64;
}

/* E1: per-subcommand help strings */
static int usage_build(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur build [-I <dir>...] <file.tur> [-o <out>]   build a single file\n"
        "  tur build <dir> [-o <out>]                       build all .tur in dir\n"
        "  tur build --shared <dir> [-o <out>] [--manifest <p>]  build a shared library (.so)\n"
        "  tur compile [-I <dir>...] <file.tur> -o <out.o>  lower a .tur to an object + .link\n"
        "  tur link [--shared] <obj/src>... -o <out>        link objects (+ .link) into an exe/.so\n"
        "  tur emit-c [-I <dir>...] <file.tur>              emit C to stdout\n"
        "  tur emit-c [-I <dir>...] --build-dir <dir> <files...>  emit per-module .h/.c\n"
        "  tur emit-h [-I <dir>...] <file.tur>              emit header to stdout\n"
        "\n"
        "flags:\n"
        "  -o <out>          output file path\n"
        "  -I <dir>          add include directory for module resolution\n"
        "                    (repeat to add multiple; intra-spice imports usually\n"
        "                    want `-I src` from the spice root)\n"
        "  -B, --build-dir <d>  route generated .c/.h/.o + final artifact into <d>\n"
        "                    (subdirs: obj/, bin/, lib/). Defaults to\n"
        "                    <project-root>/build or <cwd>/build. Override layers:\n"
        "                    CLI flag > TUR_BUILD_DIR env > build.tur :build-dir.\n"
        "  --shared          build a shared library (`-fPIC -shared`, no main);\n"
        "                    requires a directory argument. Exported defns are\n"
        "                    callable via dlopen/dlsym as `<module>__<name>`.\n"
        "  --split-build     build a single file as `compile` + `link` (cacheable\n"
        "                    `cc -c` object compiles + a link). Native builds only.\n"
        "  --no-split-build  force the monolithic single-`cc` build (the default).\n"
        "  --runtime=auto    (default) link the lean non-ASan libturt_runtime.a when\n"
        "                    locatable, else recompile the bare src/runtime autolink\n"
        "                    sources -- never links the ASan libturi.a on its own, so\n"
        "                    a default build matches the old source path.\n"
        "  --runtime=lib     force the archive link (lean preferred, else libturi.a).\n"
        "                    Set TUR_RUNTIME_LIB to point at the archive if not found.\n"
        "  --runtime=source  force recompiling the runtime sources.\n"
        "                    TUR_RUNTIME=auto|lib|source seeds the default for a build.\n"
        "  --link-flags <f>  (tur link) extra linker flags, e.g. \"-L<dir> -lfoo\"\n"
        "  --manifest <p>    (with --shared) write exports.manifest to <p>\n"
        "                    (defaults to `<out>.manifest`). Lists each export\n"
        "                    as `<mod>/<defn> -> <mangled> :: (:args) -> :ret`.\n"
        "  --target wasm     compile to WebAssembly via emcc (requires Emscripten)\n"
        "  --debug           emit `#line` directives mapping the generated C back\n"
        "                    to `.tur` source, and compile single-file builds with\n"
        "                    `-g -O0` so gdb/lldb step through Turmeric source.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_run(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur run [-I <dir>...] <file.tur> [-- <args...>]\n"
        "                                  build and execute a single file\n"
        "  tur run [-I <dir>...] - [-- <args...>]\n"
        "                                  read source from stdin, build and execute\n"
        "\n"
        "flags:\n"
        "  -I <dir>    add an include directory for module resolution\n"
        "  --release   optimized build\n"
        "  --offline   skip dependency fetch\n"
        "  --          pass remaining arguments to the program\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_check(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur check [-I <dir>...] [--no-auto-spice] [--no-auto-stdlib] <file.tur>\n"
        "                                       type-check only, no codegen\n"
        "\n"
        "flags:\n"
        "  -I <dir>             add an include directory for module resolution\n"
        "                       (repeat to add multiple)\n"
        "  --no-auto-spice      don't auto-discover the enclosing spice's src/\n"
        "                       (default behavior walks up from the file looking\n"
        "                       for build.tur and adds <spice>/src to the path)\n"
        "  --no-auto-stdlib     when the checked file is one of the auto-loaded\n"
        "                       stdlib files, skip auto-loading it and all\n"
        "                       subsequent stdlib files; earlier stdlib files\n"
        "                       still load so the file's own deps resolve.\n"
        "                       Use when type-checking auto-loaded stdlib files\n"
        "                       in isolation to avoid duplicate-definition errors.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* SC1: is argv[i] the start of a `-I <path>` or `-I<path>` flag?
 * On true, writes the number of argv positions it consumes into *consumed
 * (1 for "-Ifoo", 2 for "-I foo").  Callers loop over argv looking for
 * non-include flags and skip these positions. */
static bool is_include_flag(int argc, char **argv, int i, int *consumed) {
    const char *a = argv[i];
    if (strcmp(a, "-I") == 0 && i + 1 < argc) {
        *consumed = 2;
        return true;
    }
    if (strncmp(a, "-I", 2) == 0 && a[2] != '\0') {
        *consumed = 1;
        return true;
    }
    return false;
}

/* SC1: collect every -I flag from argv[start..argc-1] into a heap-allocated
 * array.  The array of pointers is malloc'd (caller frees); the strings it
 * points to are borrowed from argv and live as long as argv does.
 *
 * Returns the number of include dirs collected, or -1 if a bare `-I` was
 * encountered without a following path argument.  Non-`-I` arguments are
 * ignored -- callers handle them in their own pass (using is_include_flag
 * to skip the positions consumed here). */
static int parse_include_flags(int argc, char **argv, int start, char ***out_dirs) {
    int  cap = 4;
    int  n   = 0;
    char **dirs = (char **)malloc((size_t)cap * sizeof(char *));
    if (!dirs) { *out_dirs = NULL; return 0; }
    for (int i = start; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-I") == 0) {
            if (i + 1 >= argc) { free(dirs); *out_dirs = NULL; return -1; }
            if (n >= cap) {
                cap *= 2;
                char **bigger = (char **)realloc(dirs, (size_t)cap * sizeof(char *));
                if (!bigger) { free(dirs); *out_dirs = NULL; return n; }
                dirs = bigger;
            }
            dirs[n++] = argv[++i];
        } else if (strncmp(a, "-I", 2) == 0 && a[2] != '\0') {
            if (n >= cap) {
                cap *= 2;
                char **bigger = (char **)realloc(dirs, (size_t)cap * sizeof(char *));
                if (!bigger) { free(dirs); *out_dirs = NULL; return n; }
                dirs = bigger;
            }
            dirs[n++] = argv[i] + 2;
        }
    }
    *out_dirs = dirs;
    return n;
}

static int usage_eval(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur eval '<expr>'          evaluate an inline expression and print the result\n"
        "  tur eval --file <file.tur> run a .tur file through the interpreter\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_doc(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur doc <symbol>          print documentation for a builtin or special form\n"
        "  tur doc --json <symbol>   print documentation as JSON\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* E5: tur doc <symbol> -- print documentation for a builtin or special form. */
static int cmd_doc_cli(const char *sym) {
    const char *d = turi_doc_lookup_builtin(sym);
    if (d) {
        if (use_json_output) {
            char esc_sym[128], esc_doc[512];
            json_escape(sym, esc_sym, sizeof(esc_sym));
            json_escape(d,   esc_doc, sizeof(esc_doc));
            printf("{\"name\":\"%s\",\"doc\":\"%s\"}\n", esc_sym, esc_doc);
        } else {
            printf("%s\n", d);
        }
        return 0;
    }
    if (use_json_output)
        fprintf(stderr, "{\"error\":\"no documentation for '%s'\"}\n", sym);
    else
        fprintf(stderr, "no documentation for '%s'\n", sym);
    return 1;
}

static int usage_format(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur format [file.tur]          format a source file (stdin if no file given)\n"
        "  tur format --check [file.tur]  exit 1 if formatting would change the file\n"
        "  tur format --diff [file.tur]   print unified diff of formatting changes\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_parse_check(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur parse-check <a> <b>   exit 0 if both files read to the same AST\n"
        "\n"
        "  <a> is read as turmeric, <b> as sweet-exp, unless an explicit\n"
        "  #lang directive overrides. Exit codes: 0 equal, 1 mismatch, 2 error.\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_test(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur test <dir>   run all .tur test files in a directory\n"
        "\n"
        "Each test compiles and runs; it passes iff both succeed (exit 0).\n"
        "A test file may carry directives in its leading comment lines:\n"
        "  ;; tur-test-flags: --strict-refine   extra compile flags for this test\n"
        "  ;; tur-test-expect-error: TUR-W0372  this test must FAIL to compile and\n"
        "                                       its diagnostics must contain the\n"
        "                                       text; the run phase is skipped\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_repl(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur repl [--watch] [--engine <name>]   start the interactive REPL\n"
        "\n"
        "flags:\n"
        "  --watch         auto-reload the enclosing spice between prompts\n"
        "                  when any source .tur file's mtime advances\n"
        "                  (RP6; equivalent to typing (reload) each turn)\n"
        "  --engine <name> engine for building the enclosing spice:\n"
        "                  \"cc\" (default -- build --shared subprocess) or\n"
        "                  \"jit\" (in-process MIR, needs -DTUR_JIT=ON).\n"
        "                  Precedence: --engine > TUR_ENGINE > build.tur\n"
        "                  :engine > \"cc\"\n"
        "\n"
        "REPL commands:\n"
        "  :help           print help\n"
        "  :quit / :q      exit the REPL\n"
        "  :doc <sym>      look up documentation for a symbol\n"
        "  :type <expr>    print the inferred type of an expression\n"
        "  :reload <file>  reload a source file\n"
        "  :reset          clear session and start fresh\n"
        "  :tutorial       start the interactive tutorial\n"
        "\n"
        "REPL forms:\n"
        "  (reload)        rebuild the loaded spice and refresh bindings\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_explain(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur explain <TUR-E####>    print explanation for a diagnostic code\n"
        "  tur explain '<snippet>'   compile a snippet and explain errors\n"
        "\n"
        "examples:\n"
        "  tur explain TUR-E0042\n"
        "  tur explain '(+ 1 \"x\")'\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

/* Phase 8: Handle --no-color flag */
static bool parse_no_color(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-color") == 0) {
            return true;
        }
    }
    return false;
}

/* SC4: --no-auto-spice disables the per-file subcommand walk-up that
 * adds the enclosing spice's `src/` to the include path.  Inspected by
 * auto_append_spice_src(). */
static bool g_no_auto_spice = false;

/* --no-auto-stdlib (tur check only): suffix-skip -- when the file being
 * checked IS one of the auto-loaded stdlib files, skip auto-loading it
 * and all subsequent stdlib files.  Earlier entries still load so the
 * file's transitive dependencies resolve.  This prevents duplicate-
 * definition errors when type-checking auto-loaded stdlib files in isolation. */
static bool g_no_auto_stdlib = false;

static bool parse_no_auto_spice(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-auto-spice") == 0) return true;
    }
    return false;
}

/* J6: --no-abi-cache (or TUR_NO_ABI_CACHE=1) disables the persistent
 * cross-module ABI specialization cache under <build-root>/.tur-abi-cache/.
 * The cache is a build-time optimization; disabling it must always still
 * produce a correct build (clones are recomputed per invocation). */
static bool g_no_abi_cache = false;

static bool parse_no_abi_cache(int argc, char **argv) {
    const char *env = getenv("TUR_NO_ABI_CACHE");
    if (env && *env && strcmp(env, "0") != 0) return true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-abi-cache") == 0) return true;
    }
    return false;
}

/* Phase R5: Handle --panic-abort flag */
static bool parse_panic_abort(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            return true;
        }
    }
    return false;
}

/* Debugger Phase 4: --debug emits `#line` source-map directives into the
 * generated C and builds single-file targets with `-g -O0`. */
static bool parse_debug_build(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --panic-trace flag */
static bool parse_panic_trace(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-trace") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --warn-unused-result flag */
static bool parse_warn_unused_result(int argc, char **argv) {
    bool enabled = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--warn-unused-result") == 0) {
            enabled = true;
        } else if (strcmp(argv[i], "--no-warn-unused-result") == 0) {
            enabled = false;
        }
    }
    return enabled;
}

/* F4 (cross-plan-followups): --Werror=deprecated flag promotes
 * ^deprecated use-site warnings to errors. */
static bool parse_werror_deprecated(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=deprecated") == 0 ||
            strcmp(argv[i], "-Werror=deprecated") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase C: --Werror=inline-c-narrow-params promotes narrow-param-in-inline-C
 * warnings to hard errors so a strict build can gate against unannotated
 * narrow parameters reaching inline-C bodies. */
static bool parse_werror_inline_c_narrow_params(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--Werror=inline-c-narrow-params") == 0 ||
            strcmp(argv[i], "-Werror=inline-c-narrow-params") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase R6: Handle --lint-panic flag */
static bool parse_lint_panic(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lint-panic") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase C2: Handle --no-contracts flag */
static bool parse_no_contracts(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-contracts") == 0) {
            return true;
        }
    }
    return false;
}

/* Phase 8: Handle --explain flag - compile code snippet and show detailed error */

/* Phase HKT-P5: Return true if `s` looks like a diagnostic code string
 * of the form "TUR-E" followed by one or more decimal digits. */
static bool looks_like_diag_code_(const char *s) {
    return diag_looks_like_code(s);
}

/* XF3 (experimental-flag-mechanism-plan): `tur experiments` -- list the
 * EXPERIMENTS[] registry.  This is the single source of truth; guides and the
 * docs site are generated from it, not maintained by hand. */
static const char *xf_lifecycle_str(ExperimentLifecycle lc) {
    return lc == XF_LIFECYCLE_BETA ? "beta" : "prototype";
}

static const char *xf_source_str(ExperimentSource src) {
    switch (src) {
        case XF_SRC_CLI:         return "cli";
        case XF_SRC_MANIFEST:    return "manifest";
        case XF_SRC_USER_CONFIG: return "user-config";
        default:                 return "-";
    }
}

/* Minimal JSON string escape (the registry fields are ASCII path/identifier
 * strings, but quote/backslash are handled for robustness). */
static void xf_json_puts(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static int cmd_experiments(int argc, char **argv) {
    /* `--json` is consumed by the global flag pass before dispatch (it sets
     * use_json_output); honour both that and a local --json for robustness. */
    bool json = use_json_output;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage:\n  tur experiments [--json]\n\n"
                   "List the compiler's experimental features (the --enable=<name>\n"
                   "registry).  --json emits the machine-readable form the docs site\n"
                   "consumes.\n");
            return 0;
        } else {
            fprintf(stderr, "tur experiments: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    /* UC-3: reflect the ambient configuration in the source column so a user
     * can see where each enabled flag came from.  CLI --enable= was already
     * applied in the global flag pass; consult the enclosing project manifest
     * (if any) and the user-level experiments file here, honoring the same
     * suppression rule as a compile. */
    {
        char cwd[4096];
        char *root = getcwd(cwd, sizeof(cwd)) ? find_project_root(cwd) : NULL;
        char mp[4096];
        PkgManifest m;
        memset(&m, 0, sizeof(m));
        if (root && pkg_resolve_manifest_path(root, mp, sizeof(mp))
            && pkg_manifest_read(mp, &m)) {
            apply_user_config_experiments(&m);
            apply_manifest_experiments(&m);
        } else {
            apply_user_config_experiments(NULL);
        }
        pkg_manifest_free(&m);
        free(root);
    }

    size_t n = experiment_count();

    if (json) {
        printf("[");
        for (size_t i = 0; i < n; i++) {
            const ExperimentDescriptor *d = experiment_at(i);
            if (i) printf(",");
            printf("\n  {");
            printf("\"name\":");        xf_json_puts(stdout, d->name);
            printf(",\"summary\":");    xf_json_puts(stdout, d->summary);
            printf(",\"lifecycle\":");  xf_json_puts(stdout, xf_lifecycle_str(d->lifecycle));
            printf(",\"introduced\":"); xf_json_puts(stdout, d->introduced);
            printf(",\"expires_at\":"); xf_json_puts(stdout, d->expires_at);
            printf(",\"plan\":");       xf_json_puts(stdout, d->plan_path);
            printf(",\"enabled\":%s",   experiment_is_enabled(d->name) ? "true" : "false");
            printf(",\"source\":");     xf_json_puts(stdout, xf_source_str(experiment_source_at(i)));
            printf("}");
        }
        printf("%s]\n", n ? "\n" : "");
        return 0;
    }

    if (n == 0) {
        printf("No experimental features are registered.\n\n"
               "The --enable=<name> mechanism is in place, but no feature is\n"
               "gated behind it right now.  See "
               "docs/guides/experimental-flags-guide.md.\n");
        return 0;
    }

    printf("%-22s %-9s %-10s %-10s %-9s %s\n",
           "NAME", "LIFECYCLE", "INTRODUCED", "EXPIRES", "ENABLED", "PLAN");
    for (size_t i = 0; i < n; i++) {
        const ExperimentDescriptor *d = experiment_at(i);
        char enabled[24];
        if (experiment_is_enabled(d->name))
            snprintf(enabled, sizeof(enabled), "yes(%s)",
                     xf_source_str(experiment_source_at(i)));
        else
            snprintf(enabled, sizeof(enabled), "no");
        printf("%-22s %-9s %-10s %-10s %-9s %s\n",
               d->name, xf_lifecycle_str(d->lifecycle), d->introduced,
               d->expires_at, enabled, d->plan_path);
    }
    printf("\n%zu experimental feature%s registered.\n", n, n == 1 ? "" : "s");
    return 0;
}

/* L5: list the curated `#lang` layer registry (LANG_LAYERS[]), mirroring
 * `tur experiments`.  `--json` emits the machine-readable form. */
static int cmd_lang_layers(int argc, char **argv) {
    bool json = use_json_output;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage:\n  tur lang-layers [--json]\n\n"
                   "List the curated `#lang` additive layers.  A `#lang "
                   "<base> <layer>*`\nline may name any of these after the "
                   "base dialect; each is order-\nindependent and file-scoped."
                   "  --json emits the machine-readable form.\n");
            return 0;
        } else {
            fprintf(stderr, "tur lang-layers: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    size_t n = lang_layers_count();

    if (json) {
        printf("[");
        for (size_t i = 0; i < n; i++) {
            const LangLayerDescriptor *d = lang_layer_at(i);
            if (i) printf(",");
            printf("\n  {");
            printf("\"name\":");    xf_json_puts(stdout, d->name);
            printf(",\"kind\":");   xf_json_puts(stdout,
                                        d->kind == LAYER_READER ? "reader"
                                                                : "semantic");
            printf(",\"summary\":"); xf_json_puts(stdout, d->summary);
            printf(",\"since\":");  xf_json_puts(stdout, d->since);
            if (d->kind == LAYER_SEMANTIC && d->experiment) {
                printf(",\"experiment\":"); xf_json_puts(stdout, d->experiment);
            }
            printf("}");
        }
        printf("%s]\n", n ? "\n" : "");
        return 0;
    }

    if (n == 0) {
        printf("No `#lang` layers are registered.\n");
        return 0;
    }

    printf("%-12s %-9s %-7s %s\n", "NAME", "KIND", "SINCE", "SUMMARY");
    for (size_t i = 0; i < n; i++) {
        const LangLayerDescriptor *d = lang_layer_at(i);
        printf("%-12s %-9s %-7s %s\n",
               d->name, d->kind == LAYER_READER ? "reader" : "semantic",
               d->since, d->summary);
    }
    printf("\n%zu `#lang` layer%s registered.\n", n, n == 1 ? "" : "s");
    return 0;
}

static int cmd_explain(const char *code) {
    /* Phase HKT-P5: If the argument looks like a diagnostic code (TUR-E####),
     * look it up in the explanation table instead of compiling a snippet. */
    if (looks_like_diag_code_(code)) {
        DiagCode dc = diag_code_from_string(code);
        if (dc == DIAG_CODE_NONE || !diag_explain(dc, stdout)) {
            fprintf(stderr, "tur: no explanation for '%s'\n", code);
            return 1;
        }
        return 0;
    }

    /* Legacy behaviour: compile a Turmeric source snippet and surface errors. */
    SourceFile file = {0};
    file.path = "<explain>";
    file.src = code;
    file.len = strlen(code);
    file.file_id = 0;
    file.reader_type = READER_TURMERIC;  /* Default for explain snippets */
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    if (!forms || diag_had_error()) {
        /* Error already emitted with enhanced diagnostics */
        symtab_free(&st);
        arena_free(&arena);
        return 1;
    }

    Expr *prog = elaborate_program(&arena, &st, forms, nforms, 0, ".", false, false, NULL,
                                    NULL, 0, NULL, NULL);
    if (!prog || diag_had_error()) {
        /* Error already emitted */
        symtab_free(&st);
        arena_free(&arena);
        return 1;
    }

    /* If no error, just say so */
    fprintf(stderr, "No errors found in the provided code.\n");

    symtab_free(&st);
    arena_free(&arena);
    return 0;
}

/* Phase 8: Handle --json-diagnostics flag */
static bool use_json_diagnostics = false;

/* GS-M2: subcommand fallthrough. When `tur <cmd>` doesn't match a built-in,
 * search $PATH for `tur-<cmd>` and exec it with the remaining argv. Modeled
 * after git-foo / git foo. On success this does not return. On failure
 * prints a diagnostic and returns 1. */
static int try_external_subcommand(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    if (!cmd || !*cmd || cmd[0] == '-') return usage();
    /* Reject anything that's clearly a path or contains shell-unsafe bits. */
    for (const char *p = cmd; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) return usage();
    }
    char ext_name[256];
    snprintf(ext_name, sizeof(ext_name), "tur-%s", cmd);

    const char *path_env = getenv("PATH");
    if (path_env) {
        const char *p = path_env;
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t seg = colon ? (size_t)(colon - p) : strlen(p);
            if (seg > 0 && seg < 3500) {
                char candidate[4096];
                snprintf(candidate, sizeof(candidate), "%.*s/%s",
                         (int)seg, p, ext_name);
                if (access(candidate, X_OK) == 0) {
                    char **nv = (char **)calloc((size_t)argc, sizeof(char *));
                    if (!nv) return 1;
                    nv[0] = (char *)ext_name;
                    for (int i = 2; i < argc; i++) nv[i - 1] = argv[i];
                    nv[argc - 1] = NULL;
                    execv(candidate, nv);
                    fprintf(stderr, "tur: failed to exec '%s': %s\n",
                            candidate, strerror(errno));
                    free(nv);
                    return 1;
                }
            }
            if (!colon) break;
            p = colon + 1;
        }
    }
    fprintf(stderr,
        "tur: '%s' is not a tur command. See 'tur --help'.\n", cmd);
    return 1;
}


int main(int argc, char **argv) {
#ifdef _WIN32
    /*
     * Put stdout/stderr in binary mode so a newline stays one byte.
     *
     * Windows opens them in text mode by default, which silently rewrites every
     * '\n' to "\r\n" on the way out.  Three things here break on that, and two
     * of them break silently:
     *
     *   - `tur emit-c` writes generated C to stdout, so every line would gain a
     *     \r and stop matching the tests/fixtures/<name>/expected.c snapshots.
     *   - The LSP and DAP servers frame messages with a Content-Length byte
     *     count computed BEFORE the text-mode expansion -- so the count would
     *     understate the bytes actually written and desynchronise the protocol.
     *   - `tur fmt --stdout` would rewrite the file's line endings as a side
     *     effect of printing it.
     *
     * Binary mode makes output byte-exact, matching every other platform.
     */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    /* SN1: stash argv[0] for exe-path fallback in resolve_stdlib_root().
     * Platform APIs (_NSGetExecutablePath / /proc/self/exe) are tried
     * first; argv[0] is the last-resort path source. */
    g_argv0 = (argc > 0) ? argv[0] : NULL;
    /* Resolve the stdlib root once at startup so TUR_STDLIB_DIR is
     * propagated into the process env before any subsystem (elaborator,
     * worker, interpreter) reads it. */
    (void)resolve_stdlib_root();

#ifdef TUR_HAVE_JIT
    /* jit-ffi-c2mir-plan: install the c2mir-backed dynamic-FFI provider so
     * the interpreter's extern-c registration, call-ptr routing, and the
     * spice FFI ladder can synthesize call thunks at runtime.  JIT builds
     * only; without it every consumer keeps the non-JIT fallback behavior. */
    tur_jit_ffi_install();
#endif

    /* Phase 8: Check for global flags before command */
    bool no_color = parse_no_color(argc, argv);
    bool explain_mode = false;
    const char *explain_code = NULL;
    g_panic_abort = parse_panic_abort(argc, argv);
    g_emit_panic_trace = parse_panic_trace(argc, argv);
    g_warn_unused_result = parse_warn_unused_result(argc, argv);
    g_lint_panic = parse_lint_panic(argc, argv);
    /* Phase C2: --no-contracts strips contract checks (release builds). */
    g_no_contracts = parse_no_contracts(argc, argv);
    /* Debugger Phase 4: --debug emits `#line` directives + builds with -g -O0. */
    g_emit_debug_lines = parse_debug_build(argc, argv);
    /* F4: --Werror=deprecated promotes ^deprecated warnings to errors */
    g_werror_deprecated = parse_werror_deprecated(argc, argv);
    /* Phase C: --Werror=inline-c-narrow-params promotes narrow-param warnings */
    g_werror_inline_c_narrow_params = parse_werror_inline_c_narrow_params(argc, argv);
    /* SC4: --no-auto-spice disables enclosing-spice auto-discovery in
     * per-file subcommands (check/emit-c/emit-h/run). */
    g_no_auto_spice = parse_no_auto_spice(argc, argv);
    /* J6: --no-abi-cache / TUR_NO_ABI_CACHE disables the persistent
     * cross-module ABI specialization cache (.tur-abi-cache/). */
    g_no_abi_cache = parse_no_abi_cache(argc, argv);
    /* tur-link-and-build-split-plan Phase 2/3c/6: TUR_RUNTIME overrides the
     * default runtime-linkage mode (auto).  A CLI --runtime= flag, parsed
     * later, still wins. */
    {
        const char *rt = getenv("TUR_RUNTIME");
        if (rt) {
            if      (strcmp(rt, "lib") == 0)    g_runtime_mode = TUR_RT_LIB;
            else if (strcmp(rt, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
            else if (strcmp(rt, "auto") == 0)   g_runtime_mode = TUR_RT_AUTO;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-auto-spice") == 0) {
            /* SC4: already parsed into g_no_auto_spice; strip from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--panic-trace") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--warn-unused-result") == 0 ||
                   strcmp(argv[i], "--no-warn-unused-result") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=deprecated") == 0 ||
                   strcmp(argv[i], "-Werror=deprecated") == 0) {
            /* F4: already parsed into g_werror_deprecated; remove from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--Werror=inline-c-narrow-params") == 0 ||
                   strcmp(argv[i], "-Werror=inline-c-narrow-params") == 0) {
            /* Phase C: already parsed into g_werror_inline_c_narrow_params; remove. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-panic") == 0) {
            /* Already parsed, remove from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--no-contracts") == 0) {
            /* Phase C2: already parsed into g_no_contracts; remove from argv. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--debug") == 0) {
            /* Debugger Phase 4: already parsed into g_emit_debug_lines; strip
             * so the per-command flag parsers (build/emit-c) don't reject it. */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--json-diagnostics") == 0) {
            use_json_diagnostics = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--json") == 0) {
            /* E14: structured JSON output — implies --json-diagnostics for check */
            use_json_output = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--explain") == 0 && i + 1 < argc) {
            explain_mode = true;
            explain_code = argv[i + 1];
            /* Remove --explain and code from argv */
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (strcmp(argv[i], "--lint-unsafe") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_warn_nested = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--lint-unsafe-max-lines=", 24) == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_max_lines = (uint32_t)atoi(argv[i] + 24);
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--require-unsafe-docs") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_require_safety = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--unsafe-stats") == 0) {
            g_lint_unsafe_enabled = true;
            g_unsafe_stats_enabled = true;
            /* Remove from argv for command parsing */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-inline-c-unsafe") == 0) {
            g_lint_inline_c_unsafe = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-kinds") == 0) {
            /* Phase HKT-P6: enable kind-annotation dump after kind-check */
            g_dump_kinds = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--strict-refine") == 0) {
            /* RT3: upgrade every undecided/refuted refinement obligation from
             * "keep the runtime check" to a hard compile error.  A
             * diagnostic-strictness knob, NOT an experiment -- it changes how
             * loudly the compiler reports, not what it compiles. */
            g_strict_refine = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--strict-effects") == 0) {
            /* ER1: enforce unannotated effectful functions as warnings */
            g_strict_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-write-frames") == 0) {
            /* G1: print the checked verdict for each declared `#writes` frame */
            g_dump_write_frames = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            /* ER6: print inferred effect row for each defn after inference */
            g_dump_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-coloring") == 0) {
            /* CPS1: print the whole-program may-capture coloring per defn */
            g_dump_cps_coloring = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps") == 0) {
            /* CPS2: print the ANF/CPS IR for each colored defn */
            g_dump_cps = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-mono-specs") == 0) {
            /* VBM1 (van-laarhoven-monomorphization-plan): print the by-value HKT
             * monomorphization spec registry after elaboration. */
            g_dump_mono_specs = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-mono") == 0) {
            /* G1 (cps-backend-generic-monomorph-classification-plan): report CPS-
             * subset admissibility of each colored-generic monomorph. */
            g_dump_cps_mono = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-sizes") == 0) {
            /* SZ8: print inferred size index per sized-GADT constructor */
            g_dump_sizes = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--lint-effects") == 0) {
            /* ER6: advisory warnings for unannotated effectful functions */
            g_lint_effects = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--backtrack-depth=", 18) == 0) {
            /* Phase B5: cap run-backtrack at N results (0 = unlimited) */
            g_backtrack_depth = (int64_t)atoll(argv[i] + 18);
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--backtrack-depth") == 0 && i + 1 < argc) {
            g_backtrack_depth = (int64_t)atoll(argv[i + 1]);
            for (int j = i; j < argc - 2; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        } else if (strcmp(argv[i], "--dump-clone-plan") == 0) {
            /* Phase B5: dump cloneable capture plan after CPS */
            g_dump_clone_plan = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--dump-cps-coloring") == 0) {
            /* CPS1: dump CPS coloring (colored/uncolored) after cps_transform */
            g_dump_cps_coloring = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--cps-path") == 0) {
            /* CPS3: emit CPS wrappers for colored functions */
            g_cps_path = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--emit-abi-trace") == 0) {
            /* Phase I: trace the resolved ABI path for each call site during emit */
            g_emit_abi_trace = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (is_known_deprecated_x_flag(argv[i])) {
            /* drop-x-flags-plan: every -X<name> is an accept-and-warn no-op
             * as of v0.24.0.  TUR-W0050 emitted; the underlying feature is
             * always on (see src/runtime/globals.c). */
            warn_deprecated_x_flag(argv[i]);
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--keep-contracts") == 0) {
            /* CT3: keep contract checks in release builds */
            g_keep_contracts_in_release = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strncmp(argv[i], "--enable=", 9) == 0) {
            /* XF1: opt in to one or more experimental features (comma list).
             * An unknown name is a hard TUR-E0310 error. */
            if (!enable_experiment_list(argv[i] + 9, XF_SRC_CLI)) return 2;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--allow-experimental") == 0) {
            /* UC-4 (user-config-experiments-plan): --allow-experimental was
             * retired.  Enabling an experiment (via --enable=<name>,
             * build.tur, or ~/.config/turmeric/experiments.tur) is now itself
             * the acknowledgment; the separate gate added nothing.  Reject it
             * with a targeted message for one release rather than silently
             * accepting or emitting a generic "unknown flag". */
            fprintf(stderr,
                    "error: --allow-experimental was retired in v" TUR_VERSION
                    "; enabling an experiment (via --enable=<name>, build.tur, "
                    "or ~/.config/turmeric/experiments.tur) is now the "
                    "acknowledgment. Remove the flag.\n");
            return 2;
        }
    }
    
        /* Initialize diagnostics - use color unless --no-color or --json-diagnostics specified */
        /* JSON output disables color */
        if (use_json_output) use_json_diagnostics = true;
        diag_init(!no_color && !use_json_diagnostics && stderr_is_tty());
        diag_set_json_output(use_json_diagnostics);
    
    if (explain_mode) {
        if (!explain_code) {
            fprintf(stderr, "tur: --explain requires a code snippet argument\n");
            return usage();
        }
        return cmd_explain(explain_code);
    }
    
    /* E2: --version / -V / -v */
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0
                   || strcmp(argv[1], "-V") == 0
                   || strcmp(argv[1], "-v") == 0)) {
        printf("tur: the Turmeric compiler (v" TUR_VERSION ")\n");
        printf("Run `tur --help` for more commands.\n");
        return 0;
    }

    /* E1: --help / -h at top level */
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }

    if (argc < 2) return usage();
    const char *cmd = argv[1];

    {
        const char *resolved = resolve_command(cmd);
        if (resolved == COMMAND_AMBIGUOUS) return usage();
        if (resolved != NULL) cmd = resolved;
    }

    if (strcmp(cmd, "emit-c") == 0) {
        /* SC2: collect -I flags up front so both emit-c forms see them. */
        char **emit_inc = NULL;
        int    n_emit_inc = parse_include_flags(argc, argv, 2, &emit_inc);
        if (n_emit_inc < 0) { free(emit_inc); return usage_build(); }

        /* tur emit-c [-I <dir>...] [--output-dir <dir> | --build-dir <dir> | -B <dir>]
         *            <file1> [<file2> ...]
         * build-output-directory-plan: --build-dir / -B are the new spellings;
         * --output-dir stays as a deprecated alias. */
        int od_idx = -1;
        for (int i = 2; i < argc; i++) {
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--output-dir") == 0 ||
                strcmp(argv[i], "--build-dir")  == 0 ||
                strcmp(argv[i], "-B")            == 0) { od_idx = i; break; }
        }
        if (od_idx >= 0) {
            if (od_idx + 1 >= argc) { free(emit_inc); return usage_build(); }
            const char *out_dir = argv[od_idx + 1];
            /* Inputs are every non-flag arg that isn't --output-dir or its
             * value or a -I value.  Build a clean inputs list. */
            char **inputs = (char **)malloc((size_t)argc * sizeof(char *));
            int    n_inputs = 0;
            for (int i = 2; i < argc; i++) {
                int c;
                if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
                if (i == od_idx) { i++; continue; }   /* skip --output-dir and its value */
                if (strcmp(argv[i], "--no-abi-cache") == 0) continue; /* J6: global, skip */
                if (argv[i][0] == '-') { free(inputs); free(emit_inc); return usage_build(); }
                inputs[n_inputs++] = argv[i];
            }
            /* SC4+SC5: auto-discover spice src + cross-spice deps for the
             * FIRST input only.  For the multi-file --output-dir form,
             * that input typically lives inside the same spice as the
             * others; if it doesn't, an explicit -I is the right escape
             * hatch. */
            char **md_owned = NULL;
            int    n_md_owned = 0;
            Ls2ResolverCtx md_ls2 = {0};
            if (n_inputs > 0) {
                auto_append_spice_includes(inputs[0], &emit_inc, &n_emit_inc,
                                           &md_owned, &n_md_owned, &md_ls2);
            }
            ls2_resolver_ctx_set(&md_ls2);
            int rc = cmd_emit_c_to_dir(out_dir, inputs, n_inputs,
                                       (const char **)emit_inc, n_emit_inc);
            ls2_resolver_ctx_set(NULL);
            ls2_resolver_ctx_dispose(&md_ls2);
            for (int i = 0; i < n_md_owned; i++) free(md_owned[i]);
            free(md_owned);
            free(inputs);
            free(emit_inc);
            return rc;
        }

        /* tur emit-c [-I <dir>...] <file>   single file to stdout (legacy) */
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (argv[i][0] != '-') {
                if (input) { free(emit_inc); return usage_build(); }
                input = argv[i];
                continue;
            }
            free(emit_inc); return usage_build();
        }
        if (!input) { free(emit_inc); return usage_build(); }
        char **ec_owned = NULL; int n_ec_owned = 0;
        Ls2ResolverCtx ec_ls2 = {0};
        auto_append_spice_includes(input, &emit_inc, &n_emit_inc,
                                   &ec_owned, &n_ec_owned, &ec_ls2);
        ls2_resolver_ctx_set(&ec_ls2);
        int rc = cmd_emit_c(input, (const char **)emit_inc, n_emit_inc);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&ec_ls2);
        for (int i = 0; i < n_ec_owned; i++) free(ec_owned[i]);
        free(ec_owned);
        free(emit_inc);
        return rc;
    }
    if (strcmp(cmd, "emit-h") == 0) {
        /* SC2: accept -I just like emit-c / check / build.
         * SC4+SC5: auto-discover enclosing spice src/ and dep src/. */
        char **eh_inc = NULL;
        int    n_eh_inc = parse_include_flags(argc, argv, 2, &eh_inc);
        if (n_eh_inc < 0) { free(eh_inc); return usage_build(); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (argv[i][0] != '-') {
                if (input) { free(eh_inc); return usage_build(); }
                input = argv[i];
                continue;
            }
            free(eh_inc); return usage_build();
        }
        if (!input) { free(eh_inc); return usage_build(); }
        char **eh_owned = NULL; int n_eh_owned = 0;
        Ls2ResolverCtx eh_ls2 = {0};
        auto_append_spice_includes(input, &eh_inc, &n_eh_inc,
                                   &eh_owned, &n_eh_owned, &eh_ls2);
        ls2_resolver_ctx_set(&eh_ls2);
        int rc = cmd_emit_h(input, (const char **)eh_inc, n_eh_inc);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&eh_ls2);
        for (int i = 0; i < n_eh_owned; i++) free(eh_owned[i]);
        free(eh_owned);
        free(eh_inc);
        return rc;
    }
    if (strcmp(cmd, "check") == 0) {
        /* Phase 8: tur check subcommand - type-check only, no codegen
         * SC1: now accepts -I <dir> flags so per-file checks inside a spice
         * resolve their intra-spice imports the same way `tur build` does.
         * SC4: walks up to find an enclosing build.tur and auto-adds its
         * `src/` so editors / format-on-save don't need explicit -I. */
        char       **check_inc = NULL;
        int          n_check_inc = parse_include_flags(argc, argv, 2, &check_inc);
        if (n_check_inc < 0) { free(check_inc); return usage_check(); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(check_inc); return usage_check();
            }
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(check_inc); return usage_check(); }
                input = argv[i];
                continue;
            }
            free(check_inc); return usage_check();
        }
        if (!input) { free(check_inc); return usage_check(); }
        /* A directory argument checks every .tur file under it (project /
         * spice mode), mirroring `tur test <dir>`. */
        if (is_directory(input)) {
            free(check_inc);
            return cmd_check_dir(input);
        }
        char **ck_owned = NULL; int n_ck_owned = 0;
        Ls2ResolverCtx ck_ls2 = {0};
        auto_append_spice_includes(input, &check_inc, &n_check_inc,
                                   &ck_owned, &n_ck_owned, &ck_ls2);
        ls2_resolver_ctx_set(&ck_ls2);
        int rc;
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        if (use_json_output) {
            diag_lsp_begin();
            Buf out;
            buf_init(&out);
            compile_to_c(input, &out, (const char **)check_inc, n_check_inc,
                         (const char **)rm_p, rm_n);
            buf_free(&out);
            diag_lsp_flush(stdout);
            diag_lsp_end();
            rc = diag_had_error() ? 1 : 0;
        } else {
            Buf out;
            buf_init(&out);
            rc = compile_to_c(input, &out, (const char **)check_inc, n_check_inc,
                              (const char **)rm_p, rm_n);
            buf_free(&out);
        }
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&ck_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_ck_owned; i++) free(ck_owned[i]);
        free(ck_owned);
        free(check_inc);
        return rc;
    }
    if (strcmp(cmd, "audit-spans") == 0) {
        /* debugger Phase 1: elaborate <file> and report breakpoint-eligible
         * AST nodes (top-level forms, defns, let forms, call sites) that lack
         * a usable source span.  Auto-discovers the enclosing spice src/ and
         * -I flags exactly like `tur check`, so intra-spice imports resolve.
         * Exit codes: 0 = clean, 3 = holes found, 1 = could not elaborate,
         * 2 = file error. */
        char       **as_inc = NULL;
        int          n_as_inc = parse_include_flags(argc, argv, 2, &as_inc);
        if (n_as_inc < 0) { free(as_inc); return usage_check(); }
        const char *input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(as_inc); return usage_check();
            }
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--no-auto-spice") == 0) continue;
            if (strcmp(argv[i], "--no-auto-stdlib") == 0) {
                g_no_auto_stdlib = true;
                continue;
            }
            if (argv[i][0] != '-') {
                if (input) { free(as_inc); return usage_check(); }
                input = argv[i];
                continue;
            }
            free(as_inc); return usage_check();
        }
        if (!input) { free(as_inc); return usage_check(); }
        char **as_owned = NULL; int n_as_owned = 0;
        Ls2ResolverCtx as_ls2 = {0};
        auto_append_spice_includes(input, &as_inc, &n_as_inc,
                                   &as_owned, &n_as_owned, &as_ls2);
        ls2_resolver_ctx_set(&as_ls2);
        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(input, &rm_n);
        Buf out;
        buf_init(&out);
        g_audit_spans = true;
        g_audit_span_holes = 0;
        int rc = compile_to_c(input, &out, (const char **)as_inc, n_as_inc,
                              (const char **)rm_p, rm_n);
        g_audit_spans = false;
        buf_free(&out);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&as_ls2);
        free_reader_macro_paths(rm_p, rm_n);
        for (int i = 0; i < n_as_owned; i++) free(as_owned[i]);
        free(as_owned);
        free(as_inc);
        return rc;
    }
    if (strcmp(cmd, "lsp") == 0) {
        diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
        lsp_server_run(STDIN_FILENO, STDOUT_FILENO);
        return 0;
    }
    if (strcmp(cmd, "mcp") == 0) {
        diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
        mcp_server_run(STDIN_FILENO, STDOUT_FILENO);
        return 0;
    }
    /* Debugger Phase 3: DAP server over the interpreter (JSON-RPC / stdio). */
    if (strcmp(cmd, "dap") == 0) {
        return cmd_dap();
    }
    /* lsp-lite: completion/calltip/doc backend for lightweight editors.
     * Newline-delimited JSON over stdio; stdout is reserved for protocol
     * traffic. */
    if (strcmp(cmd, "lsp-lite") == 0) {
        diag_init(false);
        resolve_stdlib_root();   /* sets TUR_STDLIB_DIR so lsp_lite can find docstrings.tur */
        return lsp_lite_run(STDIN_FILENO, STDOUT_FILENO);
    }
    if (strcmp(cmd, "build") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        const char *build_target = NULL;
        bool        shared = false;  /* RP0: --shared selects shared-library build */
        const char *manifest_out = NULL; /* RP1: --manifest <path> override */
        const char *cli_build_dir = NULL; /* build-output-directory-plan: --build-dir/-B */
        /* tur-link-and-build-split-plan Phase 3b/4: opt in to the compile+link
         * split for a single-file build.  Default stays the monolithic `cc`
         * call so output is byte-identical until the split is proven; the two
         * flags let a build force either path during rollout. */
        int split_build = 0;  /* 0 = default(monolithic), 1 = split, -1 = forced monolithic */
        /* SC1: collect -I flags once via the shared helper, then walk argv
         * a second time for the build-specific flags (`-o`, `--target`)
         * and the positional input. */
        char  **build_inc = NULL;
        int     n_build_inc = parse_include_flags(argc, argv, 2, &build_inc);
        if (n_build_inc < 0) { free(build_inc); return usage_build(); }
        for (int i = 2; i < argc; i++) {
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(build_inc); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--shared") == 0) {
                shared = true;
            } else if (strcmp(argv[i], "--split-build") == 0) {
                split_build = 1;
            } else if (strcmp(argv[i], "--no-split-build") == 0) {
                split_build = -1;
            } else if (strncmp(argv[i], "--runtime=", 10) == 0 ||
                       (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)) {
                const char *mode = argv[i][9] == '=' ? argv[i] + 10 : argv[++i];
                if (strcmp(mode, "lib") == 0) g_runtime_mode = TUR_RT_LIB;
                else if (strcmp(mode, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
                else if (strcmp(mode, "auto") == 0) g_runtime_mode = TUR_RT_AUTO;
                else {
                    fprintf(stderr, "tur build: unknown --runtime '%s' "
                            "(supported: auto, lib, source)\n", mode);
                    free(build_inc); return 1;
                }
            } else if (strcmp(argv[i], "--no-abi-cache") == 0) {
                /* J6: consumed globally by parse_no_abi_cache; no-op here. */
            } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
                manifest_out = argv[++i];
            } else if ((strcmp(argv[i], "--build-dir") == 0 ||
                        strcmp(argv[i], "-B") == 0) && i + 1 < argc) {
                cli_build_dir = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                build_target = argv[++i];
                if (strcmp(build_target, "wasm") != 0) {
                    fprintf(stderr, "tur build: unknown target '%s' (supported: wasm)\n", build_target);
                    free(build_inc); return 1;
                }
            } else if (argv[i][0] != '-') {
                if (input) { free(build_inc); return usage_build(); }
                input = argv[i];
            } else {
                free(build_inc); return usage_build();
            }
        }
        if (!input) { free(build_inc); return usage_build(); }
        /* RP0: --shared only supports directory input -- the single-file
         * build path emits a `static`-by-default function and a main(), so
         * dlsym wouldn't find anything useful. Directory mode runs through
         * compile_to_implementation in separate-compilation mode, which
         * gives exported defns extern linkage. */
        if (shared && !is_directory(input)) {
            fprintf(stderr, "tur build: --shared requires a directory argument "
                            "(single-file builds emit static symbols)\n");
            free(build_inc); return 1;
        }
        if (shared && build_target) {
            fprintf(stderr, "tur build: --shared and --target are mutually exclusive\n");
            free(build_inc); return 1;
        }
        if (manifest_out && !shared) {
            fprintf(stderr, "tur build: --manifest requires --shared\n");
            free(build_inc); return 1;
        }
        /* Check if input is a directory - use multi-file build */
        int rc;
        if (is_directory(input)) {
            /* Manifest-driven project build when the directory carries a
             * build.tur: read the manifest, descend into src/, and resolve
             * the include path (own src/ + each :spices dep). Otherwise fall
             * back to the bare-directory glob. */
            char proj_manifest[4096];
            snprintf(proj_manifest, sizeof(proj_manifest),
                     "%s/build.tur", input);
            struct stat mst;
            if (stat(proj_manifest, &mst) == 0 && S_ISREG(mst.st_mode)) {
                rc = cmd_build_project(input, out, shared, manifest_out,
                                       (const char **)build_inc, n_build_inc,
                                       cli_build_dir);
            } else {
                rc = cmd_build_multi(input, out, shared, manifest_out,
                                     cli_build_dir);
            }
        } else {
            /* RM4: auto-discover the spice manifest containing `input` so
             * `:reader-macros [...]` entries apply when building a single
             * file from inside a project. No-op when there is no manifest. */
            char *b_root = find_spice_root(input);
            char **b_rm  = NULL;
            int    b_n   = 0;
            if (b_root) {
                char mp[4096];
                (void)pkg_resolve_manifest_path(b_root, mp, sizeof(mp));
                PkgManifest bm; memset(&bm, 0, sizeof(bm));
                if (pkg_manifest_read(mp, &bm)) {
                    b_rm = resolve_manifest_reader_macros(b_root, &bm, &b_n);
                }
                pkg_manifest_free(&bm);
            }
            /* Mirror emit-c / emit-h / check / run: widen the include
             * path with the enclosing spice's src/ and every declared
             * `:spices` dep's src/. Short-circuits when --no-auto-spice
             * is set or there is no enclosing build.tur. */
            char **b_owned = NULL; int n_b_owned = 0;
            Ls2ResolverCtx b_ls2 = {0};
            auto_append_spice_includes(input, &build_inc, &n_build_inc,
                                       &b_owned, &n_b_owned, &b_ls2);
            ls2_resolver_ctx_set(&b_ls2);
            if (split_build == 1 && !build_target) {
                /* tur-link-and-build-split-plan Phase 3b: `tur build` defined as
                 * `tur compile` + `tur link` composed -- the exact subcommand
                 * code paths, so the three can never drift.  The generated
                 * object + `.link` sidecar land under the build temp dir. */
                char base[512];
                default_output_name(input, base, sizeof(base));
                char objp[1200];
                snprintf(objp, sizeof(objp), "%s%s.o", stable_c_prefix(), base);
                rc = cmd_compile(input, objp, (const char **)build_inc,
                                 n_build_inc, (const char **)b_rm, b_n);
                if (rc == 0) {
                    const char *outp = out;
                    char outbuf[1024];
                    if (!outp) {
                        default_output_name(input, outbuf, sizeof(outbuf));
                        outp = outbuf;
                    }
                    const char *lk[1] = { objp };
                    rc = cmd_link(outp, lk, 1, false, NULL);
                }
                /* Drop the intermediate .o/.c/.link (ccache keyed on content
                 * already caches the underlying object compile). */
                char scrap[1300];
                unlink(objp);
                obj_sibling_path(objp, ".c",    scrap, sizeof(scrap)); unlink(scrap);
                obj_sibling_path(objp, ".link", scrap, sizeof(scrap)); unlink(scrap);
            } else {
                rc = cmd_build(input, out, (const char **)build_inc, n_build_inc,
                               build_target, (const char **)b_rm, b_n);
            }
            ls2_resolver_ctx_set(NULL);
            ls2_resolver_ctx_dispose(&b_ls2);
            for (int i = 0; i < n_b_owned; i++) free(b_owned[i]);
            free(b_owned);
            free_reader_macro_paths(b_rm, b_n);
            free(b_root);
        }
        free(build_inc);
        return rc;
    }
    /* tur-link-and-build-split-plan Phase 3a-0: tur compile <in.tur> -o <out.o> */
    if (strcmp(cmd, "compile") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        const char *cli_build_dir = NULL;
        char  **comp_inc = NULL;
        int     n_comp_inc = parse_include_flags(argc, argv, 2, &comp_inc);
        if (n_comp_inc < 0) { free(comp_inc); return usage_build(); }
        for (int i = 2; i < argc; i++) {
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(comp_inc); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if ((strcmp(argv[i], "--build-dir") == 0 ||
                        strcmp(argv[i], "-B") == 0) && i + 1 < argc) {
                cli_build_dir = argv[++i];
            } else if (strncmp(argv[i], "--runtime=", 10) == 0 ||
                       (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc)) {
                const char *mode = argv[i][9] == '=' ? argv[i] + 10 : argv[++i];
                if (strcmp(mode, "lib") == 0) g_runtime_mode = TUR_RT_LIB;
                else if (strcmp(mode, "source") == 0) g_runtime_mode = TUR_RT_SOURCE;
                else if (strcmp(mode, "auto") == 0) g_runtime_mode = TUR_RT_AUTO;
                else {
                    fprintf(stderr, "tur compile: unknown --runtime '%s' "
                            "(supported: auto, lib, source)\n", mode);
                    free(comp_inc); return 1;
                }
            } else if (strcmp(argv[i], "--no-abi-cache") == 0) {
                /* global, consumed elsewhere */
            } else if (argv[i][0] != '-') {
                if (input) { free(comp_inc); return usage_build(); }
                input = argv[i];
            } else {
                free(comp_inc); return usage_build();
            }
        }
        if (!input) { free(comp_inc); return usage_build(); }
        if (is_directory(input)) {
            fprintf(stderr, "tur compile: expects a single .tur file, not a directory\n");
            free(comp_inc); return 1;
        }
        /* Default the object under <build-dir>/obj/ when --build-dir/-B (or a
         * manifest :build-dir) applies and no explicit -o was given. */
        char obj_default[1200];
        if (!out) {
            char *bd = resolve_build_dir(input, cli_build_dir);
            if (bd) {
                char base[512];
                default_output_name(input, base, sizeof(base));
                snprintf(obj_default, sizeof(obj_default), "%s/obj/%s.o", bd, base);
                out = obj_default;
                free(bd);
            }
        }
        /* Reader macros + spice include auto-discovery, mirroring cmd_build. */
        char *c_root = find_spice_root(input);
        char **c_rm = NULL; int c_n = 0;
        if (c_root) {
            char mp[4096];
            (void)pkg_resolve_manifest_path(c_root, mp, sizeof(mp));
            PkgManifest cm; memset(&cm, 0, sizeof(cm));
            if (pkg_manifest_read(mp, &cm))
                c_rm = resolve_manifest_reader_macros(c_root, &cm, &c_n);
            pkg_manifest_free(&cm);
        }
        char **c_owned = NULL; int n_c_owned = 0;
        Ls2ResolverCtx c_ls2 = {0};
        auto_append_spice_includes(input, &comp_inc, &n_comp_inc,
                                   &c_owned, &n_c_owned, &c_ls2);
        ls2_resolver_ctx_set(&c_ls2);
        int rc = cmd_compile(input, out, (const char **)comp_inc, n_comp_inc,
                             (const char **)c_rm, c_n);
        ls2_resolver_ctx_set(NULL);
        ls2_resolver_ctx_dispose(&c_ls2);
        for (int i = 0; i < n_c_owned; i++) free(c_owned[i]);
        free(c_owned);
        free_reader_macro_paths(c_rm, c_n);
        free(c_root);
        free(comp_inc);
        return rc;
    }
    /* tur-link-and-build-split-plan Phase 3a: tur link <obj/src>... -o <out> */
    if (strcmp(cmd, "link") == 0) {
        const char *out = NULL;
        bool shared = false;
        const char *link_flags = NULL;
        const char **inputs = (const char **)malloc((size_t)argc * sizeof(char *));
        int n_inputs = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(inputs); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--shared") == 0) {
                shared = true;
            } else if (strcmp(argv[i], "--link-flags") == 0 && i + 1 < argc) {
                link_flags = argv[++i];
            } else if (argv[i][0] != '-') {
                inputs[n_inputs++] = argv[i];
            } else {
                fprintf(stderr, "tur link: unknown option '%s'\n", argv[i]);
                free(inputs); return usage_build();
            }
        }
        if (n_inputs == 0) { free(inputs); return usage_build(); }
        int rc = cmd_link(out, inputs, n_inputs, shared, link_flags);
        free(inputs);
        return rc;
    }
    if (strcmp(cmd, "jit") == 0) {
        return cmd_jit(argc, argv);
    }
    if (strcmp(cmd, "emit-rt-split") == 0) {
        return cmd_emit_rt_split(argc, argv);
    }
    if (strcmp(cmd, "run") == 0) {
        /* Disambiguate: if the first non-flag argument ends in .tur or
         * .tur.sweet, use the classic compile-and-run path; if --release /
         * -I flags appear (compile-only flags), also use classic path.
         * Otherwise dispatch to the Justfile task runner (RN0-RN7). */
        bool use_classic = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--") == 0) break;
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                /* Default help to the task runner; it documents both modes. */
                return usage_justrun();
            }
            if (strcmp(argv[i], "--release") == 0 ||
                strcmp(argv[i], "--offline") == 0  ||
                strcmp(argv[i], "--engine") == 0   ||
                strncmp(argv[i], "--engine=", 9) == 0 ||
                strncmp(argv[i], "-I", 2) == 0) {
                /* --engine is a classic-path (compile/run) flag; without this
                 * arm its VALUE would be taken as a Justfile task name. */
                use_classic = true;
                break;
            }
            if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
                /* Check if it looks like a .tur file path */
                const char *a = argv[i];
                size_t an = strlen(a);
                if ((an > 4  && strcmp(a + an - 4,  ".tur")      == 0) ||
                    (an > 10 && strcmp(a + an - 10, ".tur.sweet") == 0) ||
                    strcmp(a, "-") == 0) {
                    use_classic = true;
                }
                break;
            }
        }
        if (use_classic)
            return cmd_run(argc, argv);
        /* engine-selection-plan E3: a BARE `tur run` (no recipe, no file) in
         * a Justfile-less build.tur project takes the classic project-run
         * path -- which resolves the manifest's `:engine` -- instead of the
         * historical hard 127.  A named recipe keeps the task-runner error:
         * a typo'd task silently compiling-and-running would be worse. */
        {
            bool bare = true;
            for (int i = 2; i < argc; i++) {
                if (strcmp(argv[i], "--") == 0) break;
                if (argv[i][0] != '-') { bare = false; break; }
                if (strcmp(argv[i], "--justfile") == 0 ||
                    strcmp(argv[i], "--chdir") == 0) i++;   /* skip value */
            }
            if (bare && !justrun_finds_justfile()) {
                char _cwd[4096];
                if (getcwd(_cwd, sizeof(_cwd))) {
                    char *_root = find_project_root(_cwd);
                    if (_root) {
                        free(_root);
                        return cmd_run(argc, argv);
                    }
                }
            }
        }
        return cmd_justrun(argc, argv);
    }
    if (strcmp(cmd, "repl") == 0) {
        /* Phase S0: interactive REPL.
         * RP6: --watch enables auto-reload between prompts when a
         * spice source file changes mtime. No background thread --
         * the freshness check runs synchronously each turn. */
        bool watch_mode = false;
        const char *repl_engine_flag = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                return usage_repl();
            }
            if (strcmp(argv[i], "--watch") == 0) {
                watch_mode = true;
                continue;
            }
            if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
                repl_engine_flag = argv[++i];
                continue;
            }
            if (strncmp(argv[i], "--engine=", 9) == 0) {
                repl_engine_flag = argv[i] + 9;
                continue;
            }
            fprintf(stderr, "tur repl: unknown option '%s'\n", argv[i]);
            return usage_repl();
        }
        /* J2 (jit-engine-plan 3.3): spice auto-discovery can build in process
         * through the MIR engine instead of the `tur build --shared`
         * subprocess + dlopen.  This used to hang off `--enable=jit`; that
         * experiment graduated 2026-08-17, so it hangs off ENGINE SELECTION
         * now -- `--engine jit`, `TUR_ENGINE=jit`, or `:engine "jit"` in the
         * enclosing build.tur, the same ladder `tur run` resolves.
         *
         * Graduating the experiment deliberately did NOT make this the
         * default: unset, the engine resolves to "cc" and the subprocess path
         * is byte-for-byte what it was. */
        {
            const char *eng = resolve_engine(".", repl_engine_flag);
            if (!eng) return 2;    /* TUR-E0311 already printed */
            if (strcmp(eng, "jit") == 0) {
#ifdef TUR_HAVE_JIT
                tur_spice_set_jit_hook(&g_repl_jit_hook);
#else
                fprintf(stderr,
                        "tur repl: engine \"jit\" is configured, but this "
                        "build carries no JIT engine\n"
                        "     reconfigure with -DTUR_JIT=ON (vendors MIR at "
                        "configure time -- see cmake/mir.cmake),\n"
                        "     or override the engine: --engine cc / "
                        "TUR_ENGINE=cc\n");
                return 2;
#endif
            }
        }
        return cmd_repl(watch_mode);
    }
    /* Tier 3: persistent fixture worker for the test suite. */
    if (strcmp(cmd, "worker") == 0) {
        return cmd_worker();
    }
    if (strcmp(cmd, "interpret") == 0 || strcmp(cmd, "--interpret") == 0) {
        if (argc < 3) {
            fprintf(stderr, "tur: %s requires a file argument\n", cmd);
            return usage();
        }
        return cmd_eval(argv[2], !no_color && stderr_is_tty(), argv + 3, argc - 3,
                        /*debug=*/false);
    }
    /* Debugger Phase 2: `tur debug <file.tur> [args...]` -- run a file through
     * the tree-walking interpreter under the interactive debugger.  Drops into
     * a command REPL at program entry; commands are read from stdin (so a
     * script can drive it).  See docs/archive/history/debugger-plan.md (Phase 2). */
    if (strcmp(cmd, "debug") == 0) {
        if (argc < 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr,
                "usage:\n  tur debug <file.tur> [args...]\n"
                "\nDrops into an interactive debugger (break/step/next/finish/\n"
                "backtrace/locals/print/list/continue). Type 'help' at the\n"
                "(tur-dbg) prompt for the full command list.\n");
            return argc < 3 ? 1 : 0;
        }
        return cmd_eval(argv[2], !no_color && stderr_is_tty(), argv + 3, argc - 3,
                        /*debug=*/true);
    }
    /* E3: tur eval '<expr>' or tur eval --file <file> */
    if (strcmp(cmd, "eval") == 0) {
        if (argc < 3) return usage_eval();
        bool is_file = false;
        const char *src = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_eval();
            if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
                is_file = true;
                src = argv[++i];
            } else if (argv[i][0] != '-') {
                src = argv[i];
            }
        }
        if (!src) return usage_eval();
        bool use_color = !no_color && stderr_is_tty();
        if (is_file)
            return cmd_eval(src, use_color, NULL, 0, /*debug=*/false);
        return cmd_eval_expr(src, use_color);
    }
    /* E5: tur doc <symbol> */
    if (strcmp(cmd, "doc") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_doc();
        if (argc != 3) return usage_doc();
        return cmd_doc_cli(argv[2]);
    }
    /* AI6: tur image-info <image> -- print header without resuming. */
    if (strcmp(cmd, "image-info") == 0) {
        if (argc != 3 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr, "usage:\n  tur image-info <image>\n");
            return argc == 3 ? 0 : 1;
        }
        return cmd_image_info(argv[2]);
    }
    /* AI6: tur image-verify <image> [binary] -- validate header (+ stamp). */
    if (strcmp(cmd, "image-verify") == 0) {
        if (argc < 3 || argc > 4 || strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0) {
            fprintf(stderr, "usage:\n  tur image-verify <image> [loader-binary]\n");
            return (argc >= 3 && strcmp(argv[2], "--help") != 0) ? 2 : 0;
        }
        return cmd_image_verify(argv[2], argc == 4 ? argv[3] : NULL);
    }
    /* E13: tur explain — first-class subcommand wrapping --explain */
    if (strcmp(cmd, "explain") == 0) {
        if (argc < 3 || (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0)))
            return usage_explain();
        return cmd_explain(argv[2]);
    }
    if (strcmp(cmd, "format") == 0) {
        bool check_only = false;
        bool diff_mode  = false;
        const char *fmt_input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_format();
            if (strcmp(argv[i], "--check") == 0) {
                check_only = true;
            } else if (strcmp(argv[i], "--diff") == 0) {
                diff_mode = true;
            } else if (argv[i][0] != '-') {
                if (fmt_input) return usage_format();
                fmt_input = argv[i];
            } else {
                return usage_format();
            }
        }
        if (check_only && diff_mode) return usage_format();
        return cmd_format(fmt_input, check_only, diff_mode);
    }
    if (strcmp(cmd, "fmt") == 0)
        return cmd_fmt(argc, argv);
    if (strcmp(cmd, "parse-check") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 ||
                          strcmp(argv[2], "-h") == 0))
            return usage_parse_check();
        if (argc != 4) return usage_parse_check();
        return cmd_parse_check(argv[2], argv[3]);
    }
    if (strcmp(cmd, "test") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_test();
        if (argc != 3) return usage_test();
        return cmd_test(argv[2]);
    }
    /* Phase PKG-1: Spice package manager commands */
    if (strcmp(cmd, "new") == 0)
        return cmd_pkg_new(argc, argv);
    if (strcmp(cmd, "init") == 0)
        return cmd_pkg_init(argc, argv);
    if (strcmp(cmd, "add") == 0)
        return cmd_pkg_add(argc, argv);
    if (strcmp(cmd, "add-cmake") == 0)
        return cmd_pkg_add_cmake(argc, argv);
    if (strcmp(cmd, "fetch") == 0)
        return cmd_pkg_fetch(argc, argv);
    if (strcmp(cmd, "emit-cmake") == 0)
        return cmd_pkg_emit_cmake(argc, argv);
    /* GS-M2: global spice install commands */
    if (strcmp(cmd, "install") == 0)
        return cmd_pkg_install(argc, argv);
    if (strcmp(cmd, "uninstall") == 0)
        return cmd_pkg_uninstall(argc, argv);
    /* GS-M3 / GS-M4 */
    if (strcmp(cmd, "list") == 0)
        return cmd_pkg_list(argc, argv);
    if (strcmp(cmd, "upgrade") == 0)
        return cmd_pkg_upgrade(argc, argv);
    /* XF3: experimental-feature registry listing */
    if (strcmp(cmd, "experiments") == 0)
        return cmd_experiments(argc, argv);
    /* L5: `#lang` layer registry listing */
    if (strcmp(cmd, "lang-layers") == 0)
        return cmd_lang_layers(argc, argv);
    /* Shell completion scripts (zsh/bash) */
    if (strcmp(cmd, "completion") == 0)
        return cmd_completion(argc, argv);

    /* GS-M2: subcommand fallthrough — `tur foo bar` execs `tur-foo bar`
     * from $PATH when "foo" isn't a built-in. Built-ins always win.
     * If exec succeeds, this does not return. */
    return try_external_subcommand(argc, argv);
}
