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
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>     /* SN1: _NSGetExecutablePath for stdlib resolution */
#endif

#include "arena.h"
#include "assert.h"
#include "buf.h"
#include "borrow_check.h"  /* Phase 14 */
#include "cps.h"          /* Phase 18: CPS transformation */
#include "diag.h"
#include "effect_check.h" /* Phase P19-2: effect-row inference */
#include "effect.h"       /* built-in effect registration */
#include "kind_check.h"   /* Phase HKT H0: kind inference pass */
#include "elab.h"
#include "emit.h"
#include "effect_lower.h" /* Phase 19: Effect lowering */
#include "expr.h"
#include "fmt.h"
#include "forms.h"
#include "pass.h"         /* Phase P19-1: pass scheduling */
#include "reader.h"
#include "reader_macros.h"
#include "symbols.h"
/* Phase S0: eval API for tur repl */
#include "turi/eval.h"
/* Phase S1: REPL with libedit, multi-line input, :type/:doc/:reload */
#include "turi/repl.h"
/* Phase PKG-1: Spice package manager */
#include "pkg.h"
/* RN0-RN7: Justfile-compatible task runner */
#include "justrun.h"
/* Global configuration variables — defined in globals.c */
#include "globals.h"
/* LSP server */
#include "lsp/lsp.h"
#include "lsp/lsp_sym.h"
#include "lsp/lsp_docs.h"

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

/* Helper to detect language and adjust source for #lang directive */
static ReaderType detect_and_adjust_lang(const char *path, char *src, size_t len,
                                        const char **out_src, size_t *out_len) {
    const char *src_rest = src;
    size_t len_rest = len;
    ReaderType detected_type = reader_type_from_extension(path);
    
    /* Only parse #lang if file extension didn't already select sweet */
    if (detected_type == READER_TURMERIC) {
        detected_type = detect_lang(src, len, &src_rest, &len_rest);
    }
    
    /* Check if the reader is implemented */
    if (!reader_type_is_implemented(detected_type)) {
        fprintf(stderr, "tur: error: #lang %s is not yet implemented\n", 
                reader_type_name(detected_type));
        exit(1);
    }
    
    *out_src = src_rest;
    *out_len = len_rest;
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

    const char *env = getenv("TUR_STDLIB_DIR");
    if (env && *env) {
        size_t n = strlen(env);
        if (n < sizeof(g_stdlib_root)) {
            memcpy(g_stdlib_root, env, n + 1);
            g_stdlib_root_state = 1;
            return g_stdlib_root;
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
            /* Phase 18: CPS transformation for shift/reset. */
            ctx->prog = cps_transform(ctx->arena, ctx->prog, &ctx->tc_env);
            if (!ctx->prog || diag_had_error()) return 1;
            /* Phase B5: --dump-clone-plan: print cloneable capture plan after CPS */
            if (g_dump_clone_plan) cps_dump_clone_plan(ctx->prog, stderr);
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

/* Global state for symbol collection -- set by tur_collect_symbols before
 * calling compile_to_c, cleared after.  Single-threaded LSP use only. */
static LspSymbol  *g_collect_syms_out   = NULL;
static int         g_collect_syms_cap   = 0;
static int        *g_collect_syms_count = NULL;

/* Walk one level of Expr items and record global bindings into the collector. */
static void collect_items(const Expr **items, uint32_t n);

static void collect_binding(const Binding *b) {
    if (!b || !b->name || !b->is_global) return;
    if (!g_collect_syms_out || !g_collect_syms_count) return;
    if (*g_collect_syms_count >= g_collect_syms_cap) return;
    LspSymbol *sym = &g_collect_syms_out[(*g_collect_syms_count)++];
    memset(sym, 0, sizeof(*sym));
    size_t nlen = strlen(b->name->name);
    if (nlen >= sizeof(sym->name)) nlen = sizeof(sym->name) - 1;
    memcpy(sym->name, b->name->name, nlen);
    const char *tn = type_name(b->type);
    if (tn) {
        size_t tlen = strlen(tn);
        if (tlen >= sizeof(sym->type_str)) tlen = sizeof(sym->type_str) - 1;
        memcpy(sym->type_str, tn, tlen);
    }
    sym->line      = (int)b->span.line;
    sym->col_start = (int)b->span.col_start;
    sym->col_end   = (int)b->span.col_end;
    const char *fp = diag_file_path(b->span.file_id);
    if (fp) {
        size_t flen = strlen(fp);
        if (flen >= sizeof(sym->file_path)) flen = sizeof(sym->file_path) - 1;
        memcpy(sym->file_path, fp, flen);
    }
}

static void collect_items(const Expr **items, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const Expr *item = items[i];
        if (!item) continue;
        switch (item->kind) {
            case EX_FN_DEF:
                collect_binding(item->as.fn_def_.fn ? item->as.fn_def_.fn->binding : NULL);
                break;
            case EX_DEF:
                collect_binding(item->as.def_.binding);
                break;
            case EX_DEFDATA:
                collect_binding(item->as.defdata_.binding);
                break;
            case EX_DEFGADT:
                collect_binding(item->as.defgadt_.binding);
                break;
            case EX_DEFMODULE:
                if (item->as.defmodule_.mod)
                    collect_items((const Expr **)item->as.defmodule_.mod->body,
                                  item->as.defmodule_.mod->n_body);
                break;
            default:
                break;
        }
    }
}

static void collect_symbols_from_prog(const Expr *prog) {
    if (!prog || prog->kind != EX_PROGRAM) return;
    collect_items((const Expr **)prog->as.program.items, prog->as.program.n);
}

int tur_collect_symbols(const char *path, LspSymbol *out, int cap,
                        int *count_out) {
    *count_out = 0;
    g_collect_syms_out   = out;
    g_collect_syms_cap   = cap;
    g_collect_syms_count = count_out;
    Buf discard;
    buf_init(&discard);
    int rm_n = 0;
    char **rm_p = discover_manifest_reader_macros(path, &rm_n);
    int rc = compile_to_c(path, &discard, NULL, 0,
                          (const char **)rm_p, rm_n);
    free_reader_macro_paths(rm_p, rm_n);
    buf_free(&discard);
    g_collect_syms_out   = NULL;
    g_collect_syms_cap   = 0;
    g_collect_syms_count = NULL;
    return rc;
}

/* Reads a .tur file and emits its C source into `out_c`. Returns 0 on success,
 * nonzero on error (diagnostics already emitted).
 * include_dirs/n_include_dirs: additional module search paths for (import ...).
 * reader_macro_paths/n_reader_macro_paths: RM4 — absolute paths to
 * `(reader-macros/define ...)` definition files that are preloaded into
 * the reader's macro registry before the entry file is parsed. Typically
 * derived from the project's `build.tur :reader-macros [...]` entry. */
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
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
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

    /* Phase 7: Load standard library files */
    /* For now, load them in a specific order to ensure dependencies are met */
    /* Note: option, result, slice, str, vec, test use inline C with malloc/free
     * which causes type mismatches when compiled into every file.
     * They're deferred until Phase 11 when :ptr<T> support is added.
     * For Phase 7, we load only macros.tur which contains when/unless macros. */
    /* Basenames only — resolved at use time via $TUR_STDLIB_DIR (else "stdlib"). */
    const char *stdlib_files[] = {
        "macros.tur",
        "safe.tur",
        /* args.tur is NOT auto-loaded to avoid injecting ~400 lines of args
         * parser stubs into every compiled program.  Load it explicitly with
         * (load "stdlib/args.tur") when args/spec-* functions are needed. */
        /* Phase C1: runtime contracts - auto-load contract.tur for assert!/require!/ensure!/invariant! */
        "contract.tur",
        /* Phase P3: HAMT lowering - auto-load hamt.tur. */
        "hamt.tur",
        /* "gen.tur" - GF2 generator stdlib; not auto-loaded to avoid polluting
         * all programs.  Load explicitly with (load "stdlib/gen.tur"). */
        /* "vec.tur" - has typeclass dependencies, not auto-loaded */
        /* Phase PTC4: typeclass-eq.tur defines only the Eq class skeleton so that
         * typed-collection definstances (Eq[Vec], Eq[Map], etc.) have Eq in scope.
         * The full typeclass.tur (with all primitive instances) remains on-demand. */
        "typeclass-eq.tur",
        /* Phase TS5: typeclass-functor.tur defines the Functor class stub so that
         * rc.tur and other typed-collection modules can declare
         * (definstance Functor [...]) without importing typeclass.tur. */
        "typeclass-functor.tur",
        /* Phase B1: typeclass-clone.tur defines the Clone class stub so that
         * ref.tur can declare (definstance Clone [...]) without importing
         * typeclass.tur. */
        "typeclass-clone.tur",
        /* Phase TM0/TC1/TC2/F5: typed parameterized collection stdlib files
         * (now under unprefixed module names). */
        "map.tur",
        "vec.tur",
        "slice.tur",
        "option.tur",
        "result.tur",
        "pair.tur",
        /* Phase TP1: N-ary tuple stdlib (Tuple2..Tuple5). */
        "tuple.tur",
        "list.tur",
        "grid.tur",
        "zipper.tur",
        "set.tur",
        /* Phase F5 (cross-plan-followups): mutable open-addressed hash table. */
        "mutmap.tur",
        /* Phase T19-C/D stdlib files (mutex, rwlock, condvar, sync, thread, chan,
         * atomic) are NOT auto-loaded here to avoid polluting every program's
         * generated C and invalidating codegen snapshots.  They are library files
         * usable via `tur build <dir>` when placed next to user code, matching the
         * pattern established by stdlib/atomic.tur.  An explicit `require` or
         * module mechanism (planned post-T21) will provide auto-loading later. */
        NULL
    };

    uint32_t total_stdlib_forms = 0;
    Form **all_stdlib_forms = NULL;
    uint8_t file_id = 1;

    /* Suffix-skip: when --no-auto-stdlib is set, pre-compute the index of the
     * input file in stdlib_files[].  The auto-load loop then skips that entry
     * and all subsequent entries.  Files before it still load so the input
     * file's transitive dependencies are available.  -1 means no skip. */
    int no_stdlib_skip_from = -1;
    if (g_no_auto_stdlib) {
        const char *input_base = basename_of(path);
        for (int j = 0; stdlib_files[j] != NULL; j++) {
            if (strcmp(input_base, stdlib_files[j]) == 0) {
                no_stdlib_skip_from = j;
                break;
            }
        }
    }

    for (int i = 0; stdlib_files[i] != NULL; i++) {
        /* Suffix-skip: skip this file and all subsequent auto-loads when the
         * input file IS one of the auto-loaded ones.  Earlier entries still
         * load so the input's transitive dependencies resolve. */
        if (no_stdlib_skip_from >= 0 && i >= no_stdlib_skip_from)
            continue;
        char path_buf[4096];
        tur_stdlib_path(stdlib_files[i], path_buf, sizeof(path_buf));
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        /* SN2: use the quiet reader so a missing stdlib file does not
         * spam stderr on every invocation.  If the stdlib root is
         * mis-configured the downstream compile will fail with an
         * actionable error (missing symbols / module not found). */
        if (read_entire_file_quiet(path_buf, &stdlib_src, &stdlib_len) == 0) {
            /* strdup the source so it lives in the arena and won't be freed prematurely */
            char *src_copy = (char *)arena_alloc(&arena, stdlib_len);
            memcpy(src_copy, stdlib_src, stdlib_len);

            /* Path also needs to live in the arena since SourceFile stores a pointer. */
            char *path_copy = (char *)arena_alloc(&arena, strlen(path_buf) + 1);
            memcpy(path_copy, path_buf, strlen(path_buf) + 1);

            /* Allocate a fresh SourceFile per stdlib file — each must have its
             * own stable arena address since diag and reader store pointers.  */
            SourceFile *stdlib_file = (SourceFile *)arena_alloc(&arena, sizeof(SourceFile));
            *stdlib_file = (SourceFile){0};
            stdlib_file->path = path_copy;
            stdlib_file->src = src_copy;
            stdlib_file->len = stdlib_len;
            stdlib_file->file_id = file_id++;
            stdlib_file->reader_type = READER_TURMERIC;  /* Stdlib is always plain s-exprs */
            diag_register_file(stdlib_file);

            uint32_t stdlib_nforms = 0;
            Form **stdlib_forms = read_all(&arena, &st, stdlib_file, &stdlib_nforms);

            if (stdlib_forms && stdlib_nforms > 0) {
                /* Append to all_stdlib_forms */
                Form **new_all = (Form **)arena_alloc(&arena, 
                    (total_stdlib_forms + stdlib_nforms) * sizeof(Form *));
                for (uint32_t j = 0; j < total_stdlib_forms; j++) {
                    new_all[j] = all_stdlib_forms[j];
                }
                for (uint32_t j = 0; j < stdlib_nforms; j++) {
                    new_all[total_stdlib_forms + j] = stdlib_forms[j];
                }
                all_stdlib_forms = new_all;
                total_stdlib_forms += stdlib_nforms;
            }
            free(stdlib_src);
        }
    }
    
    /* Prepend stdlib forms to user forms */
    if (all_stdlib_forms && total_stdlib_forms > 0) {
        Form **all_forms = (Form **)arena_alloc(&arena, 
            (nforms + total_stdlib_forms) * sizeof(Form *));
        for (uint32_t i = 0; i < total_stdlib_forms; i++) {
            all_forms[i] = all_stdlib_forms[i];
        }
        for (uint32_t i = 0; i < nforms; i++) {
            all_forms[total_stdlib_forms + i] = forms[i];
        }
        forms = all_forms;
        nforms += total_stdlib_forms;
    }

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
        if (g_collect_syms_out && ctx.prog)
            collect_symbols_from_prog(ctx.prog);
        if (rc == 0 && emit_program(out_c, ctx.prog) != 0) {
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
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
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
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    SourceFile file = {0};
    file.path = path;
    file.src = src_adj;
    file.len = len_adj;
    file.file_id = 0;
    file.reader_type = reader_type;
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

/* Run type-check only on `path`; discard generated C.
 * Used by the LSP server. Must be called with diag_lsp_begin active.
 *
 * SC6: applies the same SC4+SC5 auto-discovery that `tur check` uses,
 * so editor diagnostics in spice files don't show bogus "module not
 * found" errors for intra-spice and cross-spice imports.  --no-auto-spice
 * is honored if the user passed it at the top level. */
int tur_check_only(const char *path) {
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

    /* J5: Write .tur-abi-cache/index for emit-c --output-dir. */
    if (rc == 0) {
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

/* Choose an output executable name from the input path: foo.tur -> foo. */
static void default_output_name(const char *input, char *out, size_t cap) {
    const char *base = basename_of(input);
    size_t n = strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    /* Only strip extension if this looks like a file (has a dot that's not at the start) */
    if (n > 0 && out[n-1] != '/') {
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
 * Maps the input path to /tmp/tur-build/<sanitized>.c where non-alphanumeric
 * characters (except '-') are replaced with '_'.
 * Creates /tmp/tur-build/ on first call. */
static void stable_c_path(const char *input, char *out, size_t cap) {
    static int dir_made = 0;
    if (!dir_made) { mkdir("/tmp/tur-build", 0700); dir_made = 1; }
    const char *prefix = "/tmp/tur-build/";
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

static int cmd_build(const char *input, const char *out_path,
                     const char **include_dirs, int n_include_dirs,
                     const char *target,
                     const char **reader_macro_paths,
                     int n_reader_macro_paths) {
    Buf csrc;
    buf_init(&csrc);
    int rc = compile_to_c(input, &csrc, include_dirs, n_include_dirs,
                          reader_macro_paths, n_reader_macro_paths);
    if (rc != 0) { buf_free(&csrc); return rc; }

    /* Write generated C to a deterministic path so ccache can cache the result
     * across repeated builds of the same .tur file.  Fall back to a random
     * temp if the stable path cannot be constructed (e.g. path too long). */
    char tmpl[1024];
    stable_c_path(input, tmpl, sizeof(tmpl));
    FILE *tf = tmpl[0] ? fopen(tmpl, "wb") : NULL;
    if (!tf) {
        /* fallback: random temp */
        char fallback[] = "/tmp/tur-XXXXXX.c";
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
     * top of the generated C so stdlib modules can add file-level includes.
     * The marker format is: slash-star __tur_include__: LINE star-slash */
    {
        const char *inc_mark = "/* __tur_include__: ";
        size_t inc_mlen = strlen(inc_mark);
        const char *p = csrc.data;
        Buf hdr;
        buf_init(&hdr);
        while (p && (p = strstr(p, inc_mark)) != NULL) {
            p += inc_mlen;
            const char *end = strstr(p, " */");
            if (!end) break;
            buf_write(&hdr, p, (size_t)(end - p));
            buf_putc(&hdr, '\n');
            p = end + 3;
        }
        if (hdr.len > 0) {
            Buf new_csrc;
            buf_init(&new_csrc);
            buf_write(&new_csrc, hdr.data, hdr.len);
            buf_write(&new_csrc, csrc.data, csrc.len);
            buf_free(&csrc);
            csrc = new_csrc;
        }
        buf_free(&hdr);
    }

    if (!tf || fwrite(csrc.data, 1, csrc.len, tf) != csrc.len) {
        fprintf(stderr, "tur: write failed\n");
        if (tf) fclose(tf);
        buf_free(&csrc);
        return 2;
    }
    fclose(tf);

    /* Phase S2: scan generated C for __tur_autolink__ comments and collect
     * any linker flags they embed (e.g. -lturi from stdlib/turi/eval.tur).
     * The marker format is: slash-star __tur_autolink__: FLAGS star-slash */
    Buf autolink;
    buf_init(&autolink);
    {
        const char *marker = "/* __tur_autolink__: ";
        size_t mlen = strlen(marker);
        const char *p = csrc.data;
        while (p && (p = strstr(p, marker)) != NULL) {
            p += mlen;
            const char *end = strstr(p, " */");
            if (!end) break;
            if (autolink.len > 0) buf_putc(&autolink, ' ');
            buf_write(&autolink, p, (size_t)(end - p));
            p = end + 3;
        }
        if (autolink.len > 0) buf_putc(&autolink, '\0');
    }
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
        else
            cc_flags = "-O2 -std=c99 -Wall -fno-strict-aliasing";
    }

    /* Collect cmake dep flags from cmake/spice-deps-manifest.json if present */
    Buf cmake_flags;
    buf_init(&cmake_flags);
    {
        /* Walk up from the input file's directory to find project root */
        char input_dir[4096];
        strncpy(input_dir, input, sizeof(input_dir) - 1);
        input_dir[sizeof(input_dir) - 1] = '\0';
        char *slash = strrchr(input_dir, '/');
        if (slash) *slash = '\0';
        else strncpy(input_dir, ".", sizeof(input_dir));
        char *proj_root = find_project_root(input_dir);
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

    /* If the autolink flags include -lturi, locate the turmeric SDK and
     * prepend absolute -I/-L paths so the build succeeds regardless of the
     * working directory.  This is required for prefix-installed builds
     * (e.g. Homebrew) where `tur install` cannot anchor to the source tree
     * and the relative -Lbuild/src / -Isrc in __tur_autolink__ won't resolve.
     *
     * Resolution order:
     *   1. $TUR_SDK_ROOT (explicit override)
     *   2. Walk up from exe looking for share/turmeric/src/turi/eval.h
     *      (prefix/Homebrew installed layout written by the formula)
     *
     * For dev builds the cwd is already set to the turmeric root by
     * `tur install`, so the relative paths already work; the extra absolute
     * flags are harmless redundancy in that case. */
    if (autolink.len > 0 && strstr(autolink.data, "-lturi")) {
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
            /* Prepend SDK flags so absolute paths take priority over the relative
             * -Lbuild/src and -Isrc entries that follow in the autolink block. */
            Buf new_al;
            buf_init(&new_al);
            buf_write(&new_al, sdk_flags.data, sdk_flags.len);
            buf_free(&sdk_flags);
            buf_putc(&new_al, ' ');
            /* autolink has a '\0' terminator counted in len; copy content only. */
            if (autolink.len > 1)
                buf_write(&new_al, autolink.data, autolink.len - 1);
            buf_putc(&new_al, '\0');
            buf_free(&autolink);
            autolink = new_al;
        }
    }

    /* If the autolink flags include -lturi, check whether the installed
     * libturi.a was compiled with AddressSanitizer (common in debug builds).
     * When it was, propagate -fsanitize=address,undefined so the linker can
     * resolve the ASAN runtime symbols.  We detect ASAN by looking for the
     * __asan_init symbol via `nm`; if nm is unavailable we skip the check. */
    bool autolink_needs_asan = false;
    if (autolink.len > 0 && strstr(autolink.data, "-lturi")) {
        /* Walk -L flags in cc_flags and autolink to find libturi.a */
        /* Build a best-effort nm command: check build/src/libturi.a (dev layout)
         * and any -L<dir> paths specified in cc_flags. */
        char nm_cmd[512];
        /* Collect -L paths from cc_flags */
        const char *cf = cc_flags;
        while (cf && *cf) {
            const char *lf = strstr(cf, "-L");
            if (!lf) break;
            lf += 2;
            /* Extract the path (no space between -L and path in our cc_flags) */
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
                        autolink_needs_asan = true;
                        break;
                    }
                }
            }
            cf = lf_end;
        }
    }

    /* T2: anchor any turmeric-tree-relative autolink paths (e.g.
     * `src/runtime/hamt.c -Isrc/runtime`) at the located turmeric root so
     * the cc step resolves the runtime sources regardless of cwd.  Without
     * this, project-mode `tur run` from an arbitrary directory fails with
     * `src/runtime/hamt.c: No such file`.  The -lturi SDK block above
     * already prepends absolute -I/-L flags; rewriting the trailing relative
     * ones is harmless redundancy there and the fix for the hamt.c case. */
    if (autolink.len > 1) {
        char tur_root[4096];
        resolve_turmeric_root(tur_root, sizeof(tur_root));
        if (tur_root[0] && strcmp(tur_root, ".") != 0) {
            Buf rewritten;
            buf_init(&rewritten);
            rewrite_autolink_relative_paths(autolink.data, tur_root, &rewritten);
            buf_putc(&rewritten, '\0');
            buf_free(&autolink);
            autolink = rewritten;
        }
    }

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s -o %s %s", cc, cc_flags, out_path, tmpl);
    /* Append any __tur_autolink__ flags discovered in the generated C. */
    if (autolink.len > 0) buf_printf(&cmd, " %s", autolink.data);
    buf_free(&autolink);
    /* If libturi was ASAN-instrumented, add sanitizer flags to avoid linker errors. */
    if (autolink_needs_asan) buf_puts(&cmd, " -fsanitize=address,undefined");
    /* Append cmake dep flags (-I/-L/-l). */
    if (cmake_flags.len > 0) buf_puts(&cmd, cmake_flags.data);
    buf_free(&cmake_flags);
    /* Append spice include dirs (-I). */
    for (int _i = 0; _i < n_include_dirs; _i++) {
        if (include_dirs[_i] && include_dirs[_i][0])
            buf_printf(&cmd, " -I%s", include_dirs[_i]);
    }
    /* Ensure the command string is null-terminated before passing to system(). */
    buf_putc(&cmd, '\0');
    int sys_rc = system(cmd.data);
    buf_free(&cmd);
    /* Leave the stable temp file for ccache; only unlink random fallbacks. */
    if (tmpl[0] == '/' && strncmp(tmpl, "/tmp/tur-build/", 15) != 0) {
        unlink(tmpl);
    }

    if (sys_rc != 0) {
        fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }
    return 0;
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
        snprintf(candidate, sizeof(candidate), "%s/build.tur", dir);
        struct stat st;
        if (stat(candidate, &st) == 0) {
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
        int n = snprintf(candidate, sizeof(candidate), "%s/build.tur", dir);
        if (n > 0 && (size_t)n < sizeof(candidate)) {
            struct stat st;
            if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
                size_t dl = strlen(dir);
                char *res = (char *)malloc(dl + 1);
                if (res) memcpy(res, dir, dl + 1);
                return res;
            }
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
    }
    return NULL;
}

static char **discover_manifest_reader_macros(const char *input_path,
                                              int *n_out) {
    *n_out = 0;
    if (!input_path) return NULL;
    char *sroot = find_spice_root(input_path);
    if (!sroot) return NULL;
    char mp[4096];
    snprintf(mp, sizeof(mp), "%s/build.tur", sroot);
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    char **out = NULL;
    if (pkg_manifest_read(mp, &m)) {
        out = resolve_manifest_reader_macros(sroot, &m, n_out);
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
    int mn = snprintf(manifest_path, sizeof(manifest_path), "%s/build.tur", root);
    if (mn > 0 && (size_t)mn < sizeof(manifest_path)) {
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
                int wn = snprintf(ws_manifest, sizeof(ws_manifest),
                                  "%s/build.tur", anc);
                if (wn <= 0 || (size_t)wn >= sizeof(ws_manifest)) continue;

                struct stat wst;
                if (stat(ws_manifest, &wst) != 0 || !S_ISREG(wst.st_mode))
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
        if (ent->d_type != DT_REG) continue;
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
            if (strcmp(ent->d_name, "build.tur") == 0) continue; /* manifest */
            if (*n >= *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *files = (char **)realloc(*files, (size_t)*cap * sizeof(char *));
            }
            (*files)[(*n)++] = strdup(path);
        }
    }
    closedir(d);
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
            if (strcmp(basename_of(raw[i]), "build.tur") == 0) {
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
static void resolve_include_dirs_from_manifest(const char *root,
                                               const PkgManifest *m,
                                               bool include_own_src,
                                               const char ***out_dirs,
                                               int *out_n) {
    *out_dirs = NULL;
    *out_n    = 0;

    int cap = (include_own_src ? 1 : 0) + m->n_spices;
    if (cap < 1) cap = 1;
    const char **dirs = (const char **)malloc((size_t)cap * sizeof(char *));
    if (!dirs) return;
    int n = 0;

    if (include_own_src) {
        char own_src[4096];
        snprintf(own_src, sizeof(own_src), "%s/src", root);
        struct stat ss;
        if (stat(own_src, &ss) == 0 && S_ISDIR(ss.st_mode))
            dirs[n++] = strdup(own_src);
    }

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
                    dirs[n++] = strdup(sib_src);
                    fallback_added = true;
                    break;
                }
                char sib_dir[4096];
                snprintf(sib_dir, sizeof(sib_dir),
                         "%s/%s", ancestor, s->subdir);
                if (stat(sib_dir, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                    dirs[n++] = strdup(sib_dir);
                    fallback_added = true;
                    break;
                }
            }
        }
        if (fallback_added) continue;
        if (chosen) dirs[n++] = strdup(chosen);
    }

    *out_dirs = dirs;
    *out_n    = n;
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
    snprintf(manifest_path, sizeof(manifest_path), "%s/build.tur", proj_root);
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

static int cmd_run(int argc, char **argv) {
    /* tur run [-I <dir>...] [--release] [--offline] [<file>] [-- <args>...] */
    bool        release           = false;
    bool        offline           = false;
    const char *explicit_file     = NULL;
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
        char out_path[] = "/tmp/tur-run-XXXXXX";                         \
        int _fd = mkstemp(out_path);                                     \
        if (_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); free_reader_macro_paths(rm_paths_owned, n_rm_paths); for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned); ls2_resolver_ctx_dispose(&run_ls2); return 2; } \
        close(_fd);                                                      \
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
        if (_rc != 0) { unlink(out_path); free(spice_inc_dirs); free(user_inc); free_reader_macro_paths(rm_paths_owned, n_rm_paths); for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
        free(auto_run_owned); ls2_resolver_ctx_dispose(&run_ls2); return _rc; } \
        Buf _cmd; buf_init(&_cmd);                                       \
        buf_printf(&_cmd, "'%s'", out_path);                             \
        if (passthrough_start >= 0) {                                    \
            for (int _i = passthrough_start; _i < argc; _i++)           \
                buf_printf(&_cmd, " '%s'", argv[_i]);                   \
        }                                                                \
        buf_putc(&_cmd, '\0');                                           \
        int _sys = system(_cmd.data);                                    \
        buf_free(&_cmd);                                                  \
        unlink(out_path);                                                \
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
            char src_tmp[] = "/tmp/tur-stdin-XXXXXX.tur";
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
            char out_path[] = "/tmp/tur-run-XXXXXX";
            int out_fd = mkstemp(out_path);
            if (out_fd < 0) { unlink(src_tmp); fprintf(stderr, "tur: mkstemp failed\n"); free(user_inc); return 2; }
            close(out_fd);
            /* SC2: stdin mode never enters project setup, so spice_inc_dirs
             * is empty here; we can pass user_inc straight through. */
            int brc = cmd_build(src_tmp, out_path,
                                (const char **)user_inc, n_user_inc, NULL,
                                NULL, 0);
            unlink(src_tmp);
            if (brc != 0) { unlink(out_path); free(user_inc); return brc; }
            Buf run_cmd; buf_init(&run_cmd);
            buf_printf(&run_cmd, "'%s'", out_path);
            if (passthrough_start >= 0)
                for (int i = passthrough_start; i < argc; i++)
                    buf_printf(&run_cmd, " '%s'", argv[i]);
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
                snprintf(mp, sizeof(mp), "%s/build.tur", sroot);
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
    snprintf(manifest_path, sizeof(manifest_path), "%s/build.tur", root);
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
                if (!pkg_fetch_all(root, &m, &lock, false)) {
                    fprintf(stderr, "tur run: dependency fetch failed\n");
                    pkg_lock_free(&lock);
                    pkg_manifest_free(&m);
                    free(root);
                    return 1;
                }
                pkg_lock_write(lock_path, &lock);
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

    /* CMake dependency handling: generate and build if cmake-deps present. */
    if (m.n_cmake_deps > 0) {
        /* Only (re)build if cmake/CMakeLists.txt doesn't already exist,
         * or if --update was requested.  For tur run we do a best-effort
         * build without --update so repeated runs stay fast. */
        char cmake_lists[4096];
        snprintf(cmake_lists, sizeof(cmake_lists), "%s/cmake/CMakeLists.txt", root);
        struct stat _cmst;
        bool cmake_built = (stat(cmake_lists, &_cmst) == 0);
        if (!cmake_built) {
            if (!pkg_gen_cmake_deps(root, &m) ||
                !pkg_cmake_build(root, &m, &lock, NULL)) {
                fprintf(stderr, "tur run: cmake dependency build failed\n");
                pkg_lock_free(&lock);
                pkg_manifest_free(&m);
                free(spice_inc_dirs);
                free(root);
                return 1;
            }
            pkg_lock_write(lock_path, &lock);
        }
    }

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

    RUN_ENTRY(entry);
#undef RUN_ENTRY
}

static int cmd_test(const char *dir) {
    int n_files = 0;
    char **tur_files = collect_tur_files(dir, &n_files);
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
        char out_path[] = "/tmp/tur-test-XXXXXX";
        int fd = mkstemp(out_path);
        if (fd < 0) {
            fprintf(stderr, "tur: mkstemp failed for %s\n", tur_files[i]);
            failed_files[failed++] = tur_files[i];
            putchar('F');
            continue;
        }
        close(fd);

        int rm_n = 0;
        char **rm_p = discover_manifest_reader_macros(tur_files[i], &rm_n);
        int build_rc = cmd_build(tur_files[i], out_path,
                                  spice_inc_dirs, n_spice_inc_dirs, NULL,
                                  (const char **)rm_p, rm_n);
        free_reader_macro_paths(rm_p, rm_n);
        int run_rc = 1;
        if (build_rc == 0) {
            int status = system(out_path);
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
/* Mangle a module name for use as a C header/impl base name, matching the
 * compiler's sanitize_module_name / mangle_module_name: '/' -> "__",
 * '-' -> '_', other non-identifier chars -> '_'. */
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

static int cmd_build_multi_files(char **tur_files, int n_files,
                                 const char *dir, const char *src_root,
                                 const char **file_src_roots,
                                 const char *out_path,
                                 bool shared, const char *manifest_path,
                                 const char **inc, int n_inc) {
    char chosen_out[1024];
    if (!out_path) {
        if (shared) {
            char base[1024];
            default_output_name(dir, base, sizeof(base));
            snprintf(chosen_out, sizeof(chosen_out), "lib%s.so", base);
        } else {
            default_output_name(dir, chosen_out, sizeof(chosen_out));
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
        h_files[i] = (char *)malloc(mlen + 3);
        c_files[i] = (char *)malloc(mlen + 3);
        snprintf(h_files[i], mlen + 3, "%s.h", mangled);
        snprintf(c_files[i], mlen + 3, "%s.c", mangled);
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

    /* J5: Write .tur-abi-cache/index with ownership information. */
    {
        char cache_dir[4096];
        snprintf(cache_dir, sizeof(cache_dir), "%s/.tur-abi-cache", dir);
        struct stat st_cache;
        bool cache_dir_new = (stat(cache_dir, &st_cache) != 0);
        if (cache_dir_new) {
            mkdir(cache_dir, 0755);
            /* Append to .gitignore if present. */
            char gitignore[4096];
            snprintf(gitignore, sizeof(gitignore), "%s/.gitignore", dir);
            FILE *gi = fopen(gitignore, "a");
            if (gi) { fprintf(gi, ".tur-abi-cache/\n"); fclose(gi); }
        }
        char idx_tmp[4096];
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
            char idx_path[4096];
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
        if (buf_to_path(&main_c, "_main.c") != 0) {
            fprintf(stderr, "tur: failed to write _main.c\n");
            buf_free(&main_c);
            for (int j = 0; j < n_files; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&main_c);
    }

    /* Compile everything together with cc */
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    /* See the note on -fno-strict-aliasing above. */
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

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s", cc, cc_flags);
    /* RP0: shared-library link gets -fPIC -shared and skips _main.c.
     * Position-independent code is required on Linux/macOS for symbols
     * loaded via dlopen; -shared tells the driver to emit a .so/.dylib
     * instead of an executable. */
    if (shared) buf_puts(&cmd, " -fPIC -shared");
    buf_printf(&cmd, " -o %s", out_path);
    /* Add _main.c first (executable mode only) */
    if (!shared) buf_puts(&cmd, " _main.c");
    /* Add all .c files */
    for (int i = 0; i < n_files; i++) {
        buf_printf(&cmd, " %s", c_files[i]);
    }
    /* Append cmake dep flags (-I/-L/-l). */
    if (cmake_flags.len > 0) buf_puts(&cmd, cmake_flags.data);
    buf_free(&cmake_flags);
    /* Ensure null termination before passing to system(). */
    buf_putc(&cmd, '\0');
    int sys_rc = system(cmd.data);
    buf_free(&cmd);

    /* Clean up temp files */
    for (int i = 0; i < n_files; i++) { free(h_files[i]); free(c_files[i]); free(mod_names[i]); }
    free(h_files); free(c_files); free(mod_names);
    free_tur_files(tur_files, n_files);
    if (!shared) unlink("_main.c");

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
                           const char *manifest_path) {
    int n_files;
    char **tur_files = collect_tur_files(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    return cmd_build_multi_files(tur_files, n_files, dir, NULL, NULL, out_path,
                                 shared, manifest_path, NULL, 0);
}

/* Manifest-driven project build: `tur build <dir>` where <dir>/build.tur
 * exists.  Collects the project's module files (recursively under `src/`),
 * resolves the include search path from the manifest (own src/ + each
 * `:spices` dep's src/), merges in any user `-I` dirs (which take priority),
 * and builds the whole module set.  Mirrors the project resolution `tur run`
 * and the test runner already perform. */
static int cmd_build_project(const char *root, const char *out_path,
                             bool shared, const char *manifest_path,
                             const char **user_inc, int n_user_inc) {
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
        snprintf(mpath, sizeof(mpath), "%s/build.tur", root);
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

    if (!shared) {
        char mpath[4096];
        snprintf(mpath, sizeof(mpath), "%s/build.tur", root);
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

    int rc = cmd_build_multi_files(all_files, n_all, root, src_root, all_roots,
                                   out_path, shared, manifest_path, inc, n_inc);

    free(all_roots);
    for (int k = 0; k < n_dep_dirs; k++) free((char *)dep_dirs[k]);
    free(dep_dirs);
    free(inc);
    for (int i = 0; i < n_proj_inc; i++) free((char *)proj_inc[i]);
    free(proj_inc);
    return rc;
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
                char orig_tmp[] = "/tmp/tur-fmt-orig-XXXXXX";
                int orig_fd = mkstemp(orig_tmp);
                if (orig_fd >= 0) {
                    ssize_t _wr1 = write(orig_fd, src, len); (void)_wr1;
                    close(orig_fd);
                }
                char new_tmp[] = "/tmp/tur-fmt-new-XXXXXX";
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
    if (n >= 9 && strcmp(name + n - 9, ".tursweet") == 0) return true;
    return false;
}


/* Core: read src, parse, format, return formatted Buf.
 * Returns 0 on success with *out populated, -1 on error. */
static int fmt_format_source(const char *path_label, const char *src, size_t len,
                              ReaderType rtype, Buf *out) {
    SourceFile file = {0};
    file.path        = path_label;
    file.src         = src;
    file.len         = len;
    file.file_id     = 0;
    file.reader_type = rtype;
    diag_register_file(&file);
    diag_reset();

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    ReaderMacroRegistry rmreg;
    reader_macros_init(&rmreg, &arena);
    rmreg.strict = true;

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &file, &rmreg, &nforms);

    int rc = 0;
    if (!forms || diag_had_error()) {
        rc = -1;
    } else {
        FmtOptions opts = {0};
        opts.indent_width = 2;
        opts.line_width   = 80;
        opts.src          = src;
        opts.src_len      = len;
        buf_init(out);
        if (fmt_print(out, forms, nforms, opts) != 0) {
            fprintf(stderr, "tur fmt: internal error formatting %s\n", path_label);
            buf_free(out);
            rc = -1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    return rc;
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
            char orig_tmp[] = "/tmp/tur-fmt-orig-XXXXXX";
            char new_tmp[]  = "/tmp/tur-fmt-new-XXXXXX";
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

/* Walk a directory, processing all .tur/.tursweet files.
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

        if (ent->d_type == DT_DIR) {
            changed += fmt_walk(child, force_lang, mode, err_count);
        } else if (ent->d_type == DT_REG && fmt_is_tur_file(ent->d_name)) {
            int r = fmt_process_file(child, force_lang, mode);
            if (r < 0) (*err_count)++;
            else if (r > 0) changed++;
        } else if (ent->d_type == DT_UNKNOWN) {
            struct stat cs;
            if (stat(child, &cs) == 0) {
                if (S_ISDIR(cs.st_mode))
                    changed += fmt_walk(child, force_lang, mode, err_count);
                else if (S_ISREG(cs.st_mode) && fmt_is_tur_file(ent->d_name)) {
                    int r = fmt_process_file(child, force_lang, mode);
                    if (r < 0) (*err_count)++;
                    else if (r > 0) changed++;
                }
            }
        }
    }
    closedir(d);
    return changed;
}

static int usage_fmt(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur fmt [paths...]                   format .tur/.tursweet files in place\n"
        "  tur fmt --check [paths...]           exit 1 if any file would change\n"
        "  tur fmt --dry-run [paths...]         alias for --check\n"
        "  tur fmt --diff [paths...]            print unified diff of changes\n"
        "  tur fmt --stdout <file>              format file and print to stdout\n"
        "  tur fmt --stdin [--lang <dialect>]   format stdin and print to stdout\n"
        "\n"
        "  Paths may be files or directories.  Defaults to current directory.\n"
        "  Skips:  build/  .git/  .tur-cache/  .turnb-cache/  .tur-repl-cache/\n"
        "\n"
        "  Dialects for --lang:  turmeric (default)  tursweet  curly-infix  neoteric\n"
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
            } else if (strcmp(lang, "tursweet") == 0 || strcmp(lang, "sweet-exp") == 0 || strcmp(lang, "sweet") == 0) {
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

static void wk_register_stdlib_natives(TuriEnv *env);

/* Phase S0: tur repl — interactive read-eval-print loop. */
/* Phase INT-1: run a .tur file through the tree-walking interpreter.
 * extra_argv/extra_argc are the arguments after the file path, exposed
 * to the script as *args* (a cons-cell list of C-string pointers). */
static int cmd_eval(const char *path, bool use_color,
                    char **extra_argv, int extra_argc) {
    g_interpret_mode = true;
    turi_init(use_color);
    TuriEnv *env = turi_env_new();
    if (!env) {
        fprintf(stderr, "tur: failed to create interpreter environment\n");
        return 1;
    }
    /* Preload macros.tur so that and/or/when/cond/for etc. are available.
     * This is the minimum stdlib needed for any real Turmeric program to work. */
    {
        char path_buf[4096];
        tur_stdlib_path("macros.tur", path_buf, sizeof(path_buf));
        TuriValue sv = turi_eval_file(env, path_buf);
        (void)sv;
    }
    /* Inject typed stubs so the elaborator knows the signatures of native
     * functions used by benchmark scripts.  The native shims registered below
     * replace these no-op closures at runtime. */
    {
        TuriValue sv = turi_eval(env,
            /* list operations */
            "(defn nil-value [] :int 0)\n"
            "(defn cons [v :int n :int] :int 0)\n"
            "(defn head [lst :int] :int 0)\n"
            "(defn tail [lst :int] :int 0)\n"
            /* vec operations */
            "(defn vec-new-filled [n :int v :int] :int 0)\n"
            "(defn vec-get [v :int i :int] :int 0)\n"
            "(defn vec-set! [v :int i :int x :int] :nil nil)\n"
            "(defn vec-free [v :int] :nil nil)\n"
            /* numeric helpers */
            "(defn cstr->parse-int [s :int] :int 0)\n"
            "(defn bit-shr [x :int n :int] :int 0)\n"
            "(defn bit-xor [x :int y :int] :int 0)\n"
            "(defn println-float [x :float d :int] :nil nil)\n"
            "(defn int->unit-float [x :int] :float 0.0)\n"
            "(defn tur-sqrt [x :float] :float 0.0)\n"
            "(defn int->float [x :int] :float 0.0)\n"
            /* HAMT operations for hash_map benchmark */
            "(defn hamt-new [] :int 0)\n"
            "(defn hamt-free [m :int] :nil nil)\n"
            "(defn hamt-set [m :int hash :int key :int val :int] :int 0)\n"
            "(defn hamt-get [m :int hash :int key :int] :int 0)\n"
            "(defn hamt-hash-ptr [p :int] :int 0)\n"
            /* I/O benchmark helpers (file_read.tur, file_write.tur) */
            "(defn write-temp-file [path :cstr n :int] :nil nil)\n"
            "(defn io-fopen-read [path :cstr] :int 0)\n"
            "(defn io-fread-chunk [fp :int buf :int] :int 0)\n"
            "(defn io-fclose [fp :int] :nil nil)\n"
            "(defn io-remove [path :cstr] :nil nil)\n"
            "(defn io-buf-new [] :int 0)\n"
            "(defn io-buf-free [buf :int] :nil nil)\n"
            "(defn io-alloc [n :int v :int] :int 0)\n"
            "(defn io-free [buf :int] :nil nil)\n"
            "(defn io-fopen-write [path :cstr] :int 0)\n"
            "(defn io-fwrite-chunk [fp :int buf :int offset :int chunk :int] :int 0)\n"
            /* Whole-benchmark natives (random_access, thread_ring, nbody, ray_tracing) */
            "(defn random-access-bench [size :int reads :int] :int 0)\n"
            "(defn run-ring [n :int m :int] :nil nil)\n"
            "(defn run-nbody [n :int steps :int] :nil nil)\n"
            "(defn run-raytracer [w :int h :int] :int 0)\n"
        );
        (void)sv;
    }
    /* Register native overrides for stdlib inline-C functions. */
    wk_register_stdlib_natives(env);
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
    TuriValue result = turi_eval_file(env, path);
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
            TuriValue r = turi_call(env, main_fn, NULL, 0);
            if (r.tag == TURI_ERROR) rc = 1;
            else if (r.tag == TURI_INT) rc = (int)r.as_int;
        }
        turi_run_pending_defers(env);
    }
    turi_env_free(env);
    return rc;
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

/* Apply flags string (e.g. "-Xgadt -Xlinear") to global compiler flags. */
static void wk_apply_flags(const char *flags_str) {
    if (!flags_str || !*flags_str) return;
    char copy[512];
    strncpy(copy, flags_str, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    char *tok = strtok(copy, " \t");
    while (tok) {
        if      (strcmp(tok, "-Xgadt")             == 0) g_gadt_enabled            = true;
        else if (strcmp(tok, "-Xlinear")            == 0) g_linear_enabled           = true;
        else if (strcmp(tok, "-Xunique-types")      == 0) g_unique_enabled           = true;
        else if (strcmp(tok, "-Xsubstructural")     == 0) { g_substructural_enabled = true; g_linear_enabled = true; }
        else if (strcmp(tok, "-Xunion-types")       == 0) g_union_types_enabled      = true;
        else if (strcmp(tok, "-Xintersection-types")== 0) g_intersection_types_enabled = true;
        else if (strcmp(tok, "-Xeffect-types")      == 0) { g_effect_types_enabled   = true; g_strict_effects = true; }
        else if (strcmp(tok, "-Xcontracts")         == 0) g_contracts_enabled        = true;
        else if (strcmp(tok, "-Xsessions")          == 0) { g_sessions_enabled = true; g_substructural_enabled = true; g_linear_enabled = true; }
        else if (strcmp(tok, "-Xdynamic-vars")      == 0) g_dynvar_enabled           = true;
        else if (strcmp(tok, "--unsafe-stats")      == 0) { g_lint_unsafe_enabled = true; g_unsafe_stats_enabled = true; }
        else if (strcmp(tok, "--strict-effects")    == 0) g_strict_effects           = true;
        else if (strcmp(tok, "--dump-effects")      == 0) g_dump_effects             = true;
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

/* -------------------------------------------------------------------------
 * Native HAMT wrappers for the interpreter (Tier 3).
 * These replace the nil stubs registered by EX_EXTERN_C so that HAMT
 * operations actually work during interpreter evaluation.
 * ---------------------------------------------------------------------- */
#include "runtime/hamt.h"

static TuriValue native_tur_hamt_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    Hamt *m = tur_hamt_new();
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)m; return v;
}
static TuriValue native_tur_hamt_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1) tur_hamt_free((Hamt *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_tur_hamt_retain(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    Hamt *r = tur_hamt_retain(m);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_count(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    uint32_t c = tur_hamt_count(m);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)c; return v;
}
static TuriValue native_tur_hamt_set(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_nil();
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (void *)(intptr_t)a[2].as_int;
    void *val  = (void *)(intptr_t)a[3].as_int;
    /* For cstr keys/values, use the actual pointer (they're interned). */
    if (a[2].tag == TURI_CSTR) key = (void *)a[2].as_cstr;
    if (a[3].tag == TURI_CSTR) val = (void *)a[3].as_cstr;
    Hamt *r = tur_hamt_set(m, h, key, val);
    /* If set returned the same HAMT (structural no-op), retain for the new binding. */
    if (r == m) tur_hamt_retain(r);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_del(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_nil();
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (n >= 3 && a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr
                                                     : (void *)(intptr_t)a[2].as_int;
    Hamt *r = tur_hamt_del(m, h, key);
    /* If del returned the same HAMT (key absent), the caller now holds two
     * bindings (original + new let) to the same object; retain so both
     * tur_hamt_free calls are safe. */
    if (r == m) tur_hamt_retain(r);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_has(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) { TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = false; return v; }
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr
                                          : (void *)(intptr_t)a[2].as_int;
    bool r = tur_hamt_has(m, h, key);
    TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = r; return v;
}
static TuriValue native_tur_hamt_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_nil();
    Hamt *m    = (Hamt *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key  = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr
                                          : (void *)(intptr_t)a[2].as_int;
    void *r = tur_hamt_get(m, h, key);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_merge(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    Hamt *ma = (Hamt *)(intptr_t)a[0].as_int;
    Hamt *mb = (Hamt *)(intptr_t)a[1].as_int;
    Hamt *r = tur_hamt_merge(ma, mb);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_tur_hamt_hash_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *s = (n >= 1 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : "";
    uint64_t h = tur_hamt_hash_str(s);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)h; return v;
}
static TuriValue native_tur_hamt_hash_ptr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    void *p = (n >= 1) ? ((a[0].tag == TURI_CSTR) ? (void *)a[0].as_cstr
                                                   : (void *)(intptr_t)a[0].as_int)
                       : NULL;
    uint64_t h = tur_hamt_hash_ptr(p);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)h; return v;
}
static TuriValue native_tur_hamt_iter_init(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    HamtIter *iter = (HamtIter *)(intptr_t)a[0].as_int;
    Hamt *m = (Hamt *)(intptr_t)a[1].as_int;
    if (iter) tur_hamt_iter_init(iter, m);
    return turi_nil();
}
static TuriValue native_tur_hamt_iter_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1) { HamtIter *iter = (HamtIter *)(intptr_t)a[0].as_int; if (iter) tur_hamt_iter_free(iter); }
    return turi_nil();
}
static TuriValue native_tur_hamt_iter_next(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) { TuriValue v = {0}; v.tag = TURI_BOOL; return v; }
    HamtIter *iter  = (HamtIter *)(intptr_t)a[0].as_int;
    uint64_t *hout  = (uint64_t *)(intptr_t)a[1].as_int;
    void    **kout  = (void **)(intptr_t)a[2].as_int;
    void    **vout  = (void **)(intptr_t)a[3].as_int;
    bool r = tur_hamt_iter_next(iter, hout, kout, vout);
    TuriValue v = {0}; v.tag = TURI_BOOL; v.as_bool = r; return v;
}

/* hamt/merge-with: merge two HAMTs with a Turmeric conflict-resolver closure.
 * Signature: (hamt/merge-with a b fn ctx) where fn is a Turmeric closure
 * called as (fn val_a val_b ctx) for duplicate keys. */
static TuriValue native_hamt_merge_with(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_nil();
    Hamt *ma  = (Hamt *)(intptr_t)a[0].as_int;
    Hamt *mb  = (Hamt *)(intptr_t)a[1].as_int;
    TuriValue fn  = a[2];
    TuriValue ctx = a[3];
    /* Start with a retain of ma as the result. */
    Hamt *result = tur_hamt_retain(ma);
    /* Iterate over b, merging into result. */
    HamtIter *iter = (HamtIter *)calloc(1, sizeof(HamtIter));
    if (!iter) { return turi_int((int64_t)(intptr_t)result); }
    tur_hamt_iter_init(iter, mb);
    uint64_t h; void *k; void *v;
    while (tur_hamt_iter_next(iter, &h, &k, &v)) {
        void *existing = tur_hamt_get(result, h, k);
        void *new_val;
        if (existing) {
            /* Call the Turmeric closure: (fn existing v ctx) */
            TuriValue call_args[3];
            call_args[0].tag = TURI_INT; call_args[0].as_int = (int64_t)(intptr_t)existing;
            call_args[1].tag = TURI_INT; call_args[1].as_int = (int64_t)(intptr_t)v;
            call_args[2] = ctx;
            TuriValue rv = turi_call(env, fn, call_args, 3);
            new_val = (void *)(intptr_t)rv.as_int;
        } else {
            new_val = v;
        }
        Hamt *next = tur_hamt_set(result, h, k, new_val);
        if (next != result) tur_hamt_free(result);
        result = next;
    }
    tur_hamt_iter_free(iter);
    free(iter);
    TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)result; return rv;
}

static TuriValue native_tur_hamt_show(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    char *s = tur_hamt_show(m);
    TuriValue v = {0}; v.tag = TURI_CSTR; v.as_cstr = s; return v;
}
static TuriValue native_tur_hamt_transient(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    Hamt *m = (n >= 1) ? (Hamt *)(intptr_t)a[0].as_int : NULL;
    HamtTransient *t = tur_hamt_transient(m);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)t; return v;
}
static TuriValue native_tur_hamt_transient_set(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_nil();
    HamtTransient *t = (HamtTransient *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr : (void *)(intptr_t)a[2].as_int;
    void *val = (a[3].tag == TURI_CSTR) ? (void *)a[3].as_cstr : (void *)(intptr_t)a[3].as_int;
    if (t) tur_hamt_transient_set(t, h, key, val);
    return turi_nil();
}
static TuriValue native_tur_hamt_transient_del(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_nil();
    HamtTransient *t = (HamtTransient *)(intptr_t)a[0].as_int;
    uint64_t h = (uint64_t)a[1].as_int;
    void *key = (a[2].tag == TURI_CSTR) ? (void *)a[2].as_cstr : (void *)(intptr_t)a[2].as_int;
    if (t) tur_hamt_transient_del(t, h, key);
    return turi_nil();
}
static TuriValue native_tur_hamt_persistent(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    HamtTransient *t = (n >= 1) ? (HamtTransient *)(intptr_t)a[0].as_int : NULL;
    Hamt *m = tur_hamt_persistent(t);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)m; return v;
}

static void wk_register_hamt_natives(TuriEnv *env) {
    turi_env_register_native(env, "tur_hamt_new",       native_tur_hamt_new,       NULL);
    turi_env_register_native(env, "tur_hamt_free",      native_tur_hamt_free,      NULL);
    turi_env_register_native(env, "tur_hamt_retain",    native_tur_hamt_retain,    NULL);
    turi_env_register_native(env, "tur_hamt_count",     native_tur_hamt_count,     NULL);
    turi_env_register_native(env, "tur_hamt_set",       native_tur_hamt_set,       NULL);
    turi_env_register_native(env, "tur_hamt_del",       native_tur_hamt_del,       NULL);
    turi_env_register_native(env, "tur_hamt_has",       native_tur_hamt_has,       NULL);
    turi_env_register_native(env, "tur_hamt_get",       native_tur_hamt_get,       NULL);
    turi_env_register_native(env, "tur_hamt_merge",     native_tur_hamt_merge,     NULL);
    turi_env_register_native(env, "tur_hamt_hash_str",  native_tur_hamt_hash_str,  NULL);
    turi_env_register_native(env, "tur_hamt_hash_ptr",  native_tur_hamt_hash_ptr,  NULL);
    turi_env_register_native(env, "tur_hamt_iter_init", native_tur_hamt_iter_init, NULL);
    turi_env_register_native(env, "tur_hamt_iter_free", native_tur_hamt_iter_free, NULL);
    turi_env_register_native(env, "tur_hamt_iter_next", native_tur_hamt_iter_next, NULL);
    turi_env_register_native(env, "tur_hamt_show",      native_tur_hamt_show,      NULL);
    turi_env_register_native(env, "tur_hamt_merge_with", native_hamt_merge_with,    NULL);
    turi_env_register_native(env, "tur_hamt_transient", native_tur_hamt_transient, NULL);
    turi_env_register_native(env, "tur_hamt_transient_set", native_tur_hamt_transient_set, NULL);
    turi_env_register_native(env, "tur_hamt_transient_del", native_tur_hamt_transient_del, NULL);
    turi_env_register_native(env, "tur_hamt_persistent", native_tur_hamt_persistent, NULL);
}

/* -------------------------------------------------------------------------
 * Native implementations of common inline-C stdlib patterns.
 * Registered under their Turmeric function names; EX_FN_DEF preservation
 * keeps them when fixtures define the same function with inline-C body.
 *
 * Option struct layout: { bool is_some (offset 0); int64_t value (offset 8) }
 * Matching C struct { bool; int64_t } with 7-byte padding.
 * We represent this as int64_t[2]: [0]=is_some flag, [1]=value.
 * ---------------------------------------------------------------------- */

/* Option functions */
static TuriValue native_some(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *opt = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!opt) return turi_nil();
    opt[0] = 1; /* is_some = true */
    opt[1] = (n > 0) ? a[0].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)opt;
    return v;
}
static TuriValue native_none(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    return turi_int(0); /* NULL pointer */
}
static TuriValue native_some_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    int64_t *opt = (int64_t *)(intptr_t)a[0].as_int;
    return turi_bool(opt != NULL && opt[0] != 0);
}
static TuriValue native_option_unwrap(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *opt = (int64_t *)(intptr_t)a[0].as_int;
    if (!opt || opt[0] == 0) { fprintf(stderr, "unwrap called on none\n"); return turi_int(0); }
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = opt[1]; return v;
}
static TuriValue native_option_value(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    return native_option_unwrap(env, a, n, ud);
}
static TuriValue native_option_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0) { void *p = (void *)(intptr_t)a[0].as_int; if (p) free(p); }
    return turi_nil();
}
static TuriValue native_option_must(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) goto panic_none;
    int64_t *opt = (int64_t *)(intptr_t)a[0].as_int;
    if (!opt || opt[0] == 0) goto panic_none;
    { TuriValue v = {0}; v.tag = TURI_INT; v.as_int = opt[1]; return v; }
panic_none:
    fprintf(stderr, "panic at\npanic: option-must: called on none\n");
    fflush(stderr);
    (void)env;
    _exit(1);
}
static TuriValue native_option_expect(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    int64_t *opt = (n > 0) ? (int64_t *)(intptr_t)a[0].as_int : NULL;
    const char *msg = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "option-expect: called on none";
    if (!opt || opt[0] == 0) {
        fprintf(stderr, "panic at\npanic: %s\n", msg);
        fflush(stderr);
        (void)env;
        _exit(1);
    }
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = opt[1]; return v;
}
static TuriValue native_option_unwrap_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *opt = (int64_t *)(intptr_t)a[0].as_int;
    if (!opt || opt[0] == 0) { TuriValue v = {0}; v.tag = TURI_INT; v.as_int = a[1].as_int; return v; }
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = opt[1]; return v;
}

/* Result functions: { bool is_ok (offset 0); int64_t ok_val (offset 8); int64_t err_val (offset 16) }
 * Stored as int64_t[3]: [0]=is_ok, [1]=ok_val, [2]=err_val */
static TuriValue native_ok(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *r = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!r) return turi_nil();
    r[0] = 1; r[1] = (n > 0) ? a[0].as_int : 0; r[2] = 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *r = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!r) return turi_nil();
    r[0] = 0; r[1] = 0; r[2] = (n > 0) ? a[0].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_ok_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    return turi_bool(r != NULL && r[0] != 0);
}
static TuriValue native_err_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(true);
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    return turi_bool(!r || r[0] == 0);
}
static TuriValue native_ok_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r ? r[1] : 0; return v;
}
static TuriValue native_err_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r ? r[2] : 0; return v;
}
static TuriValue native_result_unwrap_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (r && r[0] != 0) { TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r[1]; return v; }
    return a[1];
}
/* ok-val-ptr: get ok_val as a pointer (void*) */
static TuriValue native_ok_val_ptr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r ? r[1] : 0; return v;
}
/* err-val-ptr: get err_val as a pointer (void*) */
static TuriValue native_err_val_ptr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r ? r[2] : 0; return v;
}
/* result-map: apply Turmeric fn to ok value, return new result */
static TuriValue native_result_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (!r || r[0] == 0) return a[0]; /* return err unchanged */
    TuriValue arg = {0}; arg.tag = TURI_INT; arg.as_int = r[1];
    TuriValue res = turi_call(env, a[1], &arg, 1);
    int64_t *out = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!out) return turi_nil();
    out[0] = 1; out[1] = res.as_int; out[2] = 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)out; return v;
}
/* result-map-err: apply fn to err value, return new result */
static TuriValue native_result_map_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (!r || r[0] != 0) return a[0]; /* return ok unchanged */
    TuriValue arg = {0}; arg.tag = TURI_INT; arg.as_int = r[2];
    TuriValue res = turi_call(env, a[1], &arg, 1);
    int64_t *out = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!out) return turi_nil();
    out[0] = 0; out[1] = 0; out[2] = res.as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)out; return v;
}
/* result-flat-map: apply fn to ok value (fn returns a result), flatMap */
static TuriValue native_result_flat_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (!r || r[0] == 0) return a[0]; /* return err unchanged */
    TuriValue arg = {0}; arg.tag = TURI_INT; arg.as_int = r[1];
    return turi_call(env, a[1], &arg, 1);
}
/* result-or: return self if ok, else return alt */
static TuriValue native_result_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (r && r[0] != 0) return a[0];
    return a[1];
}
/* result-or-else: return self if ok, else call f(err_val) */
static TuriValue native_result_or_else(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (r && r[0] != 0) return a[0];
    TuriValue arg = {0}; arg.tag = TURI_INT; arg.as_int = r ? r[2] : 0;
    return turi_call(env, a[1], &arg, 1);
}
/* Display helpers */
static TuriValue native_result_display(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("err");
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_CSTR;
    v.as_cstr = (r && r[0] != 0) ? "ok" : "err"; return v;
}
static TuriValue native_result_debug(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("Result::Err");
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_CSTR;
    v.as_cstr = (r && r[0] != 0) ? "Result::Ok" : "Result::Err"; return v;
}
static TuriValue native_result_error_message(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("result is err");
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_CSTR;
    v.as_cstr = (r && r[0] != 0) ? "result is ok" : "result is err"; return v;
}
/* result-collect: vec<result<T,E>> → result<vec<T>,E>
 * If all elements are ok, returns ok(new_vec_of_ok_vals).
 * If any element is err, returns the first err. */
static TuriValue native_result_collect(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    int64_t len = v[1];
    int64_t *data = (int64_t *)(intptr_t)v[0];
    /* Check for first err */
    for (int64_t i = 0; i < len; i++) {
        int64_t *rp = (int64_t *)(intptr_t)data[i];
        if (!rp || rp[0] == 0) {
            int64_t ev = rp ? rp[2] : 0;
            int64_t *out = (int64_t *)malloc(3 * sizeof(int64_t));
            if (!out) return turi_nil();
            out[0] = 0; out[1] = 0; out[2] = ev;
            TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)out; return rv;
        }
    }
    /* All ok: create new vec of ok_vals */
    int64_t *ov = (int64_t *)calloc(3, sizeof(int64_t));
    if (!ov) return turi_nil();
    int64_t *od = len > 0 ? (int64_t *)malloc((size_t)len * sizeof(int64_t)) : NULL;
    ov[0] = (int64_t)(intptr_t)od; ov[1] = len; ov[2] = len;
    for (int64_t i = 0; i < len; i++) {
        int64_t *rp = (int64_t *)(intptr_t)data[i];
        od[i] = rp[1]; /* ok_val */
    }
    int64_t *out = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!out) { free(od); free(ov); return turi_nil(); }
    out[0] = 1; out[1] = (int64_t)(intptr_t)ov; out[2] = 0;
    TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)out; return rv;
}
/* Pair layout: { void *ok_vec; void *err_vec; } = int64_t[2] */
static TuriValue native_result_partition(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    int64_t len = v[1];
    int64_t *data = (int64_t *)(intptr_t)v[0];
    /* Allocate ok and err vecs */
    int64_t *ov = (int64_t *)calloc(3, sizeof(int64_t));
    int64_t *ev = (int64_t *)calloc(3, sizeof(int64_t));
    if (!ov || !ev) { free(ov); free(ev); return turi_nil(); }
    for (int64_t i = 0; i < len; i++) {
        int64_t *rp = (int64_t *)(intptr_t)data[i];
        int64_t *dst = (rp && rp[0] != 0) ? ov : ev;
        int64_t val = (rp && rp[0] != 0) ? rp[1] : (rp ? rp[2] : 0);
        /* Push to dst vec */
        int64_t dlen = dst[1], dcap = dst[2];
        int64_t *ddata = (int64_t *)(intptr_t)dst[0];
        if (dlen >= dcap) {
            int64_t nc = dcap > 0 ? dcap * 2 : 4;
            int64_t *nd = (int64_t *)malloc((size_t)nc * sizeof(int64_t));
            if (!nd) continue;
            for (int64_t j = 0; j < dlen; j++) nd[j] = ddata[j];
            free(ddata); dst[0] = (int64_t)(intptr_t)nd; dst[2] = nc; ddata = nd;
        }
        ddata[dlen] = val; dst[1] = dlen + 1;
    }
    int64_t *pair = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!pair) { return turi_nil(); }
    pair[0] = (int64_t)(intptr_t)ov; pair[1] = (int64_t)(intptr_t)ev;
    TuriValue rv = {0}; rv.tag = TURI_INT; rv.as_int = (int64_t)(intptr_t)pair; return rv;
}
static TuriValue native_result_partition_ok(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *pair = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = pair ? pair[0] : 0; return v;
}
static TuriValue native_result_partition_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *pair = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = pair ? pair[1] : 0; return v;
}
static TuriValue native_result_unwrap(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (!r || r[0] == 0) { fprintf(stderr, "unwrap called on err\n"); return turi_int(0); }
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r[1]; return v;
}
static TuriValue native_result_unwrap_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r ? r[2] : 0; return v;
}
static TuriValue native_result_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0) { void *p = (void *)(intptr_t)a[0].as_int; if (p) free(p); }
    return turi_nil();
}
static TuriValue native_result_must(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) goto panic_err;
    int64_t *r = (int64_t *)(intptr_t)a[0].as_int;
    if (!r || r[0] == 0) goto panic_err;
    { TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r[1]; return v; }
panic_err:
    fprintf(stderr, "panic at\npanic: result-must: called on err\n");
    fflush(stderr);
    (void)env;
    _exit(1);
}
static TuriValue native_result_must_msg(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    int64_t *r = (n > 0) ? (int64_t *)(intptr_t)a[0].as_int : NULL;
    const char *msg = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "result-must-msg: called on err";
    if (!r || r[0] == 0) {
        fprintf(stderr, "panic at\npanic: %s\n", msg);
        fflush(stderr);
        (void)env;
        _exit(1);
    }
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = r[1]; return v;
}

/* int->str / show-int-as-cstr: format int64 as decimal string */
static TuriValue native_int_to_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    char *buf = (char *)malloc(32);
    if (!buf) return turi_nil();
    snprintf(buf, 32, "%lld", (long long)v);
    return turi_cstr(buf);
}

/* str->int: parse decimal string to int64 */
static TuriValue native_str_to_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].tag != TURI_CSTR || !a[0].as_cstr) return turi_int(0);
    return turi_int((int64_t)atoll(a[0].as_cstr));
}

/* strcmp: compare two cstr values */
static TuriValue native_strcmp_fn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s1 = (n > 0 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : "";
    const char *s2 = (n > 1 && a[1].tag == TURI_CSTR) ? a[1].as_cstr : "";
    return turi_int((int64_t)strcmp(s1, s2));
}

/* int-val: dereference an int64_t* pointer and return the stored int.
 * Some fixture tests call this after free (same pattern as compiled C).
 * Suppress ASAN to match compiled-mode behavior. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
__attribute__((no_sanitize("address")))
#  endif
#elif defined(__SANITIZE_ADDRESS__)
__attribute__((no_sanitize("address")))
#endif
static TuriValue native_int_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *p = (int64_t *)(intptr_t)a[0].as_int;
    if (!p) return turi_int(0);
    return turi_int(*p);
}
/* alloc-str: strdup a cstr, return heap-allocated copy as int64_t ptr */
static TuriValue native_alloc_str(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *s = (n > 0 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : "";
    char *p = strdup(s ? s : "");
    if (!p) return turi_nil();
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p; return v;
}
/* cstr-free: free a cstr or int pointer */
static TuriValue native_cstr_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    void *p = (a[0].tag == TURI_CSTR) ? (void *)a[0].as_cstr : (void *)(intptr_t)a[0].as_int;
    if (p) free(p);
    return turi_nil();
}
/* alloc-int: malloc an int64_t cell, store x, return pointer as int */
static TuriValue native_alloc_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p; return v;
}
/* ptr=: pointer equality (both stored as int64_t) */
static TuriValue native_ptr_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_bool(false);
    return turi_bool(a[0].as_int == a[1].as_int);
}

/* c-abs: absolute value (fixture helper) */
static TuriValue native_c_abs(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t x = (n > 0) ? a[0].as_int : 0;
    return turi_int(x < 0 ? -x : x);
}
/* popcount: bit population count via __builtin_popcount */
static TuriValue native_popcount(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t x = (n > 0) ? a[0].as_int : 0;
    return turi_int((int64_t)__builtin_popcountll((unsigned long long)x));
}
/* flat array: calloc n int64_t cells */
static TuriValue native_flat_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t sz = (n > 0) ? a[0].as_int : 0;
    if (sz <= 0) return turi_int(0);
    int64_t *arr = (int64_t *)calloc((size_t)sz, sizeof(int64_t));
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)arr; return v;
}
/* flat-get: arr[y*width + x] */
static TuriValue native_flat_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 4) return turi_int(0);
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t x = a[1].as_int, y = a[2].as_int, w = a[3].as_int;
    if (!arr) return turi_int(0);
    return turi_int(arr[y * w + x]);
}
/* flat-set: arr[y*width + x] = val, returns 0 */
static TuriValue native_flat_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 5) return turi_int(0);
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t x = a[1].as_int, y = a[2].as_int, w = a[3].as_int, val = a[4].as_int;
    if (arr) arr[y * w + x] = val;
    return turi_int(0);
}

/* -------------------------------------------------------------------------
 * Set operations
 * Set represented as int64_t[2]: [0]=ptr to sorted int64_t array, [1]=count
 * (matches EX_SET_LIT interpreter layout in eval.c)
 * ---------------------------------------------------------------------- */
static TuriValue native_set_member(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_bool(false);
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    if (!s) return turi_bool(false);
    int64_t *items = (int64_t *)(intptr_t)s[0];
    int64_t cnt = s[1];
    int64_t x = a[1].as_int;
    int64_t lo = 0, hi = cnt - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (items[mid] == x) return turi_bool(true);
        if (items[mid] < x) lo = mid + 1; else hi = mid - 1;
    }
    return turi_bool(false);
}
static TuriValue native_set_count(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(s ? s[1] : 0);
}

/* -------------------------------------------------------------------------
 * Slice operations
 * Slice represented as int64_t[2]: [0]=data ptr, [1]=len
 * ---------------------------------------------------------------------- */
static TuriValue native_slice_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *s = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!s) return turi_nil();
    s[0] = (n >= 1) ? a[0].as_int : 0;
    s[1] = (n >= 2) ? a[1].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)s; return v;
}
static TuriValue native_slice_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(s ? s[1] : 0);
}
static TuriValue native_slice_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  i = a[1].as_int;
    if (!s || i < 0 || i >= s[1]) {
        fprintf(stderr, "slice index out of bounds\n"); fflush(stderr); _exit(1);
    }
    int64_t *data = (int64_t *)(intptr_t)s[0];
    return turi_int(data[i]);
}
static TuriValue native_slice_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 1) { void *p = (void *)(intptr_t)a[0].as_int; if (p) free(p); }
    return turi_nil();
}

/* Vec layout: { int64_t *data; size_t len; size_t cap; }
 * Stored as int64_t[3]: [0]=data ptr (as int64_t), [1]=len, [2]=cap */
static TuriValue native_vec_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    int64_t *v = (int64_t *)calloc(3, sizeof(int64_t));
    if (!v) return turi_nil();
    TuriValue r = {0}; r.tag = TURI_INT; r.as_int = (int64_t)(intptr_t)v; return r;
}
static TuriValue native_vec_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(v ? v[1] : 0);
}
static TuriValue native_vec_capacity(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(v ? v[2] : 0);
}
static TuriValue native_vec_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  i = a[1].as_int;
    if (!v || i < 0 || i >= v[1]) {
        fprintf(stderr, "vec index out of bounds\n");
        fflush(stderr);
        _exit(1);
    }
    int64_t *data = (int64_t *)(intptr_t)v[0];
    return turi_int(data[i]);
}
static TuriValue native_vec_push(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    int64_t *data = (int64_t *)(intptr_t)v[0];
    int64_t  len  = v[1];
    int64_t  cap  = v[2];
    if (len >= cap) {
        int64_t new_cap = cap > 0 ? cap * 2 : 4;
        int64_t *nd = (int64_t *)malloc((size_t)new_cap * sizeof(int64_t));
        if (!nd) return turi_nil();
        for (int64_t j = 0; j < len; j++) nd[j] = data[j];
        free(data);
        v[0] = (int64_t)(intptr_t)nd;
        v[2] = new_cap;
        data = nd;
    }
    data[len] = a[1].as_int;
    v[1] = len + 1;
    return turi_nil();
}
static TuriValue native_vec_pop(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v || v[1] == 0) {
        fprintf(stderr, "vec pop from empty vec\n");
        return turi_int(0);
    }
    int64_t *data = (int64_t *)(intptr_t)v[0];
    v[1]--;
    return turi_int(data[v[1]]);
}
static TuriValue native_vec_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  i = a[1].as_int;
    if (!v || i < 0 || i >= v[1]) {
        fprintf(stderr, "vec index out of bounds\n");
        return turi_nil();
    }
    int64_t *data = (int64_t *)(intptr_t)v[0];
    data[i] = a[2].as_int;
    return turi_nil();
}
static TuriValue native_vec_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    if (!v) return turi_nil();
    int64_t *data = (int64_t *)(intptr_t)v[0];
    free(data);
    free(v);
    return turi_nil();
}

/* nil-value: return 0 (empty list sentinel) */
static TuriValue native_nil_value(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    return turi_int(0);
}
/* cons: allocate a new cons cell {value, next} and return pointer as int64 */
static TuriValue native_cons(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t value = (n > 0) ? a[0].as_int : 0;
    int64_t next  = (n > 1) ? a[1].as_int : 0;
    int64_t *cell = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!cell) return turi_nil();
    cell[0] = value; cell[1] = next;
    return turi_int((int64_t)(intptr_t)cell);
}
/* tail: return the next pointer field of the first cons cell */
static TuriValue native_list_tail(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    int64_t *cell = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(cell[1]);
}
/* list-nil?: true if the cons-cell pointer is 0 (empty list) */
static TuriValue native_list_nil_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    TuriValue rv = {0}; rv.tag = TURI_BOOL;
    rv.as_bool = (n == 0 || a[0].as_int == 0);
    return rv;
}
/* head: return the value field of the first cons cell */
static TuriValue native_list_head(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_nil();
    int64_t *cell = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(cell[0]);
}
/* cstr->parse-int: parse a raw int (cstr pointer as int64) to int64 */
static TuriValue native_cstr_parse_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    const char *s = (const char *)(intptr_t)a[0].as_int;
    return turi_int(s ? (int64_t)atoll(s) : 0);
}
/* bit-shr: logical (unsigned) right shift */
static TuriValue native_bit_shr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    return turi_int((int64_t)((uint64_t)a[0].as_int >> (unsigned)a[1].as_int));
}
/* bit-xor: bitwise XOR of two integers */
static TuriValue native_bit_xor(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    return turi_int(a[0].as_int ^ a[1].as_int);
}
/* println-float: print float with given decimal places */
static TuriValue native_println_float(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? (a[0].tag == TURI_FLOAT ? a[0].as_float : (double)a[0].as_int) : 0.0;
    int d = (n > 1) ? (int)a[1].as_int : 6;
    if (d < 0) d = 0;
    if (d > 17) d = 17;
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%df\n", d);
    printf(fmt, x);
    return turi_nil();
}
/* vec-new-filled: allocate a vec of size sz filled with init */
static TuriValue native_vec_new_filled(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t sz  = (n > 0) ? a[0].as_int : 0;
    int64_t val = (n > 1) ? a[1].as_int : 0;
    if (sz < 0) sz = 0;
    int64_t *v = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!v) return turi_nil();
    int64_t *data = sz > 0 ? (int64_t *)malloc((size_t)sz * sizeof(int64_t)) : NULL;
    for (int64_t i = 0; i < sz; i++) data[i] = val;
    v[0] = (int64_t)(intptr_t)data; v[1] = sz; v[2] = sz;
    TuriValue ret = {0}; ret.tag = TURI_INT; ret.as_int = (int64_t)(intptr_t)v;
    return ret;
}
/* int->unit-float: map a 64-bit int to [0,1) by dividing by 2^53 */
static TuriValue native_int_to_unit_float(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0) ? (double)(uint64_t)a[0].as_int / 9007199254740992.0 : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = v; return rv;
}
/* tur-sqrt: square root via libm */
static TuriValue native_tur_sqrt(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0 && a[0].tag == TURI_FLOAT) ? a[0].as_float : (n > 0 ? (double)a[0].as_int : 0.0);
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = sqrt(x); return rv;
}
/* int->float: cast int64 to double */
static TuriValue native_int_to_float(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0) ? (double)a[0].as_int : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = v; return rv;
}

/* -------------------------------------------------------------------------
 * I/O benchmark native helpers (file_read.tur, file_write.tur).
 *
 * These replace the inline-C helper definitions in the turmeric/ benchmark
 * files so the shared turi/ symlinks work under tur --interpret.
 * ---------------------------------------------------------------------- */

/* Helper: extract a C-string from a TuriValue (TURI_CSTR or TURI_INT ptr). */
static const char *tv_to_cstr(TuriValue v) {
    if (v.tag == TURI_CSTR) return v.as_cstr;
    return (const char *)(intptr_t)v.as_int;
}

/* write-temp-file [path :cstr n :int] :nil -- write n bytes to path. */
static TuriValue native_write_temp_file(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    const char *path = tv_to_cstr(a[0]);
    int64_t bytes = a[1].as_int;
    if (!path || bytes <= 0) return turi_nil();
    char buf[4096];
    memset(buf, 0xCD, sizeof(buf));
    FILE *f = fopen(path, "wb");
    if (!f) return turi_nil();
    int64_t rem = bytes;
    while (rem > 0) {
        int64_t chunk = rem < 4096 ? rem : 4096;
        fwrite(buf, 1, (size_t)chunk, f);
        rem -= chunk;
    }
    fclose(f);
    return turi_nil();
}

/* io-fopen-read [path :cstr] :int -- fopen "rb", return FILE* as int64. */
static TuriValue native_io_fopen_read(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *path = (n >= 1) ? tv_to_cstr(a[0]) : NULL;
    FILE *f = path ? fopen(path, "rb") : NULL;
    return turi_int((int64_t)(intptr_t)f);
}

/* io-fread-chunk [fp :int buf :int] :int -- fread up to 4096 bytes; return bytes read. */
static TuriValue native_io_fread_chunk(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    FILE *fp  = (FILE *)(intptr_t)a[0].as_int;
    void *buf = (void *)(intptr_t)a[1].as_int;
    if (!fp || !buf) return turi_int(0);
    return turi_int((int64_t)fread(buf, 1, 4096, fp));
}

/* io-fclose [fp :int] :nil -- fclose a FILE*. */
static TuriValue native_io_fclose(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) fclose((FILE *)(intptr_t)a[0].as_int);
    return turi_nil();
}

/* io-remove [path :cstr] :nil -- remove a file. */
static TuriValue native_io_remove(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *path = (n >= 1) ? tv_to_cstr(a[0]) : NULL;
    if (path) remove(path);
    return turi_nil();
}

/* io-buf-new [] :int -- malloc 4096 bytes; return pointer as int64. */
static TuriValue native_io_buf_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)malloc(4096));
}

/* io-buf-free [buf :int] :nil -- free a buffer. */
static TuriValue native_io_buf_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

/* io-alloc [n :int v :int] :int -- malloc n bytes filled with byte v. */
static TuriValue native_io_alloc(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    size_t sz  = (n > 0 && a[0].as_int > 0) ? (size_t)a[0].as_int : 0;
    int    val = (n > 1) ? (int)a[1].as_int : 0;
    void *buf = sz ? malloc(sz) : NULL;
    if (buf) memset(buf, val, sz);
    return turi_int((int64_t)(intptr_t)buf);
}

/* io-free [buf :int] :nil -- free an io-alloc'd buffer. */
static TuriValue native_io_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

/* io-fopen-write [path :cstr] :int -- fopen "wb", return FILE* as int64. */
static TuriValue native_io_fopen_write(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *path = (n >= 1) ? tv_to_cstr(a[0]) : NULL;
    FILE *f = path ? fopen(path, "wb") : NULL;
    return turi_int((int64_t)(intptr_t)f);
}

/* io-fwrite-chunk [fp :int buf :int offset :int chunk :int] :int -- fwrite; return bytes. */
static TuriValue native_io_fwrite_chunk(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_int(0);
    FILE *fp    = (FILE *)(intptr_t)a[0].as_int;
    char *buf   = (char *)(intptr_t)a[1].as_int;
    int64_t off = a[2].as_int;
    int64_t len = a[3].as_int;
    if (!fp || !buf || len <= 0) return turi_int(0);
    return turi_int((int64_t)fwrite(buf + off, 1, (size_t)len, fp));
}

/* -------------------------------------------------------------------------
 * Whole-benchmark native implementations for benchmarks whose logic is
 * written entirely in inline-C (legitimate platform I/O or concurrency tests).
 * ---------------------------------------------------------------------- */

/* random-access-bench [file_size :int n_reads :int] :int */
static TuriValue native_random_access_bench(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t file_size = a[0].as_int;
    int64_t n_reads   = a[1].as_int;
    const char *path  = "/tmp/bench_io_random_tur.bin";
    char wbuf[4096];
    FILE *fw = fopen(path, "wb");
    if (!fw) return turi_int(-1);
    int64_t rem = file_size;
    int seq = 0;
    while (rem > 0) {
        int64_t chunk = rem < 4096 ? rem : 4096;
        for (int64_t i = 0; i < chunk; i++) wbuf[i] = (char)(seq++ & 0xFF);
        fwrite(wbuf, 1, (size_t)chunk, fw);
        rem -= chunk;
    }
    fclose(fw);
    FILE *fr = fopen(path, "rb");
    if (!fr) return turi_int(-1);
    uint64_t state = 12345678ULL;
    int64_t  checksum = 0;
    unsigned char byte;
    for (int64_t i = 0; i < n_reads; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        int64_t offset = (int64_t)(state >> 1) % file_size;
        fseek(fr, (long)offset, SEEK_SET);
        if (fread(&byte, 1, 1, fr)) checksum += byte;
    }
    fclose(fr);
    remove(path);
    return turi_int(checksum);
}

/* ring_worker_nat: pthread worker for the thread-ring benchmark. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             ready;
    int64_t         token;
} TRSlot_nat;
typedef struct { TRSlot_nat *ring; int id; int n; } TRArg_nat;
static void *ring_worker_nat(void *vp) {
    TRArg_nat *a = (TRArg_nat *)vp;
    TRSlot_nat *me   = &a->ring[a->id];
    TRSlot_nat *next = &a->ring[(a->id + 1) % a->n];
    while (1) {
        pthread_mutex_lock(&me->mu);
        while (!me->ready) pthread_cond_wait(&me->cv, &me->mu);
        int64_t tok = me->token; me->ready = 0;
        pthread_mutex_unlock(&me->mu);
        int64_t out = tok > 0 ? tok - 1 : tok;
        pthread_mutex_lock(&next->mu);
        next->token = out; next->ready = 1;
        pthread_cond_signal(&next->cv);
        pthread_mutex_unlock(&next->mu);
        if (tok <= 0) return NULL;
    }
}

/* run-ring [n_threads :int messages :int] :nil */
static TuriValue native_run_ring(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int n_threads = (int)a[0].as_int;
    int messages  = (int)a[1].as_int;
    if (n_threads <= 0) return turi_nil();
    TRSlot_nat *ring = (TRSlot_nat *)calloc((size_t)n_threads, sizeof(TRSlot_nat));
    for (int i = 0; i < n_threads; i++) {
        pthread_mutex_init(&ring[i].mu, NULL);
        pthread_cond_init(&ring[i].cv, NULL);
    }
    TRArg_nat *targs = (TRArg_nat *)malloc((size_t)n_threads * sizeof(TRArg_nat));
    for (int i = 0; i < n_threads; i++) {
        targs[i].ring = ring; targs[i].id = i; targs[i].n = n_threads;
    }
    pthread_t *threads = (pthread_t *)malloc((size_t)n_threads * sizeof(pthread_t));
    for (int i = 0; i < n_threads; i++)
        pthread_create(&threads[i], NULL, ring_worker_nat, &targs[i]);
    pthread_mutex_lock(&ring[0].mu);
    ring[0].token = messages; ring[0].ready = 1;
    pthread_cond_signal(&ring[0].cv);
    pthread_mutex_unlock(&ring[0].mu);
    for (int i = 0; i < n_threads; i++) pthread_join(threads[i], NULL);
    printf("done\n");
    free(threads); free(targs); free(ring);
    return turi_nil();
}

/* run-nbody [n_bodies :int steps :int] :nil */
static TuriValue native_run_nbody(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int n_bodies = (int)a[0].as_int;
    int steps    = (int)a[1].as_int;
    if (n_bodies <= 0) return turi_nil();
    typedef struct { double x,y,z,vx,vy,vz,mass; } NBody_nat;
    NBody_nat *b = (NBody_nat *)calloc((size_t)n_bodies, sizeof(NBody_nat));
    uint64_t state = 42;
    for (int i = 0; i < n_bodies; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i].x = (double)(int64_t)(state >> 32) / 1e8;
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i].y = (double)(int64_t)(state >> 32) / 1e8;
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i].z = (double)(int64_t)(state >> 32) / 1e8;
        b[i].vx = b[i].vy = b[i].vz = 0.0;
        b[i].mass = 1.0 + (i % 5) * 0.5;
    }
    for (int s = 0; s < steps; s++) {
        for (int i = 0; i < n_bodies; i++)
            for (int j = i + 1; j < n_bodies; j++) {
                double dx = b[j].x-b[i].x, dy = b[j].y-b[i].y, dz = b[j].z-b[i].z;
                double dist = sqrt(dx*dx+dy*dy+dz*dz) + 1e-10;
                double f = b[i].mass * b[j].mass / (dist*dist*dist);
                b[i].vx+=f*dx; b[i].vy+=f*dy; b[i].vz+=f*dz;
                b[j].vx-=f*dx; b[j].vy-=f*dy; b[j].vz-=f*dz;
            }
        for (int i = 0; i < n_bodies; i++) {
            b[i].x += b[i].vx; b[i].y += b[i].vy; b[i].z += b[i].vz;
        }
    }
    double ke = 0;
    for (int i = 0; i < n_bodies; i++)
        ke += 0.5 * b[i].mass * (b[i].vx*b[i].vx + b[i].vy*b[i].vy + b[i].vz*b[i].vz);
    printf("%.4f\n", ke);
    free(b);
    return turi_nil();
}

/* run-raytracer [width :int height :int] :int */
static TuriValue native_run_raytracer(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t width  = a[0].as_int;
    int64_t height = a[1].as_int;
    typedef struct { double x, y, z; } RT_Vec3;
    typedef struct { RT_Vec3 center; double radius; } RT_Sphere;
#define RT_ADD(a,b) ((RT_Vec3){(a).x+(b).x,(a).y+(b).y,(a).z+(b).z})
#define RT_SUB(a,b) ((RT_Vec3){(a).x-(b).x,(a).y-(b).y,(a).z-(b).z})
#define RT_SCALE(v,s) ((RT_Vec3){(v).x*(s),(v).y*(s),(v).z*(s)})
#define RT_DOT(a,b) ((a).x*(b).x+(a).y*(b).y+(a).z*(b).z)
    RT_Sphere spheres[] = {{{0,0,-5},1.0}, {{2,0,-7},1.5}, {{-3,0,-6},0.8}};
    RT_Vec3 lraw = {1, 1, -1};
    double llen = sqrt(RT_DOT(lraw, lraw)) + 1e-15;
    RT_Vec3 light = RT_SCALE(lraw, 1.0 / llen);
    RT_Vec3 origin = {0, 0, 0};
    int64_t checksum = 0;
    for (int64_t y = 0; y < height; y++) {
        for (int64_t x = 0; x < width; x++) {
            double u = ((double)x / width) * 2 - 1;
            double v = ((double)y / height) * 2 - 1;
            RT_Vec3 dv = {u, v, -1};
            double dlen = sqrt(RT_DOT(dv, dv)) + 1e-15;
            RT_Vec3 dir = RT_SCALE(dv, 1.0 / dlen);
            double best = 1e18; int bi = -1;
            for (int i = 0; i < 3; i++) {
                RT_Vec3 oc = RT_SUB(origin, spheres[i].center);
                double aa = RT_DOT(dir, dir), b2 = RT_DOT(oc, dir);
                double c = RT_DOT(oc, oc) - spheres[i].radius * spheres[i].radius;
                double d = b2*b2 - aa*c;
                if (d >= 0) {
                    double t = (-b2 - sqrt(d)) / aa;
                    if (t > 0.001 && t < best) { best = t; bi = i; }
                }
            }
            if (bi >= 0) {
                RT_Vec3 hp = RT_ADD(origin, RT_SCALE(dir, best));
                RT_Vec3 nv = RT_SUB(hp, spheres[bi].center);
                double nlen2 = sqrt(RT_DOT(nv, nv)) + 1e-15;
                RT_Vec3 norm = RT_SCALE(nv, 1.0 / nlen2);
                double diff = RT_DOT(norm, light);
                if (diff < 0) diff = 0;
                checksum += (int64_t)(diff * 255);
            }
        }
    }
#undef RT_ADD
#undef RT_SUB
#undef RT_SCALE
#undef RT_DOT
    return turi_int(checksum);
}

static void wk_register_stdlib_natives(TuriEnv *env) {
    /* Option/some/none */
    turi_env_register_native(env, "some",            native_some,            NULL);
    turi_env_register_native(env, "none",            native_none,            NULL);
    turi_env_register_native(env, "some?",           native_some_pred,       NULL);
    turi_env_register_native(env, "option-unwrap",   native_option_unwrap,   NULL);
    turi_env_register_native(env, "option-value",    native_option_value,    NULL);
    turi_env_register_native(env, "option-free",     native_option_free,     NULL);
    turi_env_register_native(env, "option-unwrap-or",native_option_unwrap_or,NULL);
    turi_env_register_native(env, "option-must",     native_option_must,     NULL);
    turi_env_register_native(env, "option-expect",   native_option_expect,   NULL);
    /* Result/ok/err */
    turi_env_register_native(env, "ok",              native_ok,              NULL);
    turi_env_register_native(env, "err",             native_err,             NULL);
    turi_env_register_native(env, "ok?",             native_ok_pred,         NULL);
    turi_env_register_native(env, "err?",            native_err_pred,        NULL);
    turi_env_register_native(env, "ok-val",          native_ok_val,          NULL);
    turi_env_register_native(env, "err-val",         native_err_val,         NULL);
    turi_env_register_native(env, "result-unwrap",   native_result_unwrap,   NULL);
    turi_env_register_native(env, "result-unwrap-or",native_result_unwrap_or,NULL);
    turi_env_register_native(env, "result-unwrap-err",native_result_unwrap_err,NULL);
    turi_env_register_native(env, "ok-val-ptr",      native_ok_val_ptr,      NULL);
    turi_env_register_native(env, "err-val-ptr",     native_err_val_ptr,     NULL);
    turi_env_register_native(env, "result-map",      native_result_map,      NULL);
    turi_env_register_native(env, "result-map-err",  native_result_map_err,  NULL);
    turi_env_register_native(env, "result-flat-map", native_result_flat_map, NULL);
    turi_env_register_native(env, "result-or",       native_result_or,       NULL);
    turi_env_register_native(env, "result-or-else",  native_result_or_else,  NULL);
    turi_env_register_native(env, "result-display",  native_result_display,  NULL);
    turi_env_register_native(env, "result-debug",    native_result_debug,    NULL);
    turi_env_register_native(env, "result-error-message", native_result_error_message, NULL);
    turi_env_register_native(env, "result-free",     native_result_free,     NULL);
    turi_env_register_native(env, "result-must",     native_result_must,     NULL);
    turi_env_register_native(env, "result-must-msg", native_result_must_msg, NULL);
    turi_env_register_native(env, "result-expect",   native_result_must_msg, NULL);
    /* String conversion */
    turi_env_register_native(env, "int->str",        native_int_to_str,      NULL);
    turi_env_register_native(env, "str->int",        native_str_to_int,      NULL);
    turi_env_register_native(env, "strcmp",          native_strcmp_fn,       NULL);
    /* Common math/array fixture helpers */
    turi_env_register_native(env, "c-abs",           native_c_abs,           NULL);
    turi_env_register_native(env, "popcount",        native_popcount,        NULL);
    turi_env_register_native(env, "flat-new",        native_flat_new,        NULL);
    turi_env_register_native(env, "flat-get",        native_flat_get,        NULL);
    turi_env_register_native(env, "flat-set",        native_flat_set,        NULL);
    /* Set operations */
    turi_env_register_native(env, "set-member?",     native_set_member,      NULL);
    turi_env_register_native(env, "set-count",       native_set_count,       NULL);
    /* Slice operations */
    turi_env_register_native(env, "slice-new",       native_slice_new,       NULL);
    turi_env_register_native(env, "slice-len",       native_slice_len,       NULL);
    turi_env_register_native(env, "slice-get",       native_slice_get,       NULL);
    turi_env_register_native(env, "slice-free",      native_slice_free,      NULL);
    /* Common fixture helpers: int-val, alloc-int, alloc-key, alloc-str, ptr= */
    turi_env_register_native(env, "int-val",         native_int_val,         NULL);
    turi_env_register_native(env, "alloc-int",       native_alloc_int,       NULL);
    turi_env_register_native(env, "alloc-key",       native_alloc_int,       NULL);
    turi_env_register_native(env, "alloc-str",       native_alloc_str,       NULL);
    turi_env_register_native(env, "cstr-free",       native_cstr_free,       NULL);
    turi_env_register_native(env, "ptr=",            native_ptr_eq,          NULL);
    /* Vec operations */
    turi_env_register_native(env, "vec-new",         native_vec_new,         NULL);
    turi_env_register_native(env, "vec-len",         native_vec_len,         NULL);
    turi_env_register_native(env, "vec-capacity",    native_vec_capacity,    NULL);
    turi_env_register_native(env, "vec-get",         native_vec_get,         NULL);
    turi_env_register_native(env, "vec-push!",       native_vec_push,        NULL);
    turi_env_register_native(env, "vec-push-ptr!",   native_vec_push,        NULL);
    turi_env_register_native(env, "vec-pop!",        native_vec_pop,         NULL);
    turi_env_register_native(env, "vec-set!",        native_vec_set,         NULL);
    turi_env_register_native(env, "vec-free",        native_vec_free,        NULL);
    turi_env_register_native(env, "result-collect",  native_result_collect,  NULL);
    turi_env_register_native(env, "result-partition",native_result_partition, NULL);
    turi_env_register_native(env, "result-partition-ok", native_result_partition_ok, NULL);
    turi_env_register_native(env, "result-partition-err", native_result_partition_err, NULL);
    /* List operations for benchmark arg parsing and list_ops benchmark */
    turi_env_register_native(env, "nil-value",         native_nil_value,       NULL);
    turi_env_register_native(env, "cons",              native_cons,            NULL);
    turi_env_register_native(env, "list-nil?",         native_list_nil_pred,   NULL);
    turi_env_register_native(env, "head",              native_list_head,       NULL);
    turi_env_register_native(env, "tail",              native_list_tail,       NULL);
    /* Benchmark micro-helpers */
    turi_env_register_native(env, "cstr->parse-int",  native_cstr_parse_int,  NULL);
    turi_env_register_native(env, "bit-shr",           native_bit_shr,         NULL);
    turi_env_register_native(env, "bit-xor",           native_bit_xor,         NULL);
    turi_env_register_native(env, "println-float",     native_println_float,   NULL);
    turi_env_register_native(env, "vec-new-filled",    native_vec_new_filled,  NULL);
    turi_env_register_native(env, "int->unit-float",   native_int_to_unit_float, NULL);
    turi_env_register_native(env, "tur-sqrt",          native_tur_sqrt,        NULL);
    turi_env_register_native(env, "int->float",        native_int_to_float,    NULL);
    /* HAMT operations for hash_map benchmark (int-typed wrappers) */
    turi_env_register_native(env, "hamt-new",          native_tur_hamt_new,    NULL);
    turi_env_register_native(env, "hamt-free",         native_tur_hamt_free,   NULL);
    turi_env_register_native(env, "hamt-set",          native_tur_hamt_set,    NULL);
    turi_env_register_native(env, "hamt-get",          native_tur_hamt_get,    NULL);
    turi_env_register_native(env, "hamt-hash-ptr",     native_tur_hamt_hash_ptr, NULL);
    /* I/O benchmark helpers */
    turi_env_register_native(env, "write-temp-file",   native_write_temp_file, NULL);
    turi_env_register_native(env, "io-fopen-read",     native_io_fopen_read,   NULL);
    turi_env_register_native(env, "io-fread-chunk",    native_io_fread_chunk,  NULL);
    turi_env_register_native(env, "io-fclose",         native_io_fclose,       NULL);
    turi_env_register_native(env, "io-remove",         native_io_remove,       NULL);
    turi_env_register_native(env, "io-buf-new",        native_io_buf_new,      NULL);
    turi_env_register_native(env, "io-buf-free",       native_io_buf_free,     NULL);
    turi_env_register_native(env, "io-alloc",          native_io_alloc,        NULL);
    turi_env_register_native(env, "io-free",           native_io_free,         NULL);
    turi_env_register_native(env, "io-fopen-write",    native_io_fopen_write,  NULL);
    turi_env_register_native(env, "io-fwrite-chunk",   native_io_fwrite_chunk, NULL);
    /* Whole-benchmark natives */
    turi_env_register_native(env, "random-access-bench", native_random_access_bench, NULL);
    turi_env_register_native(env, "run-ring",          native_run_ring,        NULL);
    turi_env_register_native(env, "run-nbody",         native_run_nbody,       NULL);
    turi_env_register_native(env, "run-raytracer",     native_run_raytracer,   NULL);
}

/* -------------------------------------------------------------------------
 * Native implementations of safe.tur stdlib functions (inline-C bodies).
 * These allow safe.tur functions to run correctly in interpreter mode.
 * ---------------------------------------------------------------------- */

/* array-get [arr :ptr<void> idx :int] :ptr
 * Returns a heap-allocated { bool is_some; int64_t value; } option. */
static TuriValue native_safe_array_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  idx = a[1].as_int;
    /* Layout: { bool is_some (8 bytes as int64), int64_t value } */
    int64_t *opt = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!opt) return turi_nil();
    if (arr && idx >= 0 && idx < 1024) {
        opt[0] = 1; /* is_some = true */
        opt[1] = arr[idx];
    } else {
        opt[0] = 0; /* is_some = false */
        opt[1] = 0;
    }
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)opt;
    return v;
}

/* array-set [arr :ptr<void> idx :int value :int] :int
 * Returns 1 on success, 0 on out-of-range. */
static TuriValue native_safe_array_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    int64_t *arr = (int64_t *)(intptr_t)a[0].as_int;
    int64_t  idx = a[1].as_int;
    int64_t  val = a[2].as_int;
    if (arr && idx >= 0 && idx < 1024) {
        arr[idx] = val;
        return turi_int(1);
    }
    return turi_int(0);
}

/* box [v :int] :ptr -- allocate an int64_t on the heap */
static TuriValue native_safe_box(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p;
    return v;
}

/* unbox [p :ptr] :int -- read int64_t from heap pointer */
static TuriValue native_safe_unbox(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *p = (int64_t *)(intptr_t)a[0].as_int;
    if (!p) return turi_int(0);
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = *p;
    return v;
}

static void wk_register_safe_natives(TuriEnv *env) {
    turi_env_register_native(env, "array-get",   native_safe_array_get, NULL);
    turi_env_register_native(env, "array-set",   native_safe_array_set, NULL);
    turi_env_register_native(env, "box",         native_safe_box,       NULL);
    turi_env_register_native(env, "unbox",       native_safe_unbox,     NULL);
}

/* -------------------------------------------------------------------------
 * Native overrides for typeclass instance methods with inline-C bodies.
 * These are registered under the elaborator-generated C binding names
 * (e.g. __inst_Show_show_int) so that eval_apply can find them when a
 * function has an EX_INLINE_C body.
 * ---------------------------------------------------------------------- */

/* Show [int].show: format int64 as decimal string */
static TuriValue native_show_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    char *buf = (char *)malloc(32);
    if (!buf) return turi_nil();
    snprintf(buf, 32, "%lld", (long long)v);
    /* Note: this leaks 'buf'. Acceptable for interpreter mode. */
    return turi_cstr(buf);
}

/* Show [float].show / Show [bool].show / Show [cstr].show */
static TuriValue native_show_float(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0 && a[0].tag == TURI_FLOAT) ? a[0].as_float : 0.0;
    char *buf = (char *)malloc(64);
    if (!buf) return turi_nil();
    snprintf(buf, 64, "%g", v);
    return turi_cstr(buf);
}
static TuriValue native_show_bool(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    bool v = (n > 0 && a[0].tag == TURI_BOOL) ? a[0].as_bool : false;
    return turi_cstr(v ? "true" : "false");
}
static TuriValue native_show_cstr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    return (n > 0 && a[0].tag == TURI_CSTR) ? a[0] : turi_nil();
}

/* show-float: standalone show function for floats (used in show-float fixture) */
static TuriValue native_show_float_fn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    return native_show_float(env, a, n, ud);
}

static void wk_register_typeclass_natives(TuriEnv *env) {
    /* Show typeclass instances */
    turi_env_register_native(env, "__inst_Show_show_int",   native_show_int,   NULL);
    turi_env_register_native(env, "__inst_Show_show_float", native_show_float, NULL);
    /* TY_FLOAT falls through to "T" suffix in the elaborator's type suffix builder */
    turi_env_register_native(env, "__inst_Show_show_T",     native_show_float, NULL);
    turi_env_register_native(env, "__inst_Show_show_bool",  native_show_bool,  NULL);
    turi_env_register_native(env, "__inst_Show_show_cstr",  native_show_cstr,  NULL);
    /* Standalone show helpers used in some fixtures */
    turi_env_register_native(env, "show-float", native_show_float_fn, NULL);
    turi_env_register_native(env, "show-int",   native_show_int,      NULL);
}

/* Native implementation of tur-contract-check (bool * cstr -> void).
 * Panics (exit 1) if the condition is false; otherwise returns nil. */
static TuriValue native_contract_check(TuriEnv *env, TuriValue *args,
                                        uint32_t n, void *ud) {
    (void)env; (void)ud;
    bool cond = true;
    if (n >= 1) {
        TuriValue a = args[0];
        if (a.tag == TURI_BOOL)      cond = a.as_bool;
        else if (a.tag == TURI_INT)  cond = (a.as_int != 0);
        else if (a.tag == TURI_NIL)  cond = false;
    }
    if (!cond) {
        const char *msg = (n >= 2 && args[1].tag == TURI_CSTR && args[1].as_cstr)
                          ? args[1].as_cstr : "Assertion failed";
        fprintf(stderr, "panic at\n%s\n", msg);
        fflush(stderr);
        exit(1);
    }
    return turi_nil();
}

/* Native implementation of tur-contract-check-inv (obj pred msg -> void).
 * Calls pred(obj); panics if it returns false. */
static TuriValue native_contract_check_inv(TuriEnv *env, TuriValue *args,
                                            uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_nil();
    TuriValue obj  = args[0];
    TuriValue pred = args[1];
    const char *msg = (args[2].tag == TURI_CSTR && args[2].as_cstr)
                      ? args[2].as_cstr : "Invariant failed";
    if (pred.tag != TURI_CLOSURE || !pred.as_closure) {
        fprintf(stderr, "panic at\n%s (bad predicate)\n", msg);
        fflush(stderr);
        exit(1);
    }
    TuriValue result = turi_call(env, pred, &obj, 1);
    bool ok = true;
    if (result.tag == TURI_BOOL)     ok = result.as_bool;
    else if (result.tag == TURI_INT) ok = (result.as_int != 0);
    else if (result.tag == TURI_NIL) ok = false;
    if (!ok) {
        fprintf(stderr, "panic at\n%s\n", msg);
        fflush(stderr);
        exit(1);
    }
    return turi_nil();
}

/* Native contract-enabled? -- always returns true in worker mode. */
static TuriValue native_contract_enabled(TuriEnv *env, TuriValue *args,
                                          uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_bool(true);
}

/* Run the interpreter on 'input' in a forked child.
 * Returns child exit code; writes captured stdout and stderr into out and err.
 * Caller frees *out and *err. */
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
        /* Register native HAMT implementations so persistent map operations
         * work correctly in interpreter mode. */
        wk_register_hamt_natives(env);
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

/* GS-M2: scan $PATH for `tur-*` executables and print them as an
 * "External commands:" block beneath the built-in usage. Built-in
 * subcommands always win at dispatch time, so we filter any `tur-<name>`
 * that shadows a built-in to avoid misleading the user. Within $PATH,
 * the first hit for each name wins (matching exec() resolution). */
static void list_external_subcommands(void) {
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return;

    static const char *const builtins[] = {
        "build", "emit-c", "emit-h", "emit-cmake", "run", "repl", "worker",
        "eval", "doc", "explain", "test", "check", "format", "fmt",
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

static int usage(void) {
    fprintf(stderr,
        "tur: the Turmeric compiler (phase 8)\n"
        "\n"
        "usage:\n"
        "  tur build <file.tur> [-o <out>]    build a single file\n"
        "  tur build <dir> [-o <out>]         build all .tur files in directory\n"
        "  tur emit-c <input.tur>            print the generated C to stdout\n"
        "  tur emit-h <input.tur>            print the generated header to stdout\n"
        "  tur run <input.tur>               build + execute a single file\n"
        "  tur repl                          interactive REPL (Phase S1)\n"
        "  tur worker                        persistent fixture evaluator (Tier 3, reads dirs from stdin)\n"
        "  tur --interpret <file.tur>        run a file through the tree-walking interpreter\n"
        "  tur eval '<expr>'                 evaluate an inline expression\n"
        "  tur doc <symbol>                  print documentation for a builtin or special form\n"
        "  tur explain <TUR-E####|snippet>   explain a diagnostic code or snippet errors\n"
        "  tur test <dir>                    run all .tur files in a directory\n"
        "  tur check <input.tur>             type-check only, no codegen (phase 8)\n"
        "  tur format [--check|--diff] [file.tur]   format source (stdin if no file given)\n"
        "  tur fmt [--check|--diff|--dry-run] [paths...]  format in place with dir walking\n"
        "\n"
        "package management (Spice, Phase PKG-1):\n"
        "  tur init [--bin|--lib] <name>     create a new project\n"
        "  tur add <url> [--ref <tag>]       add a Turmeric spice\n"
        "  tur add <path> --path             add a local spice\n"
        "  tur add-cmake <url> [--ref <tag>] add a C/CMake dependency\n"
        "  tur fetch [--update]              download / update all spices\n"
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
        "  --no-color                       disable colored diagnostics\n"
        "  --json                           structured JSON output (tur doc, tur test, tur check)\n"
        "  --json-diagnostics               output diagnostics as JSON (phase 8)\n"
        "  --explain <TUR-E####>            print explanation for a diagnostic code (HKT-P5)\n"
        "  --explain <snippet>              compile code snippet and explain errors (phase 8)\n"
        "  --dump-kinds                     dump kind annotations after kind-check (HKT-P6)\n"
        "  --strict-effects                 warn on unannotated effectful functions (ER1)\n"
        "  --dump-effects                   print inferred effect row for each defn (ER6)\n"
        "  --lint-effects                   advisory warnings for unannotated effectful functions (ER6)\n"
        "  --backtrack-depth <N>            cap run-backtrack at N results (0=unlimited) (Phase B5)\n"
        "  --dump-clone-plan                dump cloneable capture plan after CPS (Phase B5)\n"
        "  --emit-abi-trace                 print the resolved ABI path per call site during emit-c (Phase I)\n"
        "  --panic-abort                   all panics call abort() directly (Phase R5)\n"
        "  --panic-trace                   print scope chain on panic (Phase R6)\n"
        "  --warn-unused-result             warn on discarded result values (Phase R6)\n"
        "  --no-warn-unused-result          disable --warn-unused-result (Phase R6)\n"
        "  --lint-panic                     lint panic/must! usage (Phase R6)\n"
        "  -Xeffect-types                   enable full effect typing: TY_HANDLER, ET4 checks (ET4)\n"
        "  -Xlinear                         enable linear type checking (LT0-LT4)\n"
        "  -Xunique-types                   enable uniqueness type checking (UT0-UT3)\n"
        "  -Xsubstructural                  enable substructural type checking (ST0-ST3; implies -Xlinear)\n"
        "  -Xsessions                       enable session type syntax and checking (SS0-SS2; implies -Xsubstructural)\n"
        "  -Xunion-types                    enable union type syntax: (A | B | C) (IT0-IT1)\n"
        "  -Xintersection-types             enable intersection type syntax: (A & B & C) (IT2)\n"
        "  -Xcontracts                      enable contract checks (default in debug builds) (CT3)\n"
        "  --keep-contracts                 retain contract checks in release builds (CT3)\n"
        "  -Xdynamic-vars                   enable dynamic var syntax: (defdynamic *name* :type val) (DV0+)\n");
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
        "  tur emit-c [-I <dir>...] <file.tur>              emit C to stdout\n"
        "  tur emit-c [-I <dir>...] --output-dir <dir> <files...>  emit per-module .h/.c\n"
        "  tur emit-h [-I <dir>...] <file.tur>              emit header to stdout\n"
        "\n"
        "flags:\n"
        "  -o <out>          output file path\n"
        "  -I <dir>          add include directory for module resolution\n"
        "                    (repeat to add multiple; intra-spice imports usually\n"
        "                    want `-I src` from the spice root)\n"
        "  --shared          build a shared library (`-fPIC -shared`, no main);\n"
        "                    requires a directory argument. Exported defns are\n"
        "                    callable via dlopen/dlsym as `<module>__<name>`.\n"
        "  --manifest <p>    (with --shared) write exports.manifest to <p>\n"
        "                    (defaults to `<out>.manifest`). Lists each export\n"
        "                    as `<mod>/<defn> -> <mangled> :: (:args) -> :ret`.\n"
        "  --target wasm     compile to WebAssembly via emcc (requires Emscripten)\n"
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

static int usage_test(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur test <dir>   run all .tur test files in a directory\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_repl(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur repl [--watch]   start the interactive REPL\n"
        "\n"
        "flags:\n"
        "  --watch         auto-reload the enclosing spice between prompts\n"
        "                  when any source .tur file's mtime advances\n"
        "                  (RP6; equivalent to typing (reload) each turn)\n"
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

/* Phase R5: Handle --panic-abort flag */
static bool parse_panic_abort(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
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

/* Phase 8: Handle --explain flag - compile code snippet and show detailed error */

/* Phase HKT-P5: Return true if `s` looks like a diagnostic code string
 * of the form "TUR-E" followed by one or more decimal digits. */
static bool looks_like_diag_code_(const char *s) {
    if (!s) return false;
    /* Accept TUR-E#### and TUR-W#### */
    if (strncmp(s, "TUR-", 4) != 0) return false;
    const char *p = s + 4;
    if (*p != 'E' && *p != 'W') return false;
    p++;
    if (*p == '\0') return false;   /* need at least one digit */
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        p++;
    }
    return true;
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
    /* SN1: stash argv[0] for exe-path fallback in resolve_stdlib_root().
     * Platform APIs (_NSGetExecutablePath / /proc/self/exe) are tried
     * first; argv[0] is the last-resort path source. */
    g_argv0 = (argc > 0) ? argv[0] : NULL;
    /* Resolve the stdlib root once at startup so TUR_STDLIB_DIR is
     * propagated into the process env before any subsystem (elaborator,
     * worker, interpreter) reads it. */
    (void)resolve_stdlib_root();

    /* Phase 8: Check for global flags before command */
    bool no_color = parse_no_color(argc, argv);
    bool explain_mode = false;
    const char *explain_code = NULL;
    g_panic_abort = parse_panic_abort(argc, argv);
    g_panic_trace = parse_panic_trace(argc, argv);
    g_warn_unused_result = parse_warn_unused_result(argc, argv);
    g_lint_panic = parse_lint_panic(argc, argv);
    /* F4: --Werror=deprecated promotes ^deprecated warnings to errors */
    g_werror_deprecated = parse_werror_deprecated(argc, argv);
    /* Phase C: --Werror=inline-c-narrow-params promotes narrow-param warnings */
    g_werror_inline_c_narrow_params = parse_werror_inline_c_narrow_params(argc, argv);
    /* SC4: --no-auto-spice disables enclosing-spice auto-discovery in
     * per-file subcommands (check/emit-c/emit-h/run). */
    g_no_auto_spice = parse_no_auto_spice(argc, argv);

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
        } else if (strcmp(argv[i], "--strict-effects") == 0) {
            /* ER1: enforce unannotated effectful functions as warnings */
            g_strict_effects = true;
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
        } else if (strcmp(argv[i], "--emit-abi-trace") == 0) {
            /* Phase I: trace the resolved ABI path for each call site during emit */
            g_emit_abi_trace = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xeffect-types") == 0) {
            /* ET4: enable full effect typing (TY_HANDLER, handler typing, ET4 checks) */
            g_effect_types_enabled = true;
            g_strict_effects = true;  /* -Xeffect-types implies --strict-effects */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xgadt") == 0) {
            /* Phase G1: enable defgadt syntax */
            g_gadt_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xlinear") == 0) {
            /* LT0: enable linear type checking */
            g_linear_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xunique-types") == 0) {
            /* UT0: enable uniqueness type checking */
            g_unique_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xsubstructural") == 0) {
            /* ST0: enable substructural type checking (implies -Xlinear) */
            g_substructural_enabled = true;
            g_linear_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xsessions") == 0) {
            /* SS0a: enable session type syntax and checking (implies -Xsubstructural, -Xlinear) */
            g_sessions_enabled = true;
            g_substructural_enabled = true;
            g_linear_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xdynamic-vars") == 0) {
            /* DV0: enable dynamic var syntax and checking */
            g_dynvar_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xunion-types") == 0) {
            /* IT0: enable union type syntax and checking */
            g_union_types_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xintersection-types") == 0) {
            /* IT2: enable intersection type syntax */
            g_intersection_types_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "-Xcontracts") == 0) {
            /* CT3: explicitly enable contract checks (already on by default in debug) */
            g_contracts_enabled = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        } else if (strcmp(argv[i], "--keep-contracts") == 0) {
            /* CT3: keep contract checks in release builds */
            g_contracts_enabled = true;
            g_keep_contracts_in_release = true;
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
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
    
    /* E2: --version / -V */
    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("turmeric " TUR_VERSION "\n");
        return 0;
    }

    /* E1: --help / -h at top level */
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }

    if (argc < 2) return usage();
    const char *cmd = argv[1];

    if (strcmp(cmd, "emit-c") == 0) {
        /* SC2: collect -I flags up front so both emit-c forms see them. */
        char **emit_inc = NULL;
        int    n_emit_inc = parse_include_flags(argc, argv, 2, &emit_inc);
        if (n_emit_inc < 0) { free(emit_inc); return usage_build(); }

        /* tur emit-c [-I <dir>...] --output-dir <dir> <file1> [<file2> ...] */
        int od_idx = -1;
        for (int i = 2; i < argc; i++) {
            int c;
            if (is_include_flag(argc, argv, i, &c)) { i += c - 1; continue; }
            if (strcmp(argv[i], "--output-dir") == 0) { od_idx = i; break; }
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
    if (strcmp(cmd, "lsp") == 0) {
        diag_init(false);   /* no color -- stdout is reserved for JSON-RPC */
        lsp_server_run(STDIN_FILENO, STDOUT_FILENO);
        return 0;
    }
    if (strcmp(cmd, "build") == 0) {
        const char *input = NULL;
        const char *out = NULL;
        const char *build_target = NULL;
        bool        shared = false;  /* RP0: --shared selects shared-library build */
        const char *manifest_out = NULL; /* RP1: --manifest <path> override */
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
            } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
                manifest_out = argv[++i];
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
                                       (const char **)build_inc, n_build_inc);
            } else {
                rc = cmd_build_multi(input, out, shared, manifest_out);
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
                snprintf(mp, sizeof(mp), "%s/build.tur", b_root);
                PkgManifest bm; memset(&bm, 0, sizeof(bm));
                if (pkg_manifest_read(mp, &bm)) {
                    b_rm = resolve_manifest_reader_macros(b_root, &bm, &b_n);
                }
                pkg_manifest_free(&bm);
            }
            rc = cmd_build(input, out, (const char **)build_inc, n_build_inc,
                           build_target, (const char **)b_rm, b_n);
            free_reader_macro_paths(b_rm, b_n);
            free(b_root);
        }
        free(build_inc);
        return rc;
    }
    if (strcmp(cmd, "run") == 0) {
        /* Disambiguate: if the first non-flag argument ends in .tur or
         * .tursweet, use the classic compile-and-run path; if --release /
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
                strncmp(argv[i], "-I", 2) == 0) {
                use_classic = true;
                break;
            }
            if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
                /* Check if it looks like a .tur file path */
                const char *a = argv[i];
                size_t an = strlen(a);
                if ((an > 4  && strcmp(a + an - 4,  ".tur")      == 0) ||
                    (an > 9  && strcmp(a + an - 9,  ".tursweet") == 0) ||
                    strcmp(a, "-") == 0) {
                    use_classic = true;
                }
                break;
            }
        }
        if (use_classic)
            return cmd_run(argc, argv);
        return cmd_justrun(argc, argv);
    }
    if (strcmp(cmd, "repl") == 0) {
        /* Phase S0: interactive REPL.
         * RP6: --watch enables auto-reload between prompts when a
         * spice source file changes mtime. No background thread --
         * the freshness check runs synchronously each turn. */
        bool watch_mode = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                return usage_repl();
            }
            if (strcmp(argv[i], "--watch") == 0) {
                watch_mode = true;
                continue;
            }
            fprintf(stderr, "tur repl: unknown option '%s'\n", argv[i]);
            return usage_repl();
        }
        return cmd_repl(watch_mode);
    }
    /* Tier 3: persistent fixture worker for the test suite. */
    if (strcmp(cmd, "worker") == 0) {
        return cmd_worker();
    }
    if (strcmp(cmd, "--interpret") == 0) {
        if (argc < 3) {
            fprintf(stderr, "tur: --interpret requires a file argument\n");
            return usage();
        }
        return cmd_eval(argv[2], !no_color && stderr_is_tty(), argv + 3, argc - 3);
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
            return cmd_eval(src, use_color, NULL, 0);
        return cmd_eval_expr(src, use_color);
    }
    /* E5: tur doc <symbol> */
    if (strcmp(cmd, "doc") == 0) {
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_doc();
        if (argc != 3) return usage_doc();
        return cmd_doc_cli(argv[2]);
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

    /* GS-M2: subcommand fallthrough — `tur foo bar` execs `tur-foo bar`
     * from $PATH when "foo" isn't a built-in. Built-ins always win.
     * If exec succeeds, this does not return. */
    return try_external_subcommand(argc, argv);
}
