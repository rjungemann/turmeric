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
#include <limits.h>
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
#include "cps_ir.h"       /* CPS2: ANF/CPS IR (--dump-cps) */
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
#include "span_audit.h"  /* debugger Phase 1: breakpoint-span coverage audit */
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
#include "experiments.h"  /* XF1: --enable=<name> experimental-flag registry */
/* LSP server */
#include "lsp/lsp.h"
#include "lsp/lsp_sym.h"
#include "lsp/lsp_docs.h"
/* MCP server */
#include "lsp/mcp.h"
/* DAP server (debugger Phase 3) */
#include "turi/dap.h"
/* lsp-lite: lightweight completion/calltip backend for editors */
#include "cli/lsp_lite.h"

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

/* Helper to detect language and adjust source for #lang directive */
static ReaderType detect_and_adjust_lang(const char *path, char *src, size_t len,
                                        const char **out_src, size_t *out_len) {
    ReaderType ext_type = reader_type_from_extension(path);

    /* Always run detect_lang so any leading "#lang ..." line is stripped from
     * the source — otherwise the chosen reader would choke on the '#'. When
     * the extension already selected a non-default reader, the extension
     * still wins; the directive is treated as an optional, redundant hint. */
    const char *src_rest = src;
    size_t len_rest = len;
    ReaderType lang_type = detect_lang(src, len, &src_rest, &len_rest);

    ReaderType detected_type = (ext_type != READER_TURMERIC) ? ext_type : lang_type;

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
/* Auto-loaded stdlib files.  Shared by compile_to_c (single-file) and
 * compile_to_h / compile_to_implementation (project-mode multi-file) so
 * spice code in `tur build .` sees `Cons`, `tnil?`, `Option`, etc. without
 * explicit imports -- matching single-file semantics.  Each TU embeds its
 * own static copy of stdlib defns; emit_module.c's
 * `is_from_stdlib`-aware paths static-ify them per TU so multi-TU links
 * don't see duplicate symbols. */
static const char *const g_stdlib_autoload_files[] = {
    "macros.tur",
    "safe.tur",
    "contract.tur",
    "hamt.tur",
    "typeclass-eq.tur",
    "typeclass-functor.tur",
    "typeclass-clone.tur",
    "typeclass-hash.tur",
    "typeclass-applicative.tur",
    "typeclass-alternative.tur",
    "typeclass-monad.tur",
    "typeclass-monaderror.tur",
    "typeclass-bifunctor.tur",
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
    "json.tur",
    "schema.tur",
    "sym.tur",
    "unique.tur",
    NULL
};

/* Read every stdlib file in g_stdlib_autoload_files into `arena`/`st`, then
 * prepend the resulting forms onto `*forms_in_out` (growing the array via a
 * fresh arena allocation).  Updates `*nforms_in_out` and the running
 * `*file_id_in_out` counter.  Returns the count of prepended stdlib forms
 * (which the caller passes to `elaborate_program` as `stdlib_prefix`) so the
 * elab loop can bracket those forms with `in_stdlib_load = true` and the
 * binding records get `is_from_stdlib` set -- the same flag the
 * separate-compilation emit path keys off to static-ify stdlib defns per
 * TU.  `entry_path` is the user-visible input file: used by the suffix-skip
 * rule for `--no-auto-stdlib` builds where the entry file IS one of the
 * stdlib files (everything from that file onward is skipped). */
static uint32_t prepend_stdlib_forms(Arena *arena, SymbolTable *st,
                                     const char *entry_path,
                                     Form ***forms_in_out,
                                     uint32_t *nforms_in_out,
                                     uint8_t *file_id_in_out) {
    int no_stdlib_skip_from = -1;
    if (g_no_auto_stdlib) {
        const char *input_base = basename_of(entry_path);
        for (int j = 0; g_stdlib_autoload_files[j] != NULL; j++) {
            if (strcmp(input_base, g_stdlib_autoload_files[j]) == 0) {
                no_stdlib_skip_from = j;
                break;
            }
        }
    }

    uint32_t total = 0;
    Form **all = NULL;
    for (int i = 0; g_stdlib_autoload_files[i] != NULL; i++) {
        if (no_stdlib_skip_from >= 0 && i >= no_stdlib_skip_from) continue;
        char path_buf[4096];
        tur_stdlib_path(g_stdlib_autoload_files[i], path_buf, sizeof(path_buf));
        char *stdlib_src = NULL;
        size_t stdlib_len = 0;
        if (read_entire_file_quiet(path_buf, &stdlib_src, &stdlib_len) != 0)
            continue;

        char *src_copy = (char *)arena_alloc(arena, stdlib_len);
        memcpy(src_copy, stdlib_src, stdlib_len);
        char *path_copy = (char *)arena_alloc(arena, strlen(path_buf) + 1);
        memcpy(path_copy, path_buf, strlen(path_buf) + 1);

        SourceFile *stdlib_file = (SourceFile *)arena_alloc(arena, sizeof(SourceFile));
        *stdlib_file = (SourceFile){0};
        stdlib_file->path = path_copy;
        stdlib_file->src = src_copy;
        stdlib_file->len = stdlib_len;
        stdlib_file->file_id = (*file_id_in_out)++;
        stdlib_file->reader_type = READER_TURMERIC;
        diag_register_file(stdlib_file);

        uint32_t n = 0;
        Form **fs = read_all(arena, st, stdlib_file, &n);
        if (fs && n > 0) {
            Form **new_all = (Form **)arena_alloc(arena,
                (total + n) * sizeof(Form *));
            for (uint32_t j = 0; j < total; j++) new_all[j] = all[j];
            for (uint32_t j = 0; j < n; j++) new_all[total + j] = fs[j];
            all = new_all;
            total += n;
        }
        free(stdlib_src);
    }

    if (all && total > 0) {
        Form **out = (Form **)arena_alloc(arena,
            (*nforms_in_out + total) * sizeof(Form *));
        for (uint32_t i = 0; i < total; i++) out[i] = all[i];
        for (uint32_t i = 0; i < *nforms_in_out; i++)
            out[total + i] = (*forms_in_out)[i];
        *forms_in_out = out;
        *nforms_in_out += total;
    }
    return total;
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
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

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

    /* Phase 7: prepend stdlib autoload forms.  Shared with compile_to_h via
     * prepend_stdlib_forms so project-mode builds see the same stdlib API
     * (Cons / Option / Result / typeclass stubs / etc.) that single-file
     * builds do.  See docs/archive/project-mode-no-stdlib-autoload.md. */
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
        if (g_collect_syms_out && ctx.prog)
            collect_symbols_from_prog(ctx.prog);
        if (g_audit_spans) {
            /* Audit-only mode: never emit.  A clean audit is rc 0; remaining
             * holes surface as rc 3 (distinct from rc 1 = elaboration failure,
             * rc 2 = file error) so harnesses can tell "audited, found holes"
             * apart from "could not audit". */
            if (rc == 0 && g_audit_span_holes > 0) rc = 3;
        } else if (rc == 0 && emit_program(out_c, ctx.prog) != 0) {
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
     * docs/archive/project-mode-no-stdlib-autoload.md.
     *
     * Codegen note: bindings created during this prelude window are
     * marked `is_from_stdlib`. emit_implementation skips non-exported
     * stdlib bindings; exported ones (`ok`, `ok-val`,
     * `make-struct Result ...`) are static-or-inline-C wrappers so the
     * per-TU duplication mirrors `emit_closure_fat_runtime`. */
    const char *const *prelude_files = g_stdlib_autoload_files;

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
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    /* Fresh diagnostic slate per compilation unit -- see compile_to_c.  The
     * project-mode dir build loops compile_to_h / compile_to_implementation
     * in-process, so a stale `had_error_` from an earlier module would
     * otherwise short-circuit every later module's `diag_had_error()` gate. */
    diag_reset();
    experiment_reset_warnings();  /* XF2: once-per-compile TUR-W006x dedup */

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
    ReaderType reader_type = detect_and_adjust_lang(path, src, len, &src_adj, &len_adj);

    /* Fresh diagnostic slate per compilation unit -- see compile_to_c.  The
     * project-mode dir build loops compile_to_h / compile_to_implementation
     * in-process, so a stale `had_error_` from an earlier module would
     * otherwise short-circuit every later module's `diag_had_error()` gate. */
    diag_reset();
    experiment_reset_warnings();  /* XF2: once-per-compile TUR-W006x dedup */

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
     * top of the generated C so stdlib modules can add file-level includes. */
    hoist_tur_include_directives(&csrc);

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
    {
        /* Walk up from the input file's directory to find project root.
         * Resolve to an absolute path first -- find_project_root walks via
         * strrchr('/'), so a bare "." or "foo.tur" would stop after one
         * step and miss the enclosing spice. */
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
                pkg_cmake_manifest_append_cc_flags(&cmake_manifest, &cmake_flags);
                pkg_cmake_manifest_free(&cmake_manifest);
            }
            collect_spice_aux_c(proj_root, &aux_includes, &aux_sources);
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

    /* When -lturi is linked, libturi.a already contains every runtime
     * translation unit (hamt.c.o, gc.c.o, eval.c.o, ...).  A program that
     * also pulls in a runtime source directly via a `__tur_autolink__:
     * src/runtime/<x>.c` hint (e.g. stdlib/hamt.tur's hamt/autolink-hint,
     * dragged in transitively by `import turi/eval`) would then define those
     * symbols twice and the link fails with `multiple definition of
     * tur_hamt_*`.  The library supersedes the standalone sources, so drop
     * any bare `.c` source argument from the autolink flags whenever -lturi
     * is present.  See docs/archive/tur-eval-import-duplicate-hamt-symbols.md. */
    if (autolink.len > 1 && strstr(autolink.data, "-lturi")) {
        Buf filtered;
        buf_init(&filtered);
        const char *p = autolink.data;
        while (*p) {
            while (*p == ' ') p++;          /* skip leading spaces */
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ' ') p++;     /* token = [start, p) */
            size_t tlen = (size_t)(p - start);
            /* Drop a source-file argument (does not begin with '-', ends in
             * ".c") -- libturi already provides its object.  Keep flags and
             * non-source paths (-I.../-L.../-lturi/-fsanitize=...). */
            bool is_c_source = tlen > 2 && start[0] != '-' &&
                               start[tlen - 2] == '.' && start[tlen - 1] == 'c';
            if (is_c_source) continue;
            if (filtered.len > 0) buf_putc(&filtered, ' ');
            buf_write(&filtered, start, tlen);
        }
        buf_putc(&filtered, '\0');
        buf_free(&autolink);
        autolink = filtered;
    }

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "%s %s -o %s %s", cc, cc_flags, out_path, tmpl);
    /* spices-c-sources-plan: vendored include dirs (-I) early so inline-C in
     * the generated TU can find its private headers; vendored sources before
     * the autolink/-lm flags so they can resolve against them. */
    if (aux_includes.len > 0) buf_puts(&cmd, aux_includes.data);
    if (aux_sources.len  > 0) buf_puts(&cmd, aux_sources.data);
    buf_free(&aux_includes);
    buf_free(&aux_sources);
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
    /* Always link libm last. Pure spices can reference libm symbols
     * (sqrt/fabs/sin/cos/...) directly from inline-C, and static spice deps
     * (e.g. plutovg) reference them without carrying -lm themselves. GNU ld
     * resolves archives left-to-right, so -lm must come AFTER any
     * -l<staticlib> flags or the math symbols go unresolved -- hence it is
     * appended last. On Linux an unused -lm is a harmless no-op; on macOS
     * libm lives in libSystem so -lm is a no-op there too. Linking it
     * unconditionally means libm is usable from a pure spice without forcing
     * a fake cmake-dep just to pull it in. */
    buf_puts(&cmd, " -lm");
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

static char **discover_manifest_reader_macros(const char *input_path,
                                              int *n_out) {
    *n_out = 0;
    if (!input_path) return NULL;
    char *sroot = find_spice_root(input_path);
    if (!sroot) return NULL;
    char mp[4096];
    if (!pkg_resolve_manifest_path(sroot, mp, sizeof(mp))) {
        free(sroot);
        return NULL;
    }
    PkgManifest m;
    memset(&m, 0, sizeof(m));
    char **out = NULL;
    if (pkg_manifest_read(mp, &m)) {
        apply_manifest_experiments(&m);
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
 * dropped at the first hop.  The visited set is path-keyed to dedupe shared
 * deps across multiple parents (e.g. httpd + template both pulling json) and
 * to prevent cycles.  Capacity grows as we go since transitive width is not
 * bounded by m->n_spices. */
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

static void collect_dep_dirs_recursive(const char *root, const PkgManifest *m,
                                       const char ***dirs, int *n, int *cap);

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
            dirs[n++] = strdup(own_src);
    }

    collect_dep_dirs_recursive(root, m, &dirs, &n, &cap);

    *out_dirs = dirs;
    *out_n    = n;
}

/* Walk every :spices entry in m, resolve its on-disk dir, push its src/ (or
 * dep_dir) into *dirs (deduped), then recurse into the dep's own manifest. */
static void collect_dep_dirs_recursive(const char *root, const PkgManifest *m,
                                       const char ***dirs, int *n, int *cap) {
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
                    grow_include_dirs(dirs, cap, *n + 1);
                    if (!include_dir_seen(*dirs, *n, sib_src))
                        (*dirs)[(*n)++] = strdup(sib_src);
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
                    grow_include_dirs(dirs, cap, *n + 1);
                    if (!include_dir_seen(*dirs, *n, sib_dir))
                        (*dirs)[(*n)++] = strdup(sib_dir);
                    strncpy(resolved_root, sib_dir, sizeof(resolved_root) - 1);
                    resolved_root[sizeof(resolved_root) - 1] = '\0';
                    chosen = NULL;
                    fallback_added = true;
                    break;
                }
            }
        }
        if (chosen) {
            grow_include_dirs(dirs, cap, *n + 1);
            if (!include_dir_seen(*dirs, *n, chosen))
                (*dirs)[(*n)++] = strdup(chosen);
        }

        /* Recurse into the dep's own manifest so its :spices contribute their
         * src/ too.  Use `resolved_root` (the real on-disk package root) for
         * manifest lookup; `dep_dir` may have :subdir appended to a :path,
         * yielding a non-existent path when both are set together. */
        if (!resolved_root[0]) continue;
        char dmp[4096];
        if (!pkg_resolve_manifest_path(resolved_root, dmp, sizeof(dmp))) continue;
        PkgManifest dm; memset(&dm, 0, sizeof(dm));
        if (!pkg_manifest_read(dmp, &dm)) continue;
        collect_dep_dirs_recursive(resolved_root, &dm, dirs, n, cap);
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
        if (_rc != 0) { unlink(out_path); for (int _i = 0; _i < n_spice_inc_dirs; _i++) free((char *)spice_inc_dirs[_i]); free(spice_inc_dirs); free(user_inc); free_reader_macro_paths(rm_paths_owned, n_rm_paths); for (int _i = 0; _i < n_auto_run_owned; _i++) free(auto_run_owned[_i]); \
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
     * see docs/upcoming/transitive-cmake-deps-plan.md. */
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

/* Type-check every .tur file under a directory (no codegen kept).
 * Mirrors cmd_test's discovery so `tur check src/` works inside a spice
 * without a per-file path. Resolves intra-spice include dirs from the
 * nearest build.tur, exactly like cmd_test. Returns 0 iff all files pass. */
static int cmd_check_dir(const char *dir) {
    int n_files = 0;
    char **tur_files = collect_tur_files(dir, &n_files);
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
     * the runtime functions that operate on those shared globals.  Linked into
     * the output below; cleaned up alongside _main.c. */
    {
        Buf rt_h; buf_init(&rt_h);
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

    /* See the note on -fno-strict-aliasing and -Wno-error=int-conversion above. */
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
    buf_puts(&cmd, " -lm");
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
    char **tur_files = collect_tur_files(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }
    char *build_dir = resolve_build_dir(dir, cli_build_dir);
    if (!build_dir) { free_tur_files(tur_files, n_files); return 2; }
    int rc = cmd_build_multi_files(tur_files, n_files, n_files, dir, NULL, NULL,
                                   out_path, shared, manifest_path, NULL, 0,
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
     * docs/archive/tur-build-cmake-deps-workspace-overreach.md) and gen +
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
 * Returns 0 on success with *out populated, -1 on error. */
static int fmt_format_source(const char *path_label, const char *src, size_t len,
                              ReaderType rtype, Buf *out) {
    /* Reset BEFORE registering: diag_reset() clears the file registry, so
     * registering first (as this used to) wiped this file's entry and left
     * any format-time parse-error diagnostic without a source snippet
     * (files_[0] == NULL -> "<unknown>"). */
    diag_reset();

    SourceFile file = {0};
    file.path        = path_label;
    file.src         = src;
    file.len         = len;
    file.file_id     = 0;
    file.reader_type = rtype;
    diag_register_file(&file);

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

static void wk_register_stdlib_natives(TuriEnv *env);
static void wk_register_hamt_natives(TuriEnv *env);
static void wk_register_map_natives(TuriEnv *env);
static void wk_register_sym_natives(TuriEnv *env);
static void wk_register_seq_natives(TuriEnv *env);
static void wk_register_json_natives(TuriEnv *env);
static void wk_register_schema_natives(TuriEnv *env);
static void wk_register_safe_natives(TuriEnv *env);
static void wk_register_typeclass_natives(TuriEnv *env);
static void wk_register_comonad_natives(TuriEnv *env);
static void wk_register_mutex_natives(TuriEnv *env);
static void wk_register_future_natives(TuriEnv *env);
static void wk_register_bytes_natives(TuriEnv *env);
static void wk_register_taskgroup_natives(TuriEnv *env);
static void wk_register_chan_natives(TuriEnv *env);
static void wk_register_backtrack_natives(TuriEnv *env);
static void wk_register_proc_fs_natives(TuriEnv *env);
static void wk_register_serial_natives(TuriEnv *env);

/* 3c: TUR_TURI_FULL_PRELUDE=1 opts the interpreter into loading the carved
 * stdlib modules (contract/mutmap/json/schema) on top of the default prelude.
 * Off by default; matches the `=1` convention used by TUR_TSAN. */
static bool turi_full_prelude_enabled(void) {
    const char *e = getenv("TUR_TURI_FULL_PRELUDE");
    return e && strcmp(e, "1") == 0;
}

/* Contract runtime helpers (defined below; forward-declared so cmd_eval can
 * register them as native overrides for contract.tur's inline-C bodies). */
static TuriValue native_contract_check(TuriEnv *env, TuriValue *args,
                                        uint32_t n, void *ud);
static TuriValue native_contract_check_inv(TuriEnv *env, TuriValue *args,
                                            uint32_t n, void *ud);
static TuriValue native_contract_enabled(TuriEnv *env, TuriValue *args,
                                          uint32_t n, void *ud);

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
            ReaderType rt = detect_lang(head, hn, &rest, &rest_len);
            if (rest != head && reader_type_is_implemented(rt))
                env->reader_type = rt;
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
     * fires and a user defmodule no longer conflicts with the preloaded one. */
    {
        char path_buf[4096];
        tur_stdlib_path("macros.tur", path_buf, sizeof(path_buf));
        char load_form[4200];
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", path_buf);
        TuriValue sv = turi_eval(env, load_form);
        (void)sv;
    }
    /* Runtime contracts: loaded right after macros.tur (and, like it, in its own
     * turi_eval so it lands at the very front of the accumulated source).
     * contract.tur exports macros (assert!/require!/ensure!/invariant! + their
     * -msg! variants); the Phase M7 promotion in elaborate_program nulls a
     * tur/-prefixed module's macros' defining_module_name -- making them
     * globally visible without an explicit import -- only for modules within
     * the stdlib-prefix boundary, which (because load-expansion shifts form
     * indices) effectively covers the earliest-loaded modules.  Loading
     * contract.tur up front, next to macros.tur, keeps assert!/require!/... in
     * that promoted region so user code sees them bare under --interpret.  Its
     * inline-C tur-contract-check / tur-contract-check-inv bodies are overridden
     * by native_contract_check[_inv] below; without this preload the elaborator
     * never sees a tur-contract-check binding and silently drops every
     * :pre/:post/:type check (a silent miscompile). */
    {
        char pb[4096];
        tur_stdlib_path("contract.tur", pb, sizeof(pb));
        char load_form[4200];
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", pb);
        TuriValue sv = turi_eval(env, load_form);
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
            /* vec operations.  TI8.b/W1: vec-get/vec-set!/vec-free are dropped
             * here -- the real vec.tur (preloaded below) defines them, and a
             * stub would collide with "already defined by an auto-loaded stdlib
             * module".  vec-new-filled is benchmark-only (no module defn). */
            "(defn vec-new-filled [n :int v :int] :int 0)\n"
            /* numeric helpers.  cstr->parse-int and int->float stubs were dropped:
             * both are native-backed (their natives resolve the bare call at
             * elaboration), so the stubs were redundant -- and a fixture that
             * (load ...)s str.tur / math.tur no longer collides with them via the
             * "already defined by an auto-loaded stdlib module" guard. */
            /* bit-shr / bit-xor stubs dropped: both are kind-preserving builtins
             * (builtins.c BITWISE_OPS, registered for every integer kind) AND
             * native-backed (native_bit_shr / native_bit_xor below), so the bare
             * call resolves at elaboration without a stub.  The :int-typed stubs
             * actually *shadowed* the kind-preserving builtins, breaking narrow-int
             * use (e.g. (bit-xor 65535u16 3855u16) -> "expected int, got uint16")
             * -- a divergence from the compiled path.  bit-and/bit-or/bit-shl were
             * never stubbed and worked on narrow types all along. */
            "(defn println-float [x :float d :int] :nil nil)\n"
            "(defn int->unit-float [x :int] :float 0.0)\n"
            "(defn tur-sqrt [x :float] :float 0.0)\n"
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
            /* none? predicate signature the elaborator needs so the native
             * types as :bool (not :int, which trips the strict "if condition
             * must be bool" check).  TI8.b/W1b: ok?/err?/some? are dropped here
             * because result.tur / option.tur (both preloaded below) define them
             * -- a stub would collide with the auto-loaded module defn.  none?
             * has no module defn, so its stub stays. */
            "(defn none? [r :int] :bool false)\n"
        );
        (void)sv;
    }
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
     * as the compiled path. */
    {
        /* W1/W1b conflict-free subset.  result.tur joined the prelude (W1b)
         * once three pieces landed: the dual-rep Result readers
         * (native_ok_val/...), native_result_eq (closure-caller), and the
         * EX_GET_FIELD carrier-box path in eval.c (so result.tur's field-access
         * accessors work on a Result that flowed as the :int carrier, e.g.
         * `(:: carrier (Result A B))`).  hamt.tur joined once cmd_eval started
         * registering the raw tur_hamt_* runtime wrappers (wk_register_hamt_-
         * natives, above) -- its ops are thin wrappers over those.
         *
         * contract.tur is preloaded too, but NOT in this array -- it is loaded up
         * front next to macros.tur (Phase M7 macro promotion is order-sensitive;
         * see that load site).  Its inline-C tur-contract-check /
         * tur-contract-check-inv bodies are overridden by native_contract_check
         * / native_contract_check_inv (registered with the other natives below),
         * so the :pre/:post/:type lowering and the assert!/require!/ensure!/
         * invariant! macros actually enforce under --interpret instead of being
         * silently dropped (the elaborator only injects a contract check when
         * the tur-contract-check binding is visible -- without the preload it was
         * unbound, so every :pre/:post became a no-op: a silent miscompile).
         * mutmap.tur is now preloaded (below): its inline-C ops are re-implemented
         * as native_mutmap_* overrides over its self-contained open-addressing
         * layout (no tur_hamt_* dependency).  See
         * docs/reported/turi-map-set-hamt-interpreter-gap.md. */
        static const char *prelude[] = {
            "safe.tur",
            "typeclass-eq.tur", "typeclass-functor.tur", "typeclass-clone.tur",
            "typeclass-hash.tur", "typeclass-applicative.tur",
            "typeclass-alternative.tur", "typeclass-monad.tur",
            "typeclass-monaderror.tur", "typeclass-bifunctor.tur",
            "hamt.tur", "set.tur", "map.tur",
            "vec.tur", "slice.tur", "option.tur", "result.tur",
            "pair.tur", "tuple.tur", "list.tur", "grid.tur", "zipper.tur",
            /* MutableMap: self-contained open-addressing table; its inline-C ops
             * are re-implemented as native overrides (native_mutmap_*). */
            "mutmap.tur",
            /* Uniqueness-type pattern forms (with-unique / consume / replace).
             * Pure-Turmeric wrappers over the ^unique discipline; the compiled
             * path auto-loads it (g_stdlib_autoload_files), so the interpreter
             * preloads it too or those forms read as unbound under --interpret. */
            "unique.tur",
            NULL
        };
        /* Build one (load A)(load B)... form so the whole prelude elaborates in
         * a single turi_eval call -- distinct file_ids, deps resolved in order. */
        Buf src; buf_init(&src);
        for (int i = 0; prelude[i] != NULL; i++) {
            char pb[4096];
            tur_stdlib_path(prelude[i], pb, sizeof(pb));
            buf_write(&src, "(load \"", 7);
            buf_write(&src, pb, strlen(pb));
            buf_write(&src, "\")\n", 3);
        }
        char pb[4096];
        tur_stdlib_path("sym.tur", pb, sizeof(pb));
        buf_write(&src, "(load \"", 7);
        buf_write(&src, pb, strlen(pb));
        buf_write(&src, "\")\n", 3);
        buf_putc(&src, '\0');  /* turi_eval calls strlen; NUL-terminate */
        TuriValue sv = turi_eval(env, src.data);
        (void)sv;
        buf_free(&src);
    }
    /* 3c (turi-open-reports-prereqs.md): opt-in full prelude.  The default
     * prelude above covers the typed-stdlib core; when TUR_TURI_FULL_PRELUDE=1
     * the interpreter ADDITIONALLY loads the modules the compiled path
     * auto-loads but the interpreter carves out
     * (docs/artifacts/turi-preload-carve-out.txt): json, schema.  This makes the
     * interpreter prelude match the compiled auto-load set so the carved bucket
     * can be iterated/measured fixture-by-fixture under --interpret, without
     * committing the extra parse/elab cost to every run.  Off by default; loaded
     * BEFORE the native overrides so those still win, and each module is loaded
     * in its own turi_eval so one failing module does not block the rest.
     * (contract.tur and mutmap.tur graduated to the default prelude above.) */
    if (turi_full_prelude_enabled()) {
        static const char *full_extra[] = {
            "json.tur", "schema.tur", NULL
        };
        for (int i = 0; full_extra[i] != NULL; i++) {
            char pb[4096];
            tur_stdlib_path(full_extra[i], pb, sizeof(pb));
            Buf src; buf_init(&src);
            buf_write(&src, "(load \"", 7);
            buf_write(&src, pb, strlen(pb));
            buf_write(&src, "\")\n", 3);
            buf_putc(&src, '\0');
            TuriValue sv = turi_eval(env, src.data);
            (void)sv;
            buf_free(&src);
        }
    }
    /* JR0 (turi-json-schema-interpreter-plan, Layer 1): auto-load json.tur so
     * the #json(...) reader-macro lowering's json node constructors (and
     * json/encode|decode|type|get accessors) resolve under --interpret.  The
     * loaded inline-C bodies are overridden by the layout-exact natives
     * registered in wk_register_json_natives below.  Skipped when the opt-in
     * full prelude already loaded json.tur (avoids a redefinition). */
    if (!turi_full_prelude_enabled()) {
        char pb[4096];
        tur_stdlib_path("json.tur", pb, sizeof(pb));
        char load_form[4200];
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", pb);
        TuriValue sv = turi_eval(env, load_form);
        (void)sv;
    }
    /* RD (Layer 2): auto-load schema.tur on top of json.tur so the
     * #json-str<T>(...) typed decode reader family resolves; the inline-C
     * schema engine is overridden by wk_register_schema_natives below.  Skipped
     * when the full prelude already loaded schema.tur. */
    if (!turi_full_prelude_enabled()) {
        char pb[4096];
        tur_stdlib_path("schema.tur", pb, sizeof(pb));
        char load_form[4200];
        snprintf(load_form, sizeof(load_form), "(load \"%s\")", pb);
        TuriValue sv = turi_eval(env, load_form);
        (void)sv;
    }
    /* Register native overrides for stdlib inline-C functions. */
    wk_register_stdlib_natives(env);
    /* Contract runtime helpers: override contract.tur's inline-C
     * tur-contract-check / tur-contract-check-inv (which the simple inline-C
     * executor cannot run -- they call tur_panic) so the :pre/:post/:type
     * lowering and the assert!/require!/ensure!/invariant! macros actually
     * panic on a violated contract under --interpret.  Mirrors wk_eval_fixture. */
    turi_env_register_native(env, "tur-contract-check",
                             native_contract_check, NULL);
    turi_env_register_native(env, "tur-contract-check-inv",
                             native_contract_check_inv, NULL);
    turi_env_register_native(env, "contract-enabled?",
                             native_contract_enabled, NULL);
    /* W1b/Gap1: register the raw tur_hamt_* runtime wrappers so hamt.tur (and
     * the map/set layers built on it) work under --interpret.  Previously only
     * wk_eval_fixture did this, so `tur --interpret` on any hamt-using program
     * silently got the no-op stubs (tur_hamt_new -> 0). */
    wk_register_hamt_natives(env);
    /* TI10 Tier A: typed Map[K V] scalar-key natives (MapKey/Hash instance
     * comparators + the raw map-*-eq-o bridges over tur_hamt_*_eq_o). */
    wk_register_map_natives(env);
    /* R1 (turi-interpret-flip-residual-plan): safe.tur box/unbox/array-get/-set
     * over the int64-carrier layout, and the typeclass instance-method overrides
     * (Show/Eq inline-C bodies the tree-walker cannot run).  Previously only
     * wk_eval_fixture registered these, so `tur --interpret` on a program using
     * (box ...)/(unbox ...) or a stdlib Eq/Show instance hit "inline-C not
     * supported". */
    wk_register_safe_natives(env);
    wk_register_typeclass_natives(env);
    /* R1: comonad.tur Identity/Pair cell accessors (loaded on demand by user
     * code; the natives override the inline-C bodies). */
    wk_register_comonad_natives(env);
    /* R1: mutex.tur pthread handle ops (loaded on demand). */
    wk_register_mutex_natives(env);
    /* R1: future.tur refcounted FutureCell + serial.tur Bytes (loaded on demand). */
    wk_register_future_natives(env);
    wk_register_bytes_natives(env);
    /* R1: taskgroup.tur TaskGroupBlock handle ops (loaded on demand). */
    wk_register_taskgroup_natives(env);
    /* R1: chan.tur bounded sync/async channel ops (loaded on demand). */
    wk_register_chan_natives(env);
    /* R3: backtrack.tur cons-stream monad primitives (loaded on demand). */
    wk_register_backtrack_natives(env);
    /* R1: process.tur spawn/wait + fs.tur tmpfile OS-handle ops (loaded on demand). */
    wk_register_proc_fs_natives(env);
    /* R1: serial.tur Serializable int/bool instances (loaded on demand). */
    wk_register_serial_natives(env);
    /* SYM (turi): first-class :Sym ops over the auto-loaded sym.tur. */
    wk_register_sym_natives(env);
    /* SEQ (turi): lazy-Seq bridges over turi_call + generator advance.  The seq
     * modules are loaded on demand by user code; the natives override the
     * inline-C bodies. */
    wk_register_seq_natives(env);
    /* JSON (turi): tagged-AST JSON engine over layout-exact node structs,
     * overriding json.tur's malloc/recurse inline-C bodies (auto-loaded under
     * -Xjson-reader, or via an explicit (load "stdlib/json.tur")). */
    wk_register_json_natives(env);
    /* SCHEMA (turi): runtime schema validator over the JSON nodes, overriding
     * schema.tur's inline-C constructors/decoder/accessors (Layer 2). */
    wk_register_schema_natives(env);
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
    TuriValue result;
    if (debug) {
        /* Under the debugger the user file is brought in via `(load "path")`
         * rather than turi_eval_file's concatenate-into-<eval> path: a loaded
         * file gets its own file_id and keeps its real path + 1-based line
         * numbers, so breakpoints (`break <line>`), source listings, and stack
         * frames resolve against the user's source instead of the synthetic
         * <eval> blob (which would offset every line by the preloaded prelude). */
        char load_form[4200];
        snprintf(load_form, sizeof load_form, "(load \"%s\")", path);
        result = turi_eval(env, load_form);
    } else {
        result = turi_eval_file(env, path);
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
        else if (strcmp(tok, "--dump-effects")      == 0) g_dump_effects             = true;
        else if (strcmp(tok, "--dump-cps-coloring") == 0) g_dump_cps_coloring        = true;
        else if (strcmp(tok, "--dump-cps")          == 0) g_dump_cps                 = true;
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
        else if (strcmp(tok, "--allow-experimental") == 0) g_allow_experimental = true;
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
            if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
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
/* W1b: an Option reaches these shims either as a native int64[2] box
 * {is_some, value} (from native_some / tur_box_some) or as a make-struct TuriStruct
 * with the same field order (from `(make-struct Option ...)`).  option_field
 * reads field `idx` from whichever representation so the two coexist -- the same
 * dual-rep pattern as result_field.  none is the 0/NULL box (every field 0). */
static TuriValue option_field(TuriValue o, int idx) {
    bool found = false;
    TuriValue f = turi_struct_field(o, (uint32_t)idx, &found);
    if (found) return f;
    int64_t *p = (int64_t *)(intptr_t)o.as_int;
    return turi_int(p ? p[idx] : 0);
}
static int64_t option_field_int(TuriValue o, int idx) {
    TuriValue f = option_field(o, idx);
    return (f.tag == TURI_BOOL) ? (f.as_bool ? 1 : 0) : f.as_int;
}
static bool option_is_some(TuriValue o) {
    if (o.tag != TURI_STRUCT && o.as_int == 0) return false;  /* none */
    return option_field_int(o, 0) != 0;
}
/* option-eq? -- structural Option equality.  option.tur's body fat-dispatches
 * the value comparator through a C function pointer; this native re-implements
 * it over either Option representation (option_field) and invokes the comparator
 * (a turi closure) via turi_call. */
static TuriValue native_option_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    bool a_some = option_is_some(a[0]);
    bool b_some = option_is_some(a[1]);
    if (!a_some && !b_some) return turi_bool(true);
    if (a_some != b_some)   return turi_bool(false);
    TuriValue cargs[2];
    cargs[0].tag = TURI_INT; cargs[0].as_int = option_field_int(a[0], 1);
    cargs[1].tag = TURI_INT; cargs[1].as_int = option_field_int(a[1], 1);
    TuriValue rv = turi_call(env, a[2], cargs, 2);
    if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
    return turi_bool(rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0);
}
static TuriValue native_some_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    return turi_bool(option_is_some(a[0]));
}
static TuriValue native_option_unwrap(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    if (!option_is_some(a[0])) { fprintf(stderr, "unwrap called on none\n"); return turi_int(0); }
    return turi_int(option_field_int(a[0], 1));
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
    if (n < 1 || !option_is_some(a[0])) {
        /* Catchable panic (recoverable by catch-unwind) instead of _exit(1). */
        turi_runtime_panic(env, "option-must: called on none");
        return turi_nil(); /* unreachable */
    }
    return turi_int(option_field_int(a[0], 1));
}
static TuriValue native_option_expect(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    const char *msg = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "option-expect: called on none";
    if (n < 1 || !option_is_some(a[0])) {
        turi_runtime_panic(env, msg);
        return turi_nil(); /* unreachable */
    }
    return turi_int(option_field_int(a[0], 1));
}
static TuriValue native_option_unwrap_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    if (!option_is_some(a[0])) return turi_int(a[1].as_int);
    return turi_int(option_field_int(a[0], 1));
}
/* option-map -- apply f to the some value, returning a new Option.  option.tur's
 * body fat-dispatches f via TUR_APPLY1; this native invokes f (a turi closure)
 * via turi_call and returns a fresh native int64[2] {is_some, value} box. */
static TuriValue native_option_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    if (!option_is_some(a[0])) return turi_int(0);
    TuriValue arg; arg.tag = TURI_INT; arg.as_int = option_field_int(a[0], 1);
    TuriValue rv = turi_call(env, a[1], &arg, 1);
    if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
    int64_t *r = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!r) return turi_int(0);
    r[0] = 1; r[1] = rv.as_int;
    return turi_int((int64_t)(intptr_t)r);
}

/* Result functions: { bool is_ok (offset 0); int64_t ok_val (offset 8); int64_t err_val (offset 16) }
 * Stored as int64_t[3]: [0]=is_ok, [1]=ok_val, [2]=err_val
 *
 * R5 (turi-interpret-flip-residual-plan): the int64 box flattens the payload to
 * a bare int64, which loses the tag of a *heap* payload (a make-struct User, a
 * cstr, a closure) -- ok-val then hands back a TURI_INT and a downstream field
 * access / println reads garbage (the value-struct-payload silent miscompile).
 * So when the payload is a heap value, build a make-struct Result instead, whose
 * fields hold the full TuriValue (tag preserved); int/bool payloads keep the box
 * (no change to the carrier-ABI fixtures that depend on it). result_field reads
 * both reps, so ok?/err?/ok-val/err-val/result-eq stay uniform. */
static bool wk_result_payload_is_heap(TuriValue v) {
    return v.tag == TURI_STRUCT || v.tag == TURI_CSTR ||
           v.tag == TURI_CLOSURE || v.tag == TURI_FLOAT;
}
static TuriValue native_ok(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    TuriValue payload = (n > 0) ? a[0] : turi_int(0);
    if (wk_result_payload_is_heap(payload)) {
        TuriValue fields[3] = { turi_bool(true), payload, turi_int(0) };
        return turi_make_struct(env, "Result", fields, 3);
    }
    int64_t *r = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!r) return turi_nil();
    r[0] = 1; r[1] = payload.as_int; r[2] = 0;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
static TuriValue native_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    TuriValue payload = (n > 0) ? a[0] : turi_int(0);
    if (wk_result_payload_is_heap(payload)) {
        TuriValue fields[3] = { turi_bool(false), turi_int(0), payload };
        return turi_make_struct(env, "Result", fields, 3);
    }
    int64_t *r = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!r) return turi_nil();
    r[0] = 0; r[1] = 0; r[2] = payload.as_int;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)r; return v;
}
/* W1b: a Result reaches these shims either as a native int64[3] box
 * {is_ok, ok_val, err_val} (from native_ok/err) or as a make-struct TuriStruct
 * with the same field order (from `(make-struct Result ...)`).  These helpers
 * read field `idx` from whichever representation so the two coexist -- one of
 * the three pieces (with native_result_eq and the EX_GET_FIELD carrier path)
 * that let result.tur join the prelude. */
static TuriValue result_field(TuriValue r, int idx) {
    bool found = false;
    TuriValue f = turi_struct_field(r, (uint32_t)idx, &found);
    if (found) return f;
    int64_t *p = (int64_t *)(intptr_t)r.as_int;
    return turi_int(p ? p[idx] : 0);
}
static int64_t result_field_int(TuriValue r, int idx) {
    TuriValue f = result_field(r, idx);
    return (f.tag == TURI_BOOL) ? (f.as_bool ? 1 : 0) : f.as_int;
}
/* True if the Result carries the ok tag (dual-rep: TuriStruct or native box). */
static bool result_is_ok(TuriValue r) { return result_field_int(r, 0) != 0; }
/* Construct a Result from an is_ok flag + payload, dual-rep-correct: a heap
 * payload becomes a make-struct Result, a scalar a native int64[3] box -- exactly
 * native_ok/native_err's logic, so the reading shims (result_field) round-trip it
 * and a struct ok/err value keeps its tag instead of being flattened to int. */
static TuriValue result_box_make(TuriEnv *env, bool is_ok, TuriValue payload) {
    TuriValue arg[1] = { payload };
    return is_ok ? native_ok(env, arg, 1, NULL) : native_err(env, arg, 1, NULL);
}
static TuriValue native_ok_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(false);
    if (a[0].tag != TURI_STRUCT && a[0].as_int == 0) return turi_bool(false);
    return turi_bool(result_field_int(a[0], 0) != 0);
}
static TuriValue native_err_pred(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_bool(true);
    if (a[0].tag != TURI_STRUCT && a[0].as_int == 0) return turi_bool(true);
    return turi_bool(result_field_int(a[0], 0) == 0);
}
static TuriValue native_ok_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return result_field(a[0], 1);
}
static TuriValue native_err_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return result_field(a[0], 2);
}
static TuriValue native_result_unwrap_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    if (result_is_ok(a[0])) return result_field(a[0], 1);
    return a[1];
}
/* ok-val-ptr: get ok_val as a pointer (void*) */
static TuriValue native_ok_val_ptr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    return result_field(a[0], 1);
}
/* err-val-ptr: get err_val as a pointer (void*) */
static TuriValue native_err_val_ptr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    return result_field(a[0], 2);
}
/* result-map: apply Turmeric fn to ok value, return new result */
static TuriValue native_result_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (!result_is_ok(a[0])) return a[0]; /* return err unchanged */
    TuriValue arg = result_field(a[0], 1);
    TuriValue res = turi_call(env, a[1], &arg, 1);
    if (turi_is_error(res) || env->throwing) return res;  /* propagate callback error */
    return result_box_make(env, true, res);
}
/* W1b: result-eq? -- (result-eq? r1 r2 ok-cmp err-cmp).  result.tur's version is
 * inline-C that fat-dispatches the two comparison closures, which the simple
 * inline-C evaluator cannot run; this native invokes them via turi_call so
 * typed/result-basic and friends work under --interpret.  Reads both Results
 * dual-rep (make-struct TuriStruct or native box) via result_field. */
static TuriValue native_result_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    int64_t a_ok = result_field_int(a[0], 0);
    int64_t b_ok = result_field_int(a[1], 0);
    if ((a_ok != 0) != (b_ok != 0)) return turi_bool(false);
    int idx = a_ok ? 1 : 2;                /* compare ok-vals or err-vals */
    TuriValue cmp = a_ok ? a[2] : a[3];    /* ok-cmp or err-cmp */
    TuriValue cargs[2] = { result_field(a[0], idx), result_field(a[1], idx) };
    TuriValue res = turi_call(env, cmp, cargs, 2);
    if (turi_is_error(res) || env->throwing) return res;  /* propagate callback error */
    return turi_bool(res.tag == TURI_BOOL ? res.as_bool : res.as_int != 0);
}
/* result-map-err: apply fn to err value, return new result */
static TuriValue native_result_map_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (result_is_ok(a[0])) return a[0]; /* return ok unchanged */
    TuriValue arg = result_field(a[0], 2);
    TuriValue res = turi_call(env, a[1], &arg, 1);
    if (turi_is_error(res) || env->throwing) return res;  /* propagate callback error */
    return result_box_make(env, false, res);
}
/* result-flat-map: apply fn to ok value (fn returns a result), flatMap */
static TuriValue native_result_flat_map(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (!result_is_ok(a[0])) return a[0]; /* return err unchanged */
    TuriValue arg = result_field(a[0], 1);
    return turi_call(env, a[1], &arg, 1);
}
/* result-or: return self if ok, else return alt */
static TuriValue native_result_or(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_nil();
    if (result_is_ok(a[0])) return a[0];
    return a[1];
}
/* result-or-else: return self if ok, else call f(err_val) */
static TuriValue native_result_or_else(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    if (result_is_ok(a[0])) return a[0];
    TuriValue arg = result_field(a[0], 2);
    return turi_call(env, a[1], &arg, 1);
}
/* Display helpers */
static TuriValue native_result_display(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("err");
    return turi_cstr(result_is_ok(a[0]) ? "ok" : "err");
}
static TuriValue native_result_debug(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("Result::Err");
    return turi_cstr(result_is_ok(a[0]) ? "Result::Ok" : "Result::Err");
}
static TuriValue native_result_error_message(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_cstr("result is err");
    return turi_cstr(result_is_ok(a[0]) ? "result is ok" : "result is err");
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
    if (!result_is_ok(a[0])) { fprintf(stderr, "unwrap called on err\n"); return turi_int(0); }
    return result_field(a[0], 1);
}
static TuriValue native_result_unwrap_err(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return result_field(a[0], 2);
}
static TuriValue native_result_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0) { void *p = (void *)(intptr_t)a[0].as_int; if (p) free(p); }
    return turi_nil();
}
static TuriValue native_result_must(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1 || !result_is_ok(a[0])) {
        /* Catchable panic (recoverable by catch-unwind) instead of _exit(1). */
        turi_runtime_panic(env, "result-must: called on err");
        return turi_nil(); /* unreachable */
    }
    return result_field(a[0], 1);
}
static TuriValue native_result_must_msg(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    const char *msg = (n > 1 && a[1].tag == TURI_CSTR && a[1].as_cstr) ? a[1].as_cstr : "result-must-msg: called on err";
    if (n < 1 || !result_is_ok(a[0])) {
        turi_runtime_panic(env, msg);
        return turi_nil(); /* unreachable */
    }
    return result_field(a[0], 1);
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
 * Set operations -- W1b: a Set is set.tur's `struct { void *hamt; }` (one
 * pointer = the persistent HAMT), matching the compiled representation.  These
 * native overrides invoke the real runtime HAMT (the same `tur_hamt_*` the
 * compiled set ops call), so `set-new`/`add`/`count`/... work under --interpret.
 * Element keys use the int-keyed HAMT API (value == key == hash for the caller-
 * supplied `h`); content-keyed sets (needing a C eq callback) remain a gap.
 * ---------------------------------------------------------------------- */
static Hamt *set_hamt(TuriValue v) {
    if (v.tag != TURI_INT || v.as_int == 0) return NULL;
    return (Hamt *)((void **)(intptr_t)v.as_int)[0];
}
static TuriValue set_wrap(Hamt *h) {
    void **s = (void **)malloc(sizeof(void *));
    if (!s) return turi_nil();
    s[0] = (void *)h;
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)s; return v;
}
static TuriValue native_set_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    return set_wrap(tur_hamt_new());
}
/* (set-add s h x) -- persistent insert; returns a new set. */
static TuriValue native_set_add(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return n >= 1 ? a[0] : turi_nil();
    Hamt *r = tur_hamt_set(set_hamt(a[0]), (uint64_t)a[1].as_int,
                           (void *)(intptr_t)a[2].as_int, (void *)1);
    return set_wrap(r);
}
/* (set-remove s h x) -- persistent delete; returns a new set. */
static TuriValue native_set_remove(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return n >= 1 ? a[0] : turi_nil();
    Hamt *r = tur_hamt_del(set_hamt(a[0]), (uint64_t)a[1].as_int,
                           (void *)(intptr_t)a[2].as_int);
    return set_wrap(r);
}
/* (set-member? s h x) -- membership test. */
static TuriValue native_set_member(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_bool(false);
    return turi_bool(tur_hamt_has(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                 (void *)(intptr_t)a[2].as_int));
}
static TuriValue native_set_count(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)tur_hamt_count(set_hamt(a[0])));
}
static TuriValue native_set_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].tag != TURI_INT || a[0].as_int == 0) return turi_nil();
    tur_hamt_free(set_hamt(a[0]));
    free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}
/* set-eq-cmp? -- O(n*m) structural set equality with a user element comparator.
 * set.tur's body double-iterates the HAMTs and fat-dispatches cmp-fn through a C
 * function pointer (un-runnable in the simple inline-C executor); this native
 * re-implements it, invoking the comparator (a turi closure under --interpret)
 * via turi_call.  Elements are stored as the HAMT key (set-add s h x). */
static TuriValue native_set_eq_cmp(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    Hamt *sa = set_hamt(a[0]), *sb = set_hamt(a[1]);
    TuriValue cmp = a[2];
    if (tur_hamt_count(sa) != tur_hamt_count(sb)) return turi_bool(false);
    uint64_t iter_a[32]; for (int i = 0; i < 32; i++) iter_a[i] = 0;
    tur_hamt_iter_init((HamtIter *)iter_a, sa);
    uint64_t ha; void *ka = NULL, *vap = NULL;
    bool ok = true;
    while (ok && tur_hamt_iter_next((HamtIter *)iter_a, &ha, &ka, &vap)) {
        bool found = false;
        uint64_t iter_b[32]; for (int i = 0; i < 32; i++) iter_b[i] = 0;
        tur_hamt_iter_init((HamtIter *)iter_b, sb);
        uint64_t hb; void *kb = NULL, *vbp = NULL;
        while (tur_hamt_iter_next((HamtIter *)iter_b, &hb, &kb, &vbp)) {
            TuriValue cargs[2];
            cargs[0].tag = TURI_INT; cargs[0].as_int = (int64_t)(intptr_t)ka;
            cargs[1].tag = TURI_INT; cargs[1].as_int = (int64_t)(intptr_t)kb;
            TuriValue rv = turi_call(env, cmp, cargs, 2);
            if (turi_is_error(rv) || env->throwing) {  /* propagate callback error */
                tur_hamt_iter_free((HamtIter *)iter_b);
                tur_hamt_iter_free((HamtIter *)iter_a);
                return rv;
            }
            if (rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0) { found = true; break; }
        }
        tur_hamt_iter_free((HamtIter *)iter_b);
        if (!found) ok = false;
    }
    tur_hamt_iter_free((HamtIter *)iter_a);
    return turi_bool(ok);
}
static TuriValue native_set_union(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return n >= 1 ? a[0] : turi_nil();
    return set_wrap(tur_hamt_merge(set_hamt(a[0]), set_hamt(a[1])));
}
/* set-intersect / set-diff iterate `a` and keep keys (present | absent) in `b`. */
static TuriValue set_iter_filter(TuriValue *a, bool keep_if_in_b) {
    Hamt *ha = set_hamt(a[0]), *hb = set_hamt(a[1]);
    Hamt *result = tur_hamt_new();
    uint64_t iter_buf[32]; for (int i = 0; i < 32; i++) iter_buf[i] = 0;
    tur_hamt_iter_init((HamtIter *)iter_buf, ha);
    uint64_t h; void *key = NULL, *val = NULL;
    while (tur_hamt_iter_next((HamtIter *)iter_buf, &h, &key, &val)) {
        if (tur_hamt_has(hb, h, key) == keep_if_in_b)
            result = tur_hamt_set(result, h, key, (void *)1);
    }
    tur_hamt_iter_free((HamtIter *)iter_buf);
    return set_wrap(result);
}
static TuriValue native_set_intersect(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud; if (n < 2) return turi_nil();
    return set_iter_filter(a, true);
}
static TuriValue native_set_diff(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud; if (n < 2) return turi_nil();
    return set_iter_filter(a, false);
}
static TuriValue native_set_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_bool(false);
    Hamt *ha = set_hamt(a[0]), *hb = set_hamt(a[1]);
    if (tur_hamt_count(ha) != tur_hamt_count(hb)) return turi_bool(false);
    uint64_t iter_buf[32]; for (int i = 0; i < 32; i++) iter_buf[i] = 0;
    tur_hamt_iter_init((HamtIter *)iter_buf, ha);
    uint64_t h; void *key = NULL, *val = NULL; bool eq = true;
    while (tur_hamt_iter_next((HamtIter *)iter_buf, &h, &key, &val)) {
        if (!tur_hamt_has(hb, h, key)) { eq = false; break; }
    }
    tur_hamt_iter_free((HamtIter *)iter_buf);
    return turi_bool(eq);
}

/* -------------------------------------------------------------------------
 * Typed Map[K V] natives (TI10 Tier A -- scalar key types).
 *
 * A Map[K V] is the same {void* hamt} carrier as a Set, so set_hamt/set_wrap
 * are reused.  The public map-* accessors are macros that expand to:
 *   (map-assoc-eq-o (kcheck m) (hash k) (mk-box k) v (mk-cmp k) (mk-owned? k))
 * so unblocking map under --interpret needs natives for (a) the MapKey/Hash
 * instance methods whose inline-C bodies the tree-walker can't run, and (b) the
 * raw map-*-eq-o bridges + map-count/merge/free over the runtime HAMT.
 *
 * mk-cmp returns the *address of a C carrier comparator* -- exactly what the
 * compiled path does -- and the runtime calls it through bool(*)(int64,int64)
 * only on a 64-bit hash collision.  mk-box / mk-cmp / hash for int (and bool,
 * which boxes to int) are plain (non-inline-C) bodies the interpreter already
 * evaluates, so only the cstr / float / float32 instances need natives here.
 * Owned (boxed multi-word) keys are out of scope (owned == 0 for all scalars).
 * ---------------------------------------------------------------------- */

/* Carrier comparators -- passed by address to tur_hamt_*_eq_o; mirror map.tur. */
static bool turi_int_carrier_eq_c(int64_t a, int64_t b) { return a == b; }
static bool turi_cstr_key_eq_c(int64_t a, int64_t b) {
    const char *p = (const char *)(intptr_t)a;
    const char *q = (const char *)(intptr_t)b;
    if (p == q) return true;
    if (!p || !q) return false;
    return strcmp(p, q) == 0;
}
static bool turi_f32_carrier_eq_c(int64_t a, int64_t b) {
    union { float f; int64_t i; } ua, ub; ua.i = a; ub.i = b; return ua.f == ub.f;
}
static bool turi_f64_carrier_eq_c(int64_t a, int64_t b) {
    union { double f; int64_t i; } ua, ub; ua.i = a; ub.i = b; return ua.f == ub.f;
}

/* mk-cmp [K] -- return the carrier comparator address as the int64 carrier. */
static TuriValue native_mk_cmp_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_int_carrier_eq_c);
}
static TuriValue native_mk_cmp_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_cstr_key_eq_c);
}
static TuriValue native_mk_cmp_f32(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_f32_carrier_eq_c);
}
static TuriValue native_mk_cmp_f64(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)&turi_f64_carrier_eq_c);
}

/* mk-box [K] -- box the key into the int64 carrier word. */
static TuriValue native_mk_box_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t carrier = a[0].tag == TURI_CSTR
                          ? (int64_t)(intptr_t)a[0].as_cstr : a[0].as_int;
    return turi_int(carrier);
}
static TuriValue native_mk_box_f32(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { float f; int64_t i; } u; u.i = 0; u.f = (float)a[0].as_float;
    return turi_int(u.i);
}
static TuriValue native_mk_box_f64(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { double f; int64_t i; } u; u.f = a[0].as_float; return turi_int(u.i);
}

/* hash [K] -- content hash matching stdlib/typeclass-hash.tur. */
static TuriValue native_hash_cstr(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    const char *s = a[0].tag == TURI_CSTR ? a[0].as_cstr
                                          : (const char *)(intptr_t)a[0].as_int;
    return turi_int((int64_t)tur_hamt_hash_str(s ? s : ""));
}
static TuriValue native_hash_f32(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { float f; int32_t i; } u; u.f = (float)a[0].as_float;
    return turi_int((int64_t)u.i);
}
static TuriValue native_hash_f64(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    union { double f; int64_t i; } u; u.f = a[0].as_float; return turi_int(u.i);
}

/* Raw map bridges over the content-keyed HAMT.  The carrier shares Set's layout
 * so set_hamt/set_wrap apply.  keyeq is the comparator address mk-cmp returned;
 * owned is 0 for scalar keys. */

/* TI10 Tier B -- turi-closure-aware key comparator.
 *
 * A content-keyed map whose MapKey instance is written in Turmeric (not inline-C)
 * has `mk-cmp` return a turi *closure*, not a C function-pointer address. That
 * closure cannot be cast to bool(*)(int64,int64) and handed to the runtime HAMT.
 * The 2a `ctx`-carrying HAMT entry points (tur_hamt_*_eq_ctx) close this: we
 * pack {env, comparator-closure} into a ctx word and pass a C trampoline that,
 * on each collision-time compare, calls the closure via turi_call with the two
 * carrier words as :int args.  This is the map analogue of native_result_eq.
 *
 * The trampoline is only valid for a comparator whose carrier words are directly
 * comparable as interpreter values (e.g. a one-word scalar carrier with custom
 * equality logic). A boxed/owned key whose comparator dereferences the box is an
 * inline-C comparator and never reaches here as a runnable turi closure. */
typedef struct { TuriEnv *env; TuriValue cmp; } MapTuriEqCtx;

static bool map_turi_eq_tramp(int64_t a, int64_t b, void *vctx) {
    MapTuriEqCtx *c = (MapTuriEqCtx *)vctx;
    /* If a prior comparison in this HAMT op already raised, stop calling the
     * closure -- the enclosing native's result is discarded once the driver
     * sees env->throwing. */
    if (c->env->throwing) return false;
    TuriValue args[2] = { turi_int(a), turi_int(b) };
    TuriValue r = turi_call(c->env, c->cmp, args, 2);
    if (turi_is_error(r) || c->env->throwing) {
        /* This comparator returns a C bool to the HAMT, so it cannot return the
         * error value; promote a value-level error to a throw so the enclosing
         * native (and the driver) propagate it instead of silently mis-hashing. */
        if (turi_is_error(r)) { c->env->throwing = true; c->env->throw_value = r; }
        return false;
    }
    if (r.tag == TURI_BOOL) return r.as_bool;
    if (r.tag == TURI_INT)  return r.as_int != 0;
    return false;
}

static TuriValue native_map_assoc_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key val keyeq owned) */
    if (n < 6) return n >= 1 ? a[0] : turi_nil();
    if (a[4].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[4] };
        Hamt *r = tur_hamt_set_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      (void *)(intptr_t)a[3].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap(r);
    }
    Hamt *r = tur_hamt_set_eq_o(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int,
                                (void *)(intptr_t)a[3].as_int,
                                (tur_hamt_keyeq_fn)(intptr_t)a[4].as_int,
                                (int64_t)a[5].as_int);
    return set_wrap(r);
}
static TuriValue native_map_get_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key keyeq owned) */
    if (n < 5) return turi_int(0);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        void *v = tur_hamt_get_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return turi_int((int64_t)(intptr_t)v);
    }
    void *v = tur_hamt_get_eq_o(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int,
                                (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                (int64_t)a[4].as_int);
    return turi_int((int64_t)(intptr_t)v);
}
static TuriValue native_map_has_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 5) return turi_bool(false);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        return turi_bool(tur_hamt_has_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                             (void *)(intptr_t)a[2].as_int,
                                             map_turi_eq_tramp, &ctx));
    }
    return turi_bool(tur_hamt_has_eq_o(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                       (void *)(intptr_t)a[2].as_int,
                                       (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                       (int64_t)a[4].as_int));
}
static TuriValue native_map_dissoc_eq_o(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 5) return n >= 1 ? a[0] : turi_nil();
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        Hamt *r = tur_hamt_del_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap(r);
    }
    Hamt *r = tur_hamt_del_eq_o(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                (void *)(intptr_t)a[2].as_int,
                                (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int,
                                (int64_t)a[4].as_int);
    return set_wrap(r);
}
/* Explicit-hash content API (map-*-eq, no ownership flag) over tur_hamt_*_eq.
 * The comparator is mk-cmp's C address; the key is raw (:K, == carrier for
 * scalars).  A forced-collision test drives these with a controlled hash. */
static TuriValue native_map_assoc_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key val keyeq) */
    if (n < 5) return n >= 1 ? a[0] : turi_nil();
    if (a[4].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[4] };
        Hamt *r = tur_hamt_set_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      (void *)(intptr_t)a[3].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap(r);
    }
    Hamt *r = tur_hamt_set_eq(set_hamt(a[0]), (uint64_t)a[1].as_int,
                              (void *)(intptr_t)a[2].as_int,
                              (void *)(intptr_t)a[3].as_int,
                              (tur_hamt_keyeq_fn)(intptr_t)a[4].as_int);
    return set_wrap(r);
}
static TuriValue native_map_get_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    /* (m h key keyeq) */
    if (n < 4) return turi_int(0);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        void *v = tur_hamt_get_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return turi_int((int64_t)(intptr_t)v);
    }
    void *v = tur_hamt_get_eq(set_hamt(a[0]), (uint64_t)a[1].as_int,
                              (void *)(intptr_t)a[2].as_int,
                              (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int);
    return turi_int((int64_t)(intptr_t)v);
}
static TuriValue native_map_has_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        return turi_bool(tur_hamt_has_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                             (void *)(intptr_t)a[2].as_int,
                                             map_turi_eq_tramp, &ctx));
    }
    return turi_bool(tur_hamt_has_eq(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                     (void *)(intptr_t)a[2].as_int,
                                     (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int));
}
static TuriValue native_map_dissoc_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return n >= 1 ? a[0] : turi_nil();
    if (a[3].tag == TURI_CLOSURE) {
        MapTuriEqCtx ctx = { e, a[3] };
        Hamt *r = tur_hamt_del_eq_ctx(set_hamt(a[0]), (uint64_t)a[1].as_int,
                                      (void *)(intptr_t)a[2].as_int,
                                      map_turi_eq_tramp, &ctx);
        return set_wrap(r);
    }
    Hamt *r = tur_hamt_del_eq(set_hamt(a[0]), (uint64_t)a[1].as_int,
                              (void *)(intptr_t)a[2].as_int,
                              (tur_hamt_keyeq_fn)(intptr_t)a[3].as_int);
    return set_wrap(r);
}
static TuriValue native_map_count(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)tur_hamt_count(set_hamt(a[0])));
}
static TuriValue native_map_merge(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return n >= 1 ? a[0] : turi_nil();
    return set_wrap(tur_hamt_merge(set_hamt(a[0]), set_hamt(a[1])));
}
static TuriValue native_map_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].tag != TURI_INT || a[0].as_int == 0) return turi_nil();
    tur_hamt_free(set_hamt(a[0]));
    free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

/* tur-map-homog__ -- compile-time homogeneity check; a :nil no-op at runtime
 * (the #map{...}/hamt-of lowering emits it).  Its inline-C body is `(void)a;
 * (void)b;` with no return, which the simple inline-C executor doesn't cover. */
static TuriValue native_map_homog(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return turi_nil();
}

/* map-eq-raw? / map-eq-raw-k? -- structural Map equality.  map.tur's bodies
 * iterate the HAMT and fat-dispatch the value comparator through a C function
 * pointer (`((bool(*)(...))val_cmp[0])(...)`), which the simple inline-C executor
 * cannot run -- so these natives re-implement the iteration and invoke the
 * comparator (a turi closure under --interpret) via turi_call, mirroring
 * native_result_eq.  map-eq-raw-k? additionally threads a MapKey carrier
 * comparator (a real C fn-ptr address from mk-cmp) into the key lookup. */
static bool map_eq_iter(TuriEnv *env, Hamt *h1, Hamt *h2,
                        tur_hamt_keyeq_fn keyeq, TuriValue val_cmp) {
    if (tur_hamt_count(h1) != tur_hamt_count(h2)) return false;
    HamtIter *iter = (HamtIter *)calloc(1, sizeof(HamtIter));
    if (!iter) return false;
    tur_hamt_iter_init(iter, h1);
    uint64_t h; void *k; void *v;
    bool eq = true;
    while (tur_hamt_iter_next(iter, &h, &k, &v)) {
        void *vb = keyeq ? tur_hamt_get_eq(h2, h, k, keyeq)
                         : tur_hamt_get(h2, h, k);
        if (!vb) { eq = false; break; }
        TuriValue cargs[2];
        cargs[0].tag = TURI_INT; cargs[0].as_int = (int64_t)(intptr_t)v;
        cargs[1].tag = TURI_INT; cargs[1].as_int = (int64_t)(intptr_t)vb;
        TuriValue rv = turi_call(env, val_cmp, cargs, 2);
        if (turi_is_error(rv) || env->throwing) {
            /* returns C bool; promote a value-level error to a throw so the
             * caller (native_map_eq*) and the driver propagate it. */
            if (turi_is_error(rv)) { env->throwing = true; env->throw_value = rv; }
            eq = false; break;
        }
        bool veq = rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0;
        if (!veq) { eq = false; break; }
    }
    tur_hamt_iter_free(iter);
    free(iter);
    return eq;
}
static TuriValue native_map_eq_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    return turi_bool(map_eq_iter(env, set_hamt(a[0]), set_hamt(a[1]), NULL, a[2]));
}
static TuriValue native_map_eq_raw_k(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    return turi_bool(map_eq_iter(env, set_hamt(a[0]), set_hamt(a[1]),
                                 (tur_hamt_keyeq_fn)(intptr_t)a[2].as_int, a[3]));
}

/* ----- HAMT iterator runtime bridges (Eq[Map]/Eq[Set] pure-Turmeric loop) -----
 * map.tur's Path A Eq[Map] dispatches through map-eq-driver -> map-eq-loop, a
 * pure-Turmeric walk whose leaf calls (hamt/iter-*, hamt/keyeq, hamt/has-dynamic?,
 * map-hamt, map-iter-cur-val-as, map-get-dynamic-as) are thin extern-c/inline-C
 * shims over the tur_hamt_* runtime.  The tree-walker has no extern-c symbol table
 * and cannot run the inline-C bodies, so without these natives the leaf calls
 * silently yield 0/nil and the loop returns a count-only "true".  Bind the
 * underlying runtime entry points (and the inline-C map-* shims) to real natives;
 * pointers ride the int64 carrier exactly as the compiled ABI uses them. */
static TuriValue native_hamt_iter_alloc(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_iter_alloc((Hamt *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_destroy(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) tur_hamt_iter_destroy((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_hamt_iter_advance(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(tur_hamt_iter_advance((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_cur_hash(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(tur_hamt_iter_cur_hash((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_cur_key(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_iter_cur_key((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_iter_cur_val(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_iter_cur_val((void *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_keyeq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)(intptr_t)tur_hamt_keyeq((Hamt *)(intptr_t)a[0].as_int));
}
static TuriValue native_hamt_get_dynamic(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_int(0);
    void *r = tur_hamt_get_dynamic((Hamt *)(intptr_t)a[0].as_int, a[1].as_int,
                                   (void *)(intptr_t)a[2].as_int,
                                   (void *)(intptr_t)a[3].as_int);
    return turi_int((int64_t)(intptr_t)r);
}
static TuriValue native_hamt_has_dynamic(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 4) return turi_bool(false);
    return turi_bool(tur_hamt_has_dynamic((Hamt *)(intptr_t)a[0].as_int, a[1].as_int,
                                          (void *)(intptr_t)a[2].as_int,
                                          (void *)(intptr_t)a[3].as_int));
}
/* map-hamt: read the HAMT pointer out of the { void* hamt } Map carrier. */
static TuriValue native_map_hamt(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    return turi_int((int64_t)(intptr_t)set_hamt(a[0]));
}

/* SEQ (stdlib/seq): the lazy-Seq inline-C bridges assume the COMPILED ABI --
 * fat-closure calls via a C function pointer and a {__state,__next_fn} generator
 * struct -- so the tree-walker cannot run them.  Under --interpret a Seq factory
 * is a TURI_CLOSURE and a generator is a TURI_GEN; re-implement the bridges over
 * turi_call + turi_gen_advance_val.  A yielded value is boxed as a malloc'd
 * int64 (NULL = exhausted), matching seq-val-some?/seq-val-unwrap. */
static TuriValue seq_as_closure(TuriValue v) {
    /* A factory/callback passed through an `int` param keeps its TURI_CLOSURE
     * tag; if it arrived as a raw carrier, rebuild the closure value. */
    if (v.tag == TURI_CLOSURE) return v;
    return turi_closure((TuriClosure *)(intptr_t)v.as_int);
}
/* k-apply-raw (stdlib/kleisli.tur): invoke the fat-closure carrier `k` on `x`,
 * returning the int-level Option it produces.  kleisli.tur's body is
 * `TUR_APPLY1(k, x)` (a compiled fat-dispatch through a C function pointer)
 * which the tree-walker cannot run; this native recovers the closure from the
 * int carrier and invokes it via turi_call.  Mirrors sch_apply1 / native_seq_*. */
static TuriValue native_k_apply_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    TuriValue av = turi_int(a[1].as_int);
    TuriValue r = turi_call(env, seq_as_closure(a[0]), &av, 1);
    if (turi_is_error(r) || env->throwing) return r;  /* propagate callback error */
    /* The arrow body returns an int-level Option.  Under --interpret the
     * Option carrier is dual-rep: `some` may yield either a native int64[2]
     * box (TURI_INT) or a make-struct TuriStruct (TURI_STRUCT).  Preserve r's
     * tag rather than flattening to turi_int(r.as_int) -- collapsing a
     * TURI_STRUCT to a bare int pointer drops the struct tag, and downstream
     * some?/unwrap-or would then misread the TuriStruct as a raw int64[2]
     * box.  Returning r keeps both representations intact for the dual-rep
     * option shims (option_field / option_is_some). */
    return r;
}
static TuriValue native_seq_iter(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_nil();
    int64_t *s = (int64_t *)(intptr_t)a[0].as_int;
    if (!s) return turi_nil();
    return turi_call(e, seq_as_closure(turi_int(s[0])), NULL, 0); /* mk() -> gen */
}
static TuriValue native_seq_gen_done(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_bool(true);
    return turi_bool(turi_gen_done_val(a[0]));
}
static TuriValue native_seq_gen_next(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_int(0);
    int done = 0;
    /* turi_gen_advance_val already returns a pointer to the generator's value
     * box (the compiled ptr<void> yield protocol), valid until the next advance
     * -- exactly what seq-val-some?/seq-val-unwrap expect.  Pass it through; do
     * not re-box (that would hand back a pointer-to-pointer). */
    TuriValue v = turi_gen_advance_val(e, a[0], &done);
    if (done) return turi_int(0);                       /* NULL = exhausted */
    return v;
}
static TuriValue native_seq_val_some(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(n >= 1 && a[0].as_int != 0);
}
static TuriValue native_seq_val_unwrap(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(*(int64_t *)(intptr_t)a[0].as_int);
}
/* gen<->int identity casts used by the transform layer (the gen is already an
 * int64 carrier under --interpret). */
static TuriValue native_seq_gen_to_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_int(n >= 1 ? a[0].as_int : 0);
}
/* Fat-closure call bridges: the callback is a TURI_CLOSURE under --interpret. */
static TuriValue native_seq_call_fn0(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_int(0);
    return turi_call(e, seq_as_closure(a[0]), NULL, 0);
}
static TuriValue native_seq_call_fn1(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    return turi_call(e, seq_as_closure(a[0]), &a[1], 1);
}
static TuriValue native_seq_call_fn2(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_int(0);
    TuriValue args[2] = { a[1], a[2] };
    return turi_call(e, seq_as_closure(a[0]), args, 2);
}
static TuriValue native_seq_call_bool_fn1(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_bool(false);
    TuriValue r = turi_call(e, seq_as_closure(a[0]), &a[1], 1);
    if (turi_is_error(r) || e->throwing) return r;  /* propagate callback error */
    return turi_bool(r.tag == TURI_BOOL ? r.as_bool : r.as_int != 0);
}
static TuriValue native_seq_call_void_fn1(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_nil();
    TuriValue r = turi_call(e, seq_as_closure(a[0]), &a[1], 1);
    if (turi_is_error(r) || e->throwing) return r;  /* propagate callback error */
    return turi_nil();
}
/* gen-arr (stdlib/gen.tur): growable int64 array {len, cap, data} used by
 * gen-collect / seq-collect.  Re-implemented natively (the inline-C grows via
 * malloc/free, which the simple executor cannot run faithfully). */
static TuriValue native_gen_arr_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *h = (int64_t *)malloc(3 * sizeof(int64_t));
    h[0] = 0; h[1] = 0; h[2] = 0;
    return turi_int((int64_t)(intptr_t)h);
}
static TuriValue native_gen_arr_push(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *h = (int64_t *)(intptr_t)a[0].as_int;
    if (h[0] >= h[1]) {
        int64_t nc = h[1] ? h[1] * 2 : 4;
        int64_t *nd = (int64_t *)malloc((size_t)nc * sizeof(int64_t));
        if (h[1]) {
            int64_t *od = (int64_t *)(intptr_t)h[2];
            for (int64_t k = 0; k < h[0]; k++) nd[k] = od[k];
            free(od);
        }
        h[1] = nc; h[2] = (int64_t)(intptr_t)nd;
    }
    ((int64_t *)(intptr_t)h[2])[h[0]++] = a[1].as_int;
    return turi_nil();
}
static TuriValue native_gen_arr_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_gen_arr_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *h = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(((int64_t *)(intptr_t)h[2])[a[1].as_int]);
}
/* seq option box {bool is_some; int64 value} (offset 0 / offset 8 = slot[1]). */
static TuriValue native_seq_make_some(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t *p = (int64_t *)malloc(2 * sizeof(int64_t));
    p[0] = 1; p[1] = (n >= 1) ? a[0].as_int : 0;
    return turi_int((int64_t)(intptr_t)p);
}
static TuriValue native_seq_make_none(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *p = (int64_t *)malloc(2 * sizeof(int64_t));
    p[0] = 0; p[1] = 0;
    return turi_int((int64_t)(intptr_t)p);
}
static TuriValue native_seq_opt_some(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(((int64_t *)(intptr_t)a[0].as_int)[0] != 0);
}
static TuriValue native_seq_opt_unwrap(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
/* seq out-vec {int64* data; len; cap}. */
static TuriValue native_seq_out_vec_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *v = (int64_t *)malloc(3 * sizeof(int64_t));
    v[0] = 0; v[1] = 0; v[2] = 0;
    return turi_int((int64_t)(intptr_t)v);
}
static TuriValue native_seq_out_vec_push(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_nil();
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;   /* {data, len, cap} */
    if (v[1] == v[2]) {
        v[2] = v[2] ? v[2] * 2 : 8;
        v[0] = (int64_t)(intptr_t)realloc((void *)(intptr_t)v[0],
                                          (size_t)v[2] * sizeof(int64_t));
    }
    ((int64_t *)(intptr_t)v[0])[v[1]++] = a[1].as_int;
    return turi_nil();
}
static TuriValue native_seq_vec_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
static TuriValue native_seq_vec_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *v = (int64_t *)(intptr_t)a[0].as_int;
    return turi_int(((int64_t *)(intptr_t)v[0])[a[1].as_int]);
}
/* seq cons cell / Tuple2: both are {slot0, slot1}. */
static TuriValue native_seq_make_cell(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t *c = (int64_t *)malloc(2 * sizeof(int64_t));
    c[0] = (n >= 1) ? a[0].as_int : 0;
    c[1] = (n >= 2) ? a[1].as_int : 0;
    return turi_int((int64_t)(intptr_t)c);
}
static TuriValue native_seq_cell_fst(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_seq_cell_snd(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
static TuriValue native_seq_list_nil(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return turi_bool(n < 1 || a[0].as_int == 0);
}
static void wk_register_seq_natives(TuriEnv *env) {
    turi_env_register_native(env, "seq-iter",            native_seq_iter,         NULL);
    turi_env_register_native(env, "seq-gen-done",        native_seq_gen_done,     NULL);
    turi_env_register_native(env, "seq-gen-next",        native_seq_gen_next,     NULL);
    turi_env_register_native(env, "seq-val-some?",       native_seq_val_some,     NULL);
    turi_env_register_native(env, "seq-val-unwrap",      native_seq_val_unwrap,   NULL);
    /* transform-layer gen<->int + driver aliases (same gen carrier). */
    turi_env_register_native(env, "seq-gen-to-int",      native_seq_gen_to_int,   NULL);
    turi_env_register_native(env, "seq-gen-from-int",    native_seq_gen_to_int,   NULL);
    turi_env_register_native(env, "seq-gen-done-int",    native_seq_gen_done,     NULL);
    turi_env_register_native(env, "seq-gen-next-int",    native_seq_gen_next,     NULL);
    /* fat-closure call bridges (one per arity/result shape). */
    turi_env_register_native(env, "seq-call-fn0",        native_seq_call_fn0,     NULL);
    turi_env_register_native(env, "seq-call-fn1",        native_seq_call_fn1,     NULL);
    turi_env_register_native(env, "seq-call-fn2",        native_seq_call_fn2,     NULL);
    turi_env_register_native(env, "seq-call-bool-fn1",   native_seq_call_bool_fn1, NULL);
    turi_env_register_native(env, "seq-call-void-fn1",   native_seq_call_void_fn1, NULL);
    /* gen-arr (gen.tur) {len, cap, data} -- also unblocks gen-collect. */
    turi_env_register_native(env, "gen-arr-new",         native_gen_arr_new,      NULL);
    turi_env_register_native(env, "gen-arr-push!",       native_gen_arr_push,     NULL);
    turi_env_register_native(env, "gen-arr-len",         native_gen_arr_len,      NULL);
    turi_env_register_native(env, "gen-arr-get",         native_gen_arr_get,      NULL);
    /* seq option box / out-vec / cons cell / Tuple2 helpers (consistent layouts:
     * all producers and accessors are nativized together). */
    turi_env_register_native(env, "seq-make-some",       native_seq_make_some,    NULL);
    turi_env_register_native(env, "seq-make-none",       native_seq_make_none,    NULL);
    turi_env_register_native(env, "seq-opt-some?",       native_seq_opt_some,     NULL);
    turi_env_register_native(env, "seq-opt-unwrap",      native_seq_opt_unwrap,   NULL);
    turi_env_register_native(env, "seq-out-vec-new",     native_seq_out_vec_new,  NULL);
    turi_env_register_native(env, "seq-out-vec-push!",   native_seq_out_vec_push, NULL);
    turi_env_register_native(env, "seq-vec-len",         native_seq_vec_len,      NULL);
    turi_env_register_native(env, "seq-vec-get",         native_seq_vec_get,      NULL);
    turi_env_register_native(env, "seq-make-cons",       native_seq_make_cell,    NULL);
    turi_env_register_native(env, "seq-make-pair",       native_seq_make_cell,    NULL);
    turi_env_register_native(env, "seq-list-head",       native_seq_cell_fst,     NULL);
    turi_env_register_native(env, "seq-list-tail",       native_seq_cell_snd,     NULL);
    turi_env_register_native(env, "seq-pair-first",      native_seq_cell_fst,     NULL);
    turi_env_register_native(env, "seq-pair-second",     native_seq_cell_snd,     NULL);
    turi_env_register_native(env, "seq-list-nil?",       native_seq_list_nil,     NULL);
}

/* JSON (stdlib/json.tur): a self-contained tagged-AST JSON engine.  The public
 * ops are inline-C wrappers over malloc'd structs + a recursive-descent parser
 * and encoder (strtoll/strtod/strncmp/memcpy), which try_exec_simple_inline_c
 * cannot run -- so the whole engine is re-implemented as natives with
 * layout-exact node structures (json-schema-interpreter-plan, Layer 1).
 *
 * Node layout: int64[2] = {type, payload}
 *   0 null   payload 0
 *   1 bool   payload 0/1
 *   2 int    payload int64 value
 *   3 float  payload = double bits (memcpy into the int64 slot)
 *   4 string payload = (char*) strdup'd
 *   5 array  payload = (tur_json_vec*) {int64* data; size_t len; size_t cap}
 *   6 object payload = head of a LIFO list of int64[3] {key, val, next}
 * Every producer/accessor below agrees bit-for-bit with the json.tur inline-C
 * so a non-nativized reader (or the encoder/decoder recursion) sees the same
 * bytes. */
typedef struct { int64_t *data; size_t len; size_t cap; } tur_json_vec;

/* Extract a NUL-terminated C string from a cstr-typed argument (it may arrive
 * tagged TURI_CSTR or as a raw pointer in the int64 carrier). */
static const char *json_arg_cstr(TuriValue v) {
    if (v.tag == TURI_CSTR) return v.as_cstr;
    return (const char *)(intptr_t)v.as_int;
}
static inline int64_t *json_node_ptr(TuriValue v) {
    return (int64_t *)(intptr_t)v.as_int;
}

/* --- builders --- */
static TuriValue native_json_null(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 0; node[1] = 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_bool(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t v = 0;
    if (n >= 1) v = (a[0].tag == TURI_BOOL) ? (a[0].as_bool ? 1 : 0)
                                            : (a[0].as_int ? 1 : 0);
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 1; node[1] = v ? 1 : 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 2; node[1] = (n >= 1) ? a[0].as_int : 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_float(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    double v = (n >= 1) ? ((a[0].tag == TURI_FLOAT) ? a[0].as_float
                                                    : (double)a[0].as_int)
                        : 0.0;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 3;
    memcpy(&node[1], &v, sizeof(double));
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *s = (n >= 1) ? json_arg_cstr(a[0]) : "";
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 4; node[1] = (int64_t)(intptr_t)strdup(s ? s : "");
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_array_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    tur_json_vec *v = malloc(sizeof(*v));
    v->data = NULL; v->len = 0; v->cap = 0;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 5; node[1] = (int64_t)(intptr_t)v;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_array_push(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)node[1];
    if (v->len >= v->cap) {
        v->cap = v->cap > 0 ? v->cap * 2 : 4;
        v->data = realloc(v->data, v->cap * sizeof(int64_t));
    }
    v->data[v->len++] = a[1].as_int;
    return a[0];
}
static TuriValue native_json_object_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    int64_t *node = malloc(2 * sizeof(int64_t));
    node[0] = 6; node[1] = 0;
    return turi_int((int64_t)(intptr_t)node);
}
static TuriValue native_json_object_put(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t *entry = malloc(3 * sizeof(int64_t));
    entry[0] = (int64_t)(intptr_t)strdup(key ? key : "");
    entry[1] = a[2].as_int;
    entry[2] = node[1];
    node[1] = (int64_t)(intptr_t)entry;
    return a[0];
}

/* --- accessors --- */
static TuriValue native_json_type(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[0]);
}
static TuriValue native_json_get_bool(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(json_node_ptr(a[0])[1] != 0);
}
static TuriValue native_json_get_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[1]);
}
static TuriValue native_json_get_float(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_float(0.0);
    double v;
    memcpy(&v, &json_node_ptr(a[0])[1], sizeof(double));
    return turi_float(v);
}
static TuriValue native_json_get_string(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_cstr("");
    return turi_cstr((const char *)(intptr_t)json_node_ptr(a[0])[1]);
}
static TuriValue native_json_array_len(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)node[1];
    return turi_int((int64_t)v->len);
}
static TuriValue native_json_array_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)node[1];
    int64_t i = a[1].as_int;
    if (i < 0 || (size_t)i >= v->len) return turi_int(0);
    return turi_int(v->data[i]);
}
/* json/get: returns a some-option box {int64 is_some; int64 value} (matching the
 * inline-C struct {bool is_some; int64 value}, padded to int64[2]) or 0 (none). */
static TuriValue native_json_get(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    int64_t *node = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t cur = node[1];
    while (cur) {
        int64_t *ent = (int64_t *)(intptr_t)cur;
        if (strcmp((const char *)(intptr_t)ent[0], key) == 0) {
            int64_t *opt = malloc(2 * sizeof(int64_t));
            opt[0] = 1; opt[1] = ent[1];
            return turi_int((int64_t)(intptr_t)opt);
        }
        cur = ent[2];
    }
    return turi_int(0);
}
static TuriValue native_json_get_bang(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) {
        fprintf(stderr, "json/get!: null node\n");
        abort();
    }
    int64_t *node = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t cur = node[1];
    while (cur) {
        int64_t *ent = (int64_t *)(intptr_t)cur;
        if (strcmp((const char *)(intptr_t)ent[0], key) == 0)
            return turi_int(ent[1]);
        cur = ent[2];
    }
    fprintf(stderr, "json/get!: key not found: %s\n", key);
    abort();
}

/* --- encoder: a growable char buffer + recursive node walk --- */
typedef struct { char *data; size_t len; size_t cap; } tur_json_encbuf;
static void json_enc_ensure(tur_json_encbuf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        b->cap = (b->len + extra + 1) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
}
static void json_enc_append_s(tur_json_encbuf *b, const char *s) {
    size_t slen = strlen(s);
    json_enc_ensure(b, slen);
    memcpy(b->data + b->len, s, slen);
    b->len += slen;
    b->data[b->len] = '\0';
}
static void json_enc_append_c(tur_json_encbuf *b, char c) {
    json_enc_ensure(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}
static void json_enc_str(tur_json_encbuf *b, const char *s) {
    json_enc_append_c(b, '"');
    for (const char *sp = s; *sp; sp++) {
        if (*sp == '"')       json_enc_append_s(b, "\\\"");
        else if (*sp == '\\') json_enc_append_s(b, "\\\\");
        else if (*sp == '\n') json_enc_append_s(b, "\\n");
        else if (*sp == '\r') json_enc_append_s(b, "\\r");
        else if (*sp == '\t') json_enc_append_s(b, "\\t");
        else                  json_enc_append_c(b, *sp);
    }
    json_enc_append_c(b, '"');
}
static void json_enc_node(int64_t node, tur_json_encbuf *b) {
    if (!node) { json_enc_append_s(b, "null"); return; }
    int64_t *np = (int64_t *)(intptr_t)node;
    int type = (int)np[0];
    char tmp[64];
    switch (type) {
        case 0: json_enc_append_s(b, "null"); break;
        case 1: json_enc_append_s(b, np[1] ? "true" : "false"); break;
        case 2: snprintf(tmp, sizeof(tmp), "%lld", (long long)np[1]);
                json_enc_append_s(b, tmp); break;
        case 3: {
            double v; memcpy(&v, &np[1], sizeof(double));
            snprintf(tmp, sizeof(tmp), "%.17g", v);
            json_enc_append_s(b, tmp); break;
        }
        case 4: json_enc_str(b, (const char *)(intptr_t)np[1]); break;
        case 5: {
            tur_json_vec *v = (tur_json_vec *)(intptr_t)np[1];
            json_enc_append_c(b, '[');
            for (size_t i = 0; i < v->len; i++) {
                if (i > 0) json_enc_append_c(b, ',');
                json_enc_node(v->data[i], b);
            }
            json_enc_append_c(b, ']');
            break;
        }
        case 6: {
            json_enc_append_c(b, '{');
            int64_t cur = np[1];
            int first = 1;
            while (cur) {
                int64_t *ent = (int64_t *)(intptr_t)cur;
                if (!first) json_enc_append_c(b, ',');
                first = 0;
                json_enc_str(b, (const char *)(intptr_t)ent[0]);
                json_enc_append_c(b, ':');
                json_enc_node(ent[1], b);
                cur = ent[2];
            }
            json_enc_append_c(b, '}');
            break;
        }
        default: json_enc_append_s(b, "null"); break;
    }
}
static TuriValue native_json_encode(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_json_encbuf b;
    b.data = malloc(256);
    b.data[0] = '\0';
    b.len = 0;
    b.cap = 256;
    json_enc_node((n >= 1) ? a[0].as_int : 0, &b);
    return turi_cstr(b.data);
}

/* --- decoder: recursive-descent over a {s, pos, err} context --- */
typedef struct { const char *s; size_t pos; int err; } tur_json_ctx;
static void json_dec_skip_ws(tur_json_ctx *c) {
    while (c->s[c->pos] == ' ' || c->s[c->pos] == '\n' ||
           c->s[c->pos] == '\r' || c->s[c->pos] == '\t') c->pos++;
}
static char *json_dec_parse_string(tur_json_ctx *c) {
    if (c->s[c->pos] != '"') { c->err = 1; return NULL; }
    c->pos++;
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    while (c->s[c->pos] && c->s[c->pos] != '"') {
        if (len + 4 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        if (c->s[c->pos] == '\\') {
            c->pos++;
            switch (c->s[c->pos]) {
                case '"':  buf[len++] = '"';  break;
                case '\\': buf[len++] = '\\'; break;
                case '/':  buf[len++] = '/';  break;
                case 'n':  buf[len++] = '\n'; break;
                case 'r':  buf[len++] = '\r'; break;
                case 't':  buf[len++] = '\t'; break;
                case 'b':  buf[len++] = '\b'; break;
                case 'f':  buf[len++] = '\f'; break;
                default:   buf[len++] = c->s[c->pos]; break;
            }
        } else {
            buf[len++] = c->s[c->pos];
        }
        c->pos++;
    }
    if (c->s[c->pos] != '"') { c->err = 1; free(buf); return NULL; }
    c->pos++;
    buf[len] = '\0';
    return buf;
}
static int64_t json_dec_parse_value(tur_json_ctx *c) {
    json_dec_skip_ws(c);
    char ch = c->s[c->pos];
    if (ch == 'n' && strncmp(c->s + c->pos, "null", 4) == 0) {
        c->pos += 4;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 0; node[1] = 0;
        return (int64_t)(intptr_t)node;
    }
    if (ch == 't' && strncmp(c->s + c->pos, "true", 4) == 0) {
        c->pos += 4;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 1; node[1] = 1;
        return (int64_t)(intptr_t)node;
    }
    if (ch == 'f' && strncmp(c->s + c->pos, "false", 5) == 0) {
        c->pos += 5;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 1; node[1] = 0;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '"') {
        char *sv = json_dec_parse_string(c);
        if (!sv) return 0;
        int64_t *node = malloc(2 * sizeof(int64_t));
        node[0] = 4; node[1] = (int64_t)(intptr_t)sv;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        char *end;
        int64_t ival = strtoll(c->s + c->pos, &end, 10);
        if (*end == '.' || *end == 'e' || *end == 'E') {
            double fval = strtod(c->s + c->pos, &end);
            c->pos = (size_t)(end - c->s);
            int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 3;
            memcpy(&node[1], &fval, sizeof(double));
            return (int64_t)(intptr_t)node;
        }
        c->pos = (size_t)(end - c->s);
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 2; node[1] = ival;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '[') {
        c->pos++;
        tur_json_vec *v = malloc(sizeof(*v));
        v->data = NULL; v->len = 0; v->cap = 0;
        json_dec_skip_ws(c);
        if (c->s[c->pos] != ']') {
            for (;;) {
                int64_t elem = json_dec_parse_value(c);
                if (c->err) { free(v->data); free(v); return 0; }
                if (v->len >= v->cap) {
                    v->cap = v->cap > 0 ? v->cap * 2 : 4;
                    v->data = realloc(v->data, v->cap * sizeof(int64_t));
                }
                v->data[v->len++] = elem;
                json_dec_skip_ws(c);
                if (c->s[c->pos] == ']') break;
                if (c->s[c->pos] != ',') { c->err = 1; free(v->data); free(v); return 0; }
                c->pos++;
            }
        }
        c->pos++;
        int64_t *node = malloc(2 * sizeof(int64_t));
        node[0] = 5; node[1] = (int64_t)(intptr_t)v;
        return (int64_t)(intptr_t)node;
    }
    if (ch == '{') {
        c->pos++;
        int64_t *node = malloc(2 * sizeof(int64_t)); node[0] = 6; node[1] = 0;
        json_dec_skip_ws(c);
        if (c->s[c->pos] != '}') {
            for (;;) {
                json_dec_skip_ws(c);
                char *key = json_dec_parse_string(c);
                if (!key || c->err) { free(node); return 0; }
                json_dec_skip_ws(c);
                if (c->s[c->pos] != ':') { c->err = 1; free(key); free(node); return 0; }
                c->pos++;
                int64_t val = json_dec_parse_value(c);
                if (c->err) { free(key); free(node); return 0; }
                int64_t *entry = malloc(3 * sizeof(int64_t));
                entry[0] = (int64_t)(intptr_t)key;
                entry[1] = val;
                entry[2] = node[1];
                node[1] = (int64_t)(intptr_t)entry;
                json_dec_skip_ws(c);
                if (c->s[c->pos] == '}') break;
                if (c->s[c->pos] != ',') { c->err = 1; free(node); return 0; }
                c->pos++;
            }
        }
        c->pos++;
        return (int64_t)(intptr_t)node;
    }
    c->err = 1;
    return 0;
}
static TuriValue native_json_decode(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    const char *s = json_arg_cstr(a[0]);
    if (!s) return turi_int(0);
    tur_json_ctx ctx; ctx.s = s; ctx.pos = 0; ctx.err = 0;
    int64_t result = json_dec_parse_value(&ctx);
    if (ctx.err) return turi_int(0);
    return turi_int(result);
}

/* --- free: no-op under the interpreter's process-lifetime policy (match the
 * signature so a call type-checks and is a harmless no-op). --- */
static TuriValue native_json_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_nil();
}

static void wk_register_json_natives(TuriEnv *env) {
    /* builders */
    turi_env_register_native(env, "json/null",       native_json_null,       NULL);
    turi_env_register_native(env, "json/bool",       native_json_bool,       NULL);
    turi_env_register_native(env, "json/int",        native_json_int,        NULL);
    turi_env_register_native(env, "json/float",      native_json_float,      NULL);
    turi_env_register_native(env, "json/string",     native_json_string,     NULL);
    turi_env_register_native(env, "json/array-new",  native_json_array_new,  NULL);
    turi_env_register_native(env, "json/array-push", native_json_array_push, NULL);
    turi_env_register_native(env, "json/object-new", native_json_object_new, NULL);
    turi_env_register_native(env, "json/object-put", native_json_object_put, NULL);
    /* accessors */
    turi_env_register_native(env, "json/type",       native_json_type,       NULL);
    turi_env_register_native(env, "json/get-bool",   native_json_get_bool,   NULL);
    turi_env_register_native(env, "json/get-int",    native_json_get_int,    NULL);
    turi_env_register_native(env, "json/get-float",  native_json_get_float,  NULL);
    turi_env_register_native(env, "json/get-string", native_json_get_string, NULL);
    turi_env_register_native(env, "json/array-len",  native_json_array_len,  NULL);
    turi_env_register_native(env, "json/array-get",  native_json_array_get,  NULL);
    turi_env_register_native(env, "json/get",        native_json_get,        NULL);
    turi_env_register_native(env, "json/get!",       native_json_get_bang,   NULL);
    /* encoder / decoder / free */
    turi_env_register_native(env, "json/encode",     native_json_encode,     NULL);
    turi_env_register_native(env, "json/decode",     native_json_decode,     NULL);
    turi_env_register_native(env, "json/free",       native_json_free,       NULL);
}

/* SCHEMA (stdlib/schema.tur): the runtime schema validator built on top of the
 * tagged JSON nodes (Layer 2 of turi-json-schema-interpreter-plan).  A schema
 * node is int64[4]{kind,a,b,c} with the SCHEMA_* discriminants; the decoder
 * (sch-decode-rec-) recursively walks (schema, json-node, path) accumulating
 * SchemaError records into a {data,len,cap} vector.  The constructors malloc
 * tagged structs and the decoder recurses + invokes transform/fmap/ap fat
 * closures via a C function pointer -- neither runnable by the simple inline-C
 * executor -- so the whole engine is re-implemented natively with layout-exact
 * structs, and the fat-closure call points route through turi_call (the closure
 * is a TURI_CLOSURE under --interpret, same shape as the seq bridges).
 *
 * Schema node:   int64[4]{kind, a, b, c}
 * Object fields: s[1]=count s[2]=cap s[3]=(int64* data) of {key,inner} pairs
 * SchemaError:   int64[3]{char* path, char* msg, int64 val}
 * Error vec:     {int64* data; size_t len; size_t cap}  (== int64[3])
 * Result:        {bool is_ok; int64 ok_val; int64 err_val} (== int64[3]:
 *                [0]=is_ok [1]=ok_val [2]=err_val). */
typedef struct { int64_t kind, a, b, c; } tur_sch_t;

static const char *sch_tyname(int64_t jtype) {
    switch (jtype) {
        case 0: return ":nil";    case 1: return ":bool";
        case 2: return ":int";    case 3: return ":float";
        case 4: return ":cstr";   case 5: return ":array";
        case 6: return ":object"; default: return ":unknown";
    }
}
static const char *sch_want_name(int64_t kind) {
    switch (kind) {
        case 0: return ":cstr";  case 1: return ":int";
        case 2: return ":float"; case 3: return ":bool";
        case 4: return ":nil";   case 6: return ":object";
        case 7: return ":array"; default: return "value";
    }
}
static int64_t sch_jtype(int64_t node) {
    if (!node) return -1;
    return (int64_t)(int)((int64_t *)(intptr_t)node)[0];
}
static int64_t sch_jpayload(int64_t node) {
    return ((int64_t *)(intptr_t)node)[1];
}
static void sch_vpush(tur_json_vec *v, int64_t x) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->data = realloc(v->data, v->cap * sizeof(int64_t));
    }
    v->data[v->len++] = x;
}
static void sch_push_err(tur_json_vec *errs, const char *path,
                         const char *msg, int64_t val) {
    int64_t *e = malloc(3 * sizeof(int64_t));
    e[0] = (int64_t)(intptr_t)strdup(path ? path : "");
    e[1] = (int64_t)(intptr_t)strdup(msg);
    e[2] = val;
    sch_vpush(errs, (int64_t)(intptr_t)e);
}
static char *sch_mkpath(const char *base, const char *key) {
    if (!base || !base[0]) return strdup(key);
    size_t n = strlen(base) + strlen(key) + 2;
    char *out = malloc(n);
    snprintf(out, n, "%s.%s", base, key);
    return out;
}
static char *sch_mkidx(const char *base, int64_t idx) {
    const char *b = base ? base : "";
    size_t n = strlen(b) + 24;
    char *out = malloc(n);
    snprintf(out, n, "%s[%lld]", b, (long long)idx);
    return out;
}
/* Invoke a stored fat closure (carried as the int64 pointer in the schema node)
 * with one int64-carrier argument, returning the result as an int64 carrier. */
static int64_t sch_apply1(TuriEnv *env, int64_t fn_carrier, int64_t arg) {
    TuriValue av = turi_int(arg);
    TuriValue r = turi_call(env, seq_as_closure(turi_int(fn_carrier)), &av, 1);
    if (turi_is_error(r) || env->throwing) {
        /* returns a raw int64 carrier; promote a value-level error to a throw so
         * the enclosing schema native and the driver propagate it. */
        if (turi_is_error(r)) { env->throwing = true; env->throw_value = r; }
        return 0;
    }
    if (r.tag == TURI_FLOAT) { int64_t b; memcpy(&b, &r.as_float, 8); return b; }
    if (r.tag == TURI_BOOL)  return r.as_bool ? 1 : 0;
    return r.as_int;
}

/* The recursive decoder -- mirrors sch-decode-rec- (schema.tur) bit-for-bit,
 * with TUR_APPLY1 / the kind-14 C call routed through turi_call. */
static int64_t sch_decode_rec(TuriEnv *env, int64_t schema, int64_t node,
                              const char *path, tur_json_vec *ep) {
    tur_sch_t *s = (tur_sch_t *)(intptr_t)schema;
    if (!s) return 0;
    char buf[128];
    int64_t jt = sch_jtype(node);
    int64_t jp = node ? sch_jpayload(node) : 0;
    switch (s->kind) {
        case 0: case 1: case 2: case 3: case 4: { /* scalars */
            int64_t want = (s->kind == 0) ? 4 : (s->kind == 1) ? 2 :
                           (s->kind == 2) ? 3 : (s->kind == 3) ? 1 : 0;
            if (jt != want) {
                snprintf(buf, sizeof(buf), "expected %s, got %s",
                         sch_want_name(s->kind), sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            return jp;
        }
        case 5: { /* literal */
            if (s->a == 2) {
                if (jt != 2 || jp != s->b) {
                    snprintf(buf, sizeof(buf), "expected literal %lld", (long long)s->b);
                    sch_push_err(ep, path, buf, node);
                    return 0;
                }
                return s->b;
            } else {
                const char *want = (const char *)(intptr_t)s->b;
                if (jt != 4 || strcmp((const char *)(intptr_t)jp, want) != 0) {
                    snprintf(buf, sizeof(buf), "expected literal \"%s\"", want);
                    sch_push_err(ep, path, buf, node);
                    return 0;
                }
                return jp;
            }
        }
        case 6: { /* object */
            if (jt != 6) {
                snprintf(buf, sizeof(buf), "expected :object, got %s", sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            int64_t fcount = s->a;
            int64_t *fdata = (int64_t *)(intptr_t)s->c;
            if (fdata) {
                for (int64_t i = 0; i < fcount; i++) {
                    const char *key = (const char *)(intptr_t)fdata[i * 2];
                    tur_sch_t *fsch = (tur_sch_t *)(intptr_t)fdata[i * 2 + 1];
                    int64_t *on = (int64_t *)(intptr_t)node;
                    int64_t cur = on[1]; int64_t found = 0; int present = 0;
                    while (cur) {
                        int64_t *ent = (int64_t *)(intptr_t)cur;
                        if (strcmp((const char *)(intptr_t)ent[0], key) == 0) {
                            found = ent[1]; present = 1; break;
                        }
                        cur = ent[2];
                    }
                    char *fpath = sch_mkpath(path, key);
                    if (!present) {
                        if (fsch && fsch->kind == 8) { free(fpath); continue; }
                        sch_push_err(ep, fpath, "missing required field", 0);
                    } else {
                        sch_decode_rec(env, (int64_t)(intptr_t)fsch, found, fpath, ep);
                    }
                    free(fpath);
                }
            }
            return node;
        }
        case 7: { /* array */
            if (jt != 5) {
                snprintf(buf, sizeof(buf), "expected :array, got %s", sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            int64_t elem = s->a;
            tur_json_vec *jarr = (tur_json_vec *)(intptr_t)jp;
            tur_json_vec *out = malloc(sizeof(*out));
            out->data = NULL; out->len = 0; out->cap = 0;
            if (jarr) {
                for (size_t i = 0; i < jarr->len; i++) {
                    char *ipath = sch_mkidx(path, (int64_t)i);
                    int64_t dv = sch_decode_rec(env, elem, jarr->data[i], ipath, ep);
                    free(ipath);
                    sch_vpush(out, dv);
                }
            }
            return (int64_t)(intptr_t)out;
        }
        case 8: { /* optional */
            if (jt == 0 || node == 0) return 0;
            return sch_decode_rec(env, s->a, node, path, ep);
        }
        case 9: { /* union: first arm that matches wins */
            tur_json_vec *arms = (tur_json_vec *)(intptr_t)s->a;
            if (!arms || arms->len == 0) return 0;
            for (size_t i = 0; i < arms->len; i++) {
                size_t before = ep->len;
                int64_t dv = sch_decode_rec(env, arms->data[i], node, path, ep);
                if (ep->len == before) return dv;
                ep->len = before;
            }
            snprintf(buf, sizeof(buf), "no union arm matched (got %s)", sch_tyname(jt));
            sch_push_err(ep, path, buf, node);
            return 0;
        }
        case 10: { /* transform / fmap */
            size_t before = ep->len;
            int64_t dv = sch_decode_rec(env, s->a, node, path, ep);
            if (ep->len != before) return 0;
            return sch_apply1(env, s->b, dv);
        }
        case 11: { /* recursive */
            if (s->c == 0) {
                s->c = sch_apply1(env, s->a, (int64_t)(intptr_t)s);
            }
            return sch_decode_rec(env, s->c, node, path, ep);
        }
        case 12: return s->a; /* always */
        case 13: /* never */
            sch_push_err(ep, path, (const char *)(intptr_t)s->a, node);
            return 0;
        case 14: case 16: { /* ap / ap-fat: apply decoded fn arm to arg arm */
            size_t before = ep->len;
            int64_t fv = sch_decode_rec(env, s->a, node, path, ep);
            int64_t av = sch_decode_rec(env, s->b, node, path, ep);
            if (ep->len != before) return 0;
            return sch_apply1(env, fv, av);
        }
        case 15: { /* field-of */
            if (jt != 6) {
                snprintf(buf, sizeof(buf), "expected :object, got %s", sch_tyname(jt));
                sch_push_err(ep, path, buf, node);
                return 0;
            }
            const char *key = (const char *)(intptr_t)s->a;
            tur_sch_t *inner = (tur_sch_t *)(intptr_t)s->b;
            int64_t *on = (int64_t *)(intptr_t)node;
            int64_t cur = on[1]; int64_t found = 0; int present = 0;
            while (cur) {
                int64_t *ent = (int64_t *)(intptr_t)cur;
                if (strcmp((const char *)(intptr_t)ent[0], key) == 0) {
                    found = ent[1]; present = 1; break;
                }
                cur = ent[2];
            }
            char *fpath = sch_mkpath(path, key);
            int64_t result;
            if (!present) {
                if (inner && inner->kind == 8) { result = 0; }
                else { sch_push_err(ep, fpath, "missing required field", 0); result = 0; }
            } else {
                result = sch_decode_rec(env, (int64_t)(intptr_t)inner, found, fpath, ep);
            }
            free(fpath);
            return result;
        }
        default: return 0;
    }
}

/* --- schema node constructors (each mallocs int64[4]{kind,a,b,c}) --- */
static int64_t *sch_alloc(int64_t kind, int64_t a, int64_t b, int64_t c) {
    int64_t *s = malloc(4 * sizeof(int64_t));
    s[0] = kind; s[1] = a; s[2] = b; s[3] = c;
    return s;
}
#define SCH_RET(p) turi_int((int64_t)(intptr_t)(p))
static TuriValue native_schema_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(0, 0, 0, 0));
}
static TuriValue native_schema_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(1, 0, 0, 0));
}
static TuriValue native_schema_float(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(2, 0, 0, 0));
}
static TuriValue native_schema_bool(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(3, 0, 0, 0));
}
static TuriValue native_schema_nil(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(4, 0, 0, 0));
}
static TuriValue native_schema_literal_int(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(5, 2, (n >= 1) ? a[0].as_int : 0, 0));
}
static TuriValue native_schema_literal_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *v = (n >= 1) ? json_arg_cstr(a[0]) : "";
    return SCH_RET(sch_alloc(5, 4, (int64_t)(intptr_t)strdup(v ? v : ""), 0));
}
static TuriValue native_schema_object_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud; return SCH_RET(sch_alloc(6, 0, 0, 0));
}
static TuriValue native_schema_field(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 3) return turi_int(0);
    int64_t *s = json_node_ptr(a[0]);
    const char *key = json_arg_cstr(a[1]);
    int64_t count = s[1], cap = s[2];
    int64_t *data = (int64_t *)(intptr_t)s[3];
    if (count == cap) {
        cap = cap ? cap * 2 : 4;
        data = realloc(data, cap * 2 * sizeof(int64_t));
        s[2] = cap;
        s[3] = (int64_t)(intptr_t)data;
    }
    data[count * 2]     = (int64_t)(intptr_t)strdup(key ? key : "");
    data[count * 2 + 1] = a[2].as_int;
    s[1] = count + 1;
    return a[0];
}
static TuriValue native_schema_array(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(7, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_optional(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(8, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_union(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(9, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_transform(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    return SCH_RET(sch_alloc(10, a[0].as_int, a[1].as_int, 0));
}
static TuriValue native_schema_rec(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(11, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_kind(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(-1);
    return turi_int(json_node_ptr(a[0])[0]);
}
static TuriValue native_schema_always(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    return SCH_RET(sch_alloc(12, (n >= 1) ? a[0].as_int : 0, 0, 0));
}
static TuriValue native_schema_never(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    const char *m = (n >= 1) ? json_arg_cstr(a[0]) : "";
    return SCH_RET(sch_alloc(13, (int64_t)(intptr_t)strdup(m ? m : ""), 0, 0));
}
static TuriValue native_schema_ap(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    return SCH_RET(sch_alloc(14, a[0].as_int, a[1].as_int, 0));
}
static TuriValue native_schema_ap_fat(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    return SCH_RET(sch_alloc(16, a[0].as_int, a[1].as_int, 0));
}
static TuriValue native_schema_field_of(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_int(0);
    const char *key = json_arg_cstr(a[0]);
    return SCH_RET(sch_alloc(15, (int64_t)(intptr_t)strdup(key ? key : ""),
                             a[1].as_int, 0));
}
#undef SCH_RET

/* --- SchemaError accessors --- */
static TuriValue native_schema_error_path(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_cstr("");
    return turi_cstr((const char *)(intptr_t)json_node_ptr(a[0])[0]);
}
static TuriValue native_schema_error_text(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_cstr("");
    return turi_cstr((const char *)(intptr_t)json_node_ptr(a[0])[1]);
}
static TuriValue native_schema_error_count(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)a[0].as_int;
    return turi_int((int64_t)v->len);
}
static TuriValue native_schema_error_at(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    tur_json_vec *v = (tur_json_vec *)(intptr_t)a[0].as_int;
    int64_t i = a[1].as_int;
    if (i < 0 || (size_t)i >= v->len) return turi_int(0);
    return turi_int(v->data[i]);
}
static TuriValue native_schema_error_message(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_json_vec *v = (n >= 1) ? (tur_json_vec *)(intptr_t)a[0].as_int : NULL;
    if (!v || v->len == 0) { char *empty = malloc(1); empty[0] = 0; return turi_cstr(empty); }
    size_t total = 1;
    for (size_t i = 0; i < v->len; i++) {
        int64_t *er = (int64_t *)(intptr_t)v->data[i];
        const char *path = (const char *)(intptr_t)er[0];
        const char *msg  = (const char *)(intptr_t)er[1];
        total += strlen(msg) + 2;
        if (path && path[0]) total += strlen(path) + 2;
    }
    char *out = malloc(total);
    out[0] = 0;
    for (size_t i = 0; i < v->len; i++) {
        int64_t *er = (int64_t *)(intptr_t)v->data[i];
        const char *path = (const char *)(intptr_t)er[0];
        const char *msg  = (const char *)(intptr_t)er[1];
        if (i > 0) strcat(out, "\n");
        if (path && path[0]) { strcat(out, path); strcat(out, ": "); }
        strcat(out, msg);
    }
    return turi_cstr(out);
}

/* --- decoder entry points + Result accessors --- */
static TuriValue native_schema_decode(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    tur_json_vec *errs = malloc(sizeof(*errs));
    errs->data = NULL; errs->len = 0; errs->cap = 0;
    int64_t value = sch_decode_rec(e, a[0].as_int, a[1].as_int, "", errs);
    int64_t *r = malloc(3 * sizeof(int64_t));
    if (errs->len == 0) {
        r[0] = 1; r[1] = value; r[2] = 0;
        free(errs->data); free(errs);
    } else {
        r[0] = 0; r[1] = 0; r[2] = (int64_t)(intptr_t)errs;
    }
    return turi_int((int64_t)(intptr_t)r);
}
static TuriValue native_schema_decode_ok(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_bool(false);
    return turi_bool(json_node_ptr(a[0])[0] != 0);
}
static TuriValue native_schema_decode_value(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[1]);
}
static TuriValue native_schema_decode_errors(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1 || a[0].as_int == 0) return turi_int(0);
    return turi_int(json_node_ptr(a[0])[2]);
}
static TuriValue native_schema_decode_abort(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    tur_json_vec *v = (n >= 1) ? (tur_json_vec *)(intptr_t)a[0].as_int : NULL;
    fprintf(stderr, "schema-decode!: validation failed\n");
    if (v) for (size_t i = 0; i < v->len; i++) {
        int64_t *er = (int64_t *)(intptr_t)v->data[i];
        const char *path = (const char *)(intptr_t)er[0];
        const char *msg  = (const char *)(intptr_t)er[1];
        if (path && path[0]) fprintf(stderr, "  %s: %s\n", path, msg);
        else                 fprintf(stderr, "  %s\n", msg);
    }
    abort();
}

static void wk_register_schema_natives(TuriEnv *env) {
    /* constructors */
    turi_env_register_native(env, "schema/str",         native_schema_str,         NULL);
    turi_env_register_native(env, "schema/int",         native_schema_int,         NULL);
    turi_env_register_native(env, "schema/float",       native_schema_float,       NULL);
    turi_env_register_native(env, "schema/bool",        native_schema_bool,        NULL);
    turi_env_register_native(env, "schema/nil",         native_schema_nil,         NULL);
    turi_env_register_native(env, "schema/literal-int", native_schema_literal_int, NULL);
    turi_env_register_native(env, "schema/literal-str", native_schema_literal_str, NULL);
    turi_env_register_native(env, "schema/object-new",  native_schema_object_new,  NULL);
    turi_env_register_native(env, "schema/field",       native_schema_field,       NULL);
    turi_env_register_native(env, "schema/array",       native_schema_array,       NULL);
    turi_env_register_native(env, "schema/optional",    native_schema_optional,    NULL);
    turi_env_register_native(env, "schema/union",       native_schema_union,       NULL);
    turi_env_register_native(env, "schema/transform",   native_schema_transform,   NULL);
    turi_env_register_native(env, "schema/rec",         native_schema_rec,         NULL);
    turi_env_register_native(env, "schema/kind",        native_schema_kind,        NULL);
    turi_env_register_native(env, "schema/always",      native_schema_always,      NULL);
    turi_env_register_native(env, "schema/never",       native_schema_never,       NULL);
    turi_env_register_native(env, "schema/ap",          native_schema_ap,          NULL);
    turi_env_register_native(env, "schema/ap-fat",      native_schema_ap_fat,      NULL);
    turi_env_register_native(env, "schema/field-of",    native_schema_field_of,    NULL);
    turi_env_register_native(env, "schema/fmap",        native_schema_transform,   NULL);
    /* error accessors */
    turi_env_register_native(env, "schema-error-path",    native_schema_error_path,    NULL);
    turi_env_register_native(env, "schema-error-text",    native_schema_error_text,    NULL);
    turi_env_register_native(env, "schema-error-count",   native_schema_error_count,   NULL);
    turi_env_register_native(env, "schema-error-at",      native_schema_error_at,      NULL);
    turi_env_register_native(env, "schema-error-message", native_schema_error_message, NULL);
    /* decoder + result accessors */
    turi_env_register_native(env, "schema-decode",        native_schema_decode,        NULL);
    turi_env_register_native(env, "schema-decode-ok?",    native_schema_decode_ok,     NULL);
    turi_env_register_native(env, "schema-decode-value",  native_schema_decode_value,  NULL);
    turi_env_register_native(env, "schema-decode-errors", native_schema_decode_errors, NULL);
    turi_env_register_native(env, "schema-decode-abort",  native_schema_decode_abort,  NULL);
}

/* SYM (turi): first-class :Sym natives (-Xsymbols).  The interpreter carries a
 * :Sym as a stable `const Symbol *` (interned in env->st by the EX_SYM_LIT case
 * in eval.c) in the int64 carrier.  These override the inline-C sym.tur bodies
 * (sym->str / sym=? / Eq[Sym] / Hash[Sym]), which deref a `struct __tur_sym`
 * the tree-walker cannot evaluate, and provide str->sym for sym-dynamic.tur. */
static TuriValue native_sym_to_str(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_nil();
    const Symbol *s = (const Symbol *)(intptr_t)a[0].as_int;
    if (!s) return turi_nil();
    return turi_cstr(s->name);
}
static TuriValue native_sym_eq(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2) return turi_bool(false);
    /* Symbols are interned: pointer identity is content equality. */
    return turi_bool(a[0].as_int == a[1].as_int);
}
static TuriValue native_sym_hash(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 1) return turi_int(0);
    const Symbol *s = (const Symbol *)(intptr_t)a[0].as_int;
    return turi_int(s ? (int64_t)s->hash : 0);
}
static TuriValue native_str_to_sym(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 1) return turi_nil();
    const char *str = (a[0].tag == TURI_CSTR)
                        ? a[0].as_cstr
                        : (const char *)(intptr_t)a[0].as_int;
    if (!str) return turi_nil();
    StrSlice sl = { str, (uint32_t)strlen(str) };
    const Symbol *s = symtab_intern(&e->st, sl);
    TuriValue v = {0};
    v.tag = TURI_INT;
    v.as_int = (int64_t)(intptr_t)s;
    return v;
}
static void wk_register_sym_natives(TuriEnv *env) {
    turi_env_register_native(env, "sym->str",  native_sym_to_str, NULL);
    turi_env_register_native(env, "sym=?",      native_sym_eq,     NULL);
    turi_env_register_native(env, "str->sym",   native_str_to_sym, NULL);
    /* Typeclass instance methods: Eq[Sym].eq?, Hash[Sym].hash (pointer identity
     * / precomputed hash).  MapKey[Sym] reuses the int-carrier comparator already
     * registered for scalar keys, so no Sym-specific mk-* native is needed. */
    turi_env_register_native(env, "__inst_Eq_eq_qu_Sym", native_sym_eq,   NULL);
    turi_env_register_native(env, "__inst_Hash_hash_Sym", native_sym_hash, NULL);
    /* MapKey[Sym].mk-cmp returns the carrier comparator address; symbols compare
     * by pointer identity, so reuse the int-carrier eq comparator (its inline-C
     * body returns a captured C function-pointer address the tree-walker cannot
     * evaluate).  mk-box (identity) and mk-owned? (0) are plain bodies the simple
     * inline-C executor already handles. */
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_Sym", native_mk_cmp_int, NULL);
    /* MapKey[Sym].mk-box is inline-C (`(int64_t)(intptr_t)x`) like the cstr/float
     * boxes (the int box is pure-turi `x` and needs none); reuse the cstr native,
     * which returns the carrier word unchanged -- the Sym pointer. */
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_Sym", native_mk_box_cstr, NULL);
}

static void wk_register_map_natives(TuriEnv *env) {
    /* MapKey[K].mk-cmp -- bool(*)(int64,int64) carrier comparator address. */
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_int",     native_mk_cmp_int,  NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_cstr",    native_mk_cmp_cstr, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_float",   native_mk_cmp_f64,  NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hycmp_float32", native_mk_cmp_f32,  NULL);
    /* MapKey[K].mk-box -- int/bool are plain bodies; only cstr/float need a native. */
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_cstr",    native_mk_box_cstr, NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_float",   native_mk_box_f64,  NULL);
    turi_env_register_native(env, "__inst_MapKey_mk_hybox_float32", native_mk_box_f32,  NULL);
    /* Hash[K] -- int/bool plain; cstr/float hash by content/bits. */
    turi_env_register_native(env, "__inst_Hash_hash_cstr",    native_hash_cstr, NULL);
    turi_env_register_native(env, "__inst_Hash_hash_float",   native_hash_f64,  NULL);
    turi_env_register_native(env, "__inst_Hash_hash_float32", native_hash_f32,  NULL);
    /* Raw runtime bridges + count/merge/free. */
    /* map-new: map.tur's body is `malloc({void* hamt}); m->hamt = tur_hamt_new()`
     * -- the simple inline-C executor cannot run the tur_hamt_new() call, so it
     * would hand back a carrier whose hamt slot is garbage (map-free then frees a
     * non-heap pointer).  The Map[K V] carrier is the same single-slot
     * { void* hamt } box as a Set, so native_set_new (set_wrap(tur_hamt_new()))
     * produces the exact layout the map-* natives below read via set_hamt. */
    turi_env_register_native(env, "map-new",         native_set_new,         NULL);
    turi_env_register_native(env, "map-assoc-eq-o",  native_map_assoc_eq_o,  NULL);
    turi_env_register_native(env, "map-get-eq-o",    native_map_get_eq_o,    NULL);
    turi_env_register_native(env, "map-has-eq-o?",   native_map_has_eq_o,    NULL);
    turi_env_register_native(env, "map-dissoc-eq-o", native_map_dissoc_eq_o, NULL);
    turi_env_register_native(env, "map-assoc-eq",    native_map_assoc_eq,    NULL);
    turi_env_register_native(env, "map-get-eq",      native_map_get_eq,      NULL);
    turi_env_register_native(env, "map-has-eq?",     native_map_has_eq,      NULL);
    turi_env_register_native(env, "map-dissoc-eq",   native_map_dissoc_eq,   NULL);
    turi_env_register_native(env, "map-count",       native_map_count,       NULL);
    turi_env_register_native(env, "map-merge",       native_map_merge,       NULL);
    turi_env_register_native(env, "map-free",        native_map_free,        NULL);
    turi_env_register_native(env, "map-eq-raw?",     native_map_eq_raw,      NULL);
    turi_env_register_native(env, "map-eq-raw-k?",   native_map_eq_raw_k,    NULL);
    /* Eq [Map] dispatches through the pure-Turmeric map-eq-driver -> map-eq-loop,
     * which walks the HAMT via hamt/iter-* + map-get-dynamic-as helpers.  Those
     * leaf shims are extern-c calls into the tur_hamt_* runtime (and a few
     * inline-C map-* accessors); the tree-walker has no extern-c symbol table and
     * cannot run the inline-C bodies, so without natives the loop short-circuits
     * to a count-only "true".  Bind the runtime entry points by their C names so
     * the extern-c shims resolve, and override the inline-C map-* accessors by
     * name. */
    turi_env_register_native(env, "tur_hamt_iter_alloc",    native_hamt_iter_alloc,    NULL);
    turi_env_register_native(env, "tur_hamt_iter_destroy",  native_hamt_iter_destroy,  NULL);
    turi_env_register_native(env, "tur_hamt_iter_advance",  native_hamt_iter_advance,  NULL);
    turi_env_register_native(env, "tur_hamt_iter_cur_hash", native_hamt_iter_cur_hash, NULL);
    turi_env_register_native(env, "tur_hamt_iter_cur_key",  native_hamt_iter_cur_key,  NULL);
    turi_env_register_native(env, "tur_hamt_iter_cur_val",  native_hamt_iter_cur_val,  NULL);
    turi_env_register_native(env, "tur_hamt_keyeq",         native_hamt_keyeq,         NULL);
    turi_env_register_native(env, "tur_hamt_get_dynamic",   native_hamt_get_dynamic,   NULL);
    turi_env_register_native(env, "tur_hamt_has_dynamic",   native_hamt_has_dynamic,   NULL);
    turi_env_register_native(env, "map-hamt",               native_map_hamt,           NULL);
    turi_env_register_native(env, "map-iter-cur-val-as",    native_hamt_iter_cur_val,  NULL);
    turi_env_register_native(env, "map-get-dynamic-as",     native_hamt_get_dynamic,   NULL);
    turi_env_register_native(env, "tur-map-homog__", native_map_homog,       NULL);
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

/* Interpreter vec element-tag side table (end-to-end-monomorphization, Vec
 * typed-pointer slice).
 *
 * A native vec stores raw int64 carrier cells in its `data` buffer, so the
 * value tag (TURI_FLOAT / TURI_CSTR / TURI_INT) is lost on push.  Before the
 * Vec typed-pointer migration, `vec-get`'s tyvar return type made the
 * `(:: (vec-get v i) :float)` read-back an EX_ASCRIBE that re-tagged the
 * carrier; now `vec-get` returns the concrete element type, so the elaborator
 * lowers that read to a *transparent* EX_REINTERPRET (tag-preserving by
 * design -- see eval.c) and the re-tag is lost.  Vecs are homogeneous
 * (tur-vec-homog__ enforces a single element type), so we record the element
 * tag once per vec-header pointer here and re-tag on get/pop.  Vecs built by
 * other natives (schema/json int64 buffers) are absent from the table and
 * default to TURI_INT, which is correct -- those only ever hold int carriers.
 * The table is process-lifetime (the interpreter never frees it); entries for
 * freed vecs are harmless (a later vec may reuse the address and overwrite the
 * stale tag on its first push). */
typedef struct { void *key; uint8_t tag; } TurVecTagEnt;
static TurVecTagEnt *g_vec_tag_tab = NULL;
static size_t g_vec_tag_cap = 0;
static size_t g_vec_tag_n   = 0;

static size_t vec_tag_probe(void *key) {
    size_t mask = g_vec_tag_cap - 1;
    size_t h = ((uintptr_t)key >> 4) & mask;
    while (g_vec_tag_tab[h].key && g_vec_tag_tab[h].key != key)
        h = (h + 1) & mask;
    return h;
}
static void vec_tag_set(void *key, uint8_t tag) {
    if (!key) return;
    if (g_vec_tag_n * 2 >= g_vec_tag_cap) {
        size_t old_cap = g_vec_tag_cap;
        TurVecTagEnt *old = g_vec_tag_tab;
        g_vec_tag_cap = old_cap ? old_cap * 2 : 64;
        g_vec_tag_tab = (TurVecTagEnt *)calloc(g_vec_tag_cap, sizeof(TurVecTagEnt));
        g_vec_tag_n = 0;
        for (size_t i = 0; i < old_cap; i++)
            if (old[i].key) {
                g_vec_tag_tab[vec_tag_probe(old[i].key)] = old[i];
                g_vec_tag_n++;
            }
        free(old);
    }
    size_t h = vec_tag_probe(key);
    if (!g_vec_tag_tab[h].key) g_vec_tag_n++;
    g_vec_tag_tab[h].key = key;
    g_vec_tag_tab[h].tag = tag;
}
static uint8_t vec_tag_get(void *key) {
    if (!key || g_vec_tag_cap == 0) return (uint8_t)TURI_INT;
    size_t h = vec_tag_probe(key);
    return g_vec_tag_tab[h].key ? g_vec_tag_tab[h].tag : (uint8_t)TURI_INT;
}
/* Re-tag a raw int64 carrier read from a vec cell according to the vec's
 * recorded homogeneous element tag, so float/cstr elements round-trip with
 * their value tag under --interpret (matching the compiled bit-reinterpret). */
static TuriValue vec_retag_cell(void *vec_key, int64_t cell) {
    switch ((int)vec_tag_get(vec_key)) {
        case TURI_FLOAT: { union { int64_t i; double d; } u; u.i = cell; return turi_float(u.d); }
        case TURI_CSTR:  return turi_cstr((const char *)(intptr_t)cell);
        case TURI_BOOL:  return turi_bool(cell != 0);
        /* A by-value aggregate element (struct/ADT, e.g. (Option int) pushed as a
         * TuriStruct) rides the cell as its pointer; re-tag it so the read-back
         * value is a real TURI_STRUCT and field/.value access works (a bare
         * turi_int would make .is-some/.value misread the struct as an int64
         * carrier).  Guard on non-null to leave a 0/nil carrier alone. */
        case TURI_STRUCT: return cell ? turi_struct_val((TuriStruct *)(intptr_t)cell)
                                      : turi_int(0);
        default:         return turi_int(cell);
    }
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
    return vec_retag_cell(v, data[i]);
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
    /* Record the homogeneous element tag so vec-get/pop re-tag float/cstr/bool
     * carriers (the buffer only holds raw int64 cells). */
    if (a[1].tag != TURI_INT) vec_tag_set(v, (uint8_t)a[1].tag);
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
    return vec_retag_cell(v, data[v[1]]);
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
    if (a[2].tag != TURI_INT) vec_tag_set(v, (uint8_t)a[2].tag);
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

/* Typed-list (stdlib/list.tur) carrier-level length.  A Cons cell is a malloc'd
 * { int64_t head; int64_t tail; }; its pointer is the int64 carrier and tnil is
 * 0 -- exactly the layout list.tur's inline-C uses, so this reproduces
 * list-length (which the tree-walker cannot run as inline-C) by walking the
 * chain.  The carrier ctor + head/tail accessors already exist as native_cons /
 * native_list_head / native_list_tail (registered for the untyped head/tail/cons
 * benchmark surface); they read the same box, so list.tur's typed tcons /
 * list-head / list-tail bind to them too.  The first cell may also be a
 * make-struct Cons TuriStruct (thead/ttail's representation) -- handled here
 * defensively, then the carrier tail is followed. */
static TuriValue native_list_length(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t count = 0;
    TuriValue node = a[0];
    for (;;) {
        if (node.tag == TURI_STRUCT) {
            /* interpreter representation: tcons builds a make-struct Cons whose
             * tail field (index 1) is the next cell -- itself a Cons struct or
             * the int 0 nil sentinel.  Walk the struct chain. */
            count++;
            bool f = false; node = turi_struct_field(node, 1, &f);
            if (!f) break;
            continue;
        }
        /* carrier representation: a malloc'd { head, tail } box chain (the
         * untyped cons/head/tail benchmark surface); 0 is nil. */
        int64_t ptr = node.as_int;
        while (ptr) {
            count++;
            int64_t *cell = (int64_t *)(intptr_t)ptr;
            ptr = cell[1];
        }
        break;
    }
    return turi_int(count);
}

/* Free monad (stdlib/free.tur) natives.  free.tur's free-bind / free-fmap /
 * free-run have #{Unsafe} inline-C bodies that cast the Free ADT carrier to a C
 * tagged-union struct and invoke the ^fat continuation through a tur_poly_fn_t.
 * Under --interpret the Free value is a TuriStruct (the ADT constructor
 * PureFree/Suspend built by adt_ctor_native), and the continuation is a turi
 * closure -- so these natives read the tag by struct name, read the payload
 * (field 0 for both arms), and call the continuation via turi_call. */
static bool free_is_ctor(TuriValue v, const char *name) {
    const char *nm = turi_struct_name(v);
    return nm && strcmp(nm, name) == 0;
}
/* Invoke a ^fat continuation that may arrive as a closure or as an int64
 * carrier holding the TuriClosure* (the carrier-readback case). */
static TuriValue free_call_fat(TuriEnv *env, TuriValue k, TuriValue arg) {
    if (k.tag == TURI_INT && k.as_int != 0) {
        TuriClosure *cl = (TuriClosure *)(intptr_t)k.as_int;
        k.tag = TURI_CLOSURE; k.as_closure = cl;
    }
    return turi_call(env, k, &arg, 1);
}
/* free-bind : (ma  ^fat kont) -- PureFree x => kont(x); Suspend => pass through. */
static TuriValue native_free_bind(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    if (free_is_ctor(a[0], "PureFree")) {
        bool f = false; TuriValue x = turi_struct_field(a[0], 0, &f);
        return free_call_fat(env, a[1], x);
    }
    return a[0]; /* Suspend -- pass through */
}
/* free-run : (^fat interp  free) -- PureFree x => x; Suspend fx => interp(fx). */
static TuriValue native_free_run(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    if (free_is_ctor(a[1], "PureFree")) {
        bool f = false; TuriValue x = turi_struct_field(a[1], 0, &f);
        return x;
    }
    bool f = false; TuriValue inner = turi_struct_field(a[1], 0, &f); /* Suspend fx */
    return free_call_fat(env, a[0], inner);
}

/* str->int-checked (str.tur): inline-C strtoll that returns an Either -- a
 * (Left code) on failure, (Right n) on a clean parse.  Re-implemented as a
 * native that builds the Either ADT via turi_make_struct (a cstr arrives as an
 * int64 carrier holding the char*, same as native_cstr_parse_int reads). */
static TuriValue native_str_to_int_checked(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *cs = (n >= 1) ? (const char *)(intptr_t)a[0].as_int : NULL;
    if (!cs || *cs == '\0') {
        TuriValue f[1] = { turi_int(0) };           /* empty -> Left 0 */
        return turi_make_struct(env, "Left", f, 1);
    }
    char *end = NULL;
    long long v = strtoll(cs, &end, 10);
    if (end == cs || *end != '\0') {
        TuriValue f[1] = { turi_int(1) };           /* garbage -> Left 1 */
        return turi_make_struct(env, "Left", f, 1);
    }
    TuriValue f[1] = { turi_int((int64_t)v) };
    return turi_make_struct(env, "Right", f, 1);
}

/* Grid (stdlib/grid.tur) natives.  grid.tur's ops are inline-C over a
 * { int64_t *data; int width; int height; int cx; int cy; } header (the grid
 * handle is the int64 carrier of that pointer; cells are row-major int64).
 * Re-implemented over the identical layout so the tree-walker can run them. */
typedef struct { int64_t *data; int width; int height; int cx; int cy; } TuriGridRep;
static TuriValue native_grid_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t w = (n >= 1) ? a[0].as_int : 0;
    int64_t h = (n >= 2) ? a[1].as_int : 0;
    TuriGridRep *g = (TuriGridRep *)malloc(sizeof(*g));
    if (!g) return turi_int(0);
    g->width = (int)w; g->height = (int)h; g->cx = 0; g->cy = 0;
    int64_t cells = w * h; if (cells < 0) cells = 0;
    g->data = (int64_t *)calloc((size_t)cells, sizeof(int64_t));
    return turi_int((int64_t)(intptr_t)g);
}
static TuriValue native_grid_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    if (!g || !g->data) return turi_int(0);
    int64_t x = a[1].as_int, y = a[2].as_int;
    return turi_int(g->data[(size_t)(y * g->width + x)]);
}
static TuriValue native_grid_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 4) return turi_nil();
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    if (!g || !g->data) return turi_nil();
    int64_t x = a[1].as_int, y = a[2].as_int, v = a[3].as_int;
    g->data[(size_t)(y * g->width + x)] = v;
    return turi_nil();
}
static TuriValue native_grid_width(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    return turi_int(g ? g->width : 0);
}
static TuriValue native_grid_height(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    return turi_int(g ? g->height : 0);
}
static TuriValue native_grid_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    TuriGridRep *g = (TuriGridRep *)(intptr_t)a[0].as_int;
    if (g) { if (g->data) free(g->data); free(g); }
    return turi_nil();
}

/* SizedBuf (stdlib/sized-buf.tur) natives.  The user-facing sized-buf-* ops are
 * thin pure-turi wrappers over these __sized-buf-*-raw #{Unsafe} inline-C
 * primitives, which operate on a { int64_t len; int64_t *data; } header (the
 * SizedBuf carrier is the int64 of that pointer).  Re-implemented over the
 * identical layout. */
typedef struct { int64_t len; int64_t *data; } TuriSizedBufRep;
static TuriSizedBufRep *sbuf_of(TuriValue v) { return (TuriSizedBufRep *)(intptr_t)v.as_int; }
static TuriValue native_sbuf_new_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t k = (n >= 1) ? a[0].as_int : 0;
    TuriSizedBufRep *b = (TuriSizedBufRep *)malloc(sizeof(*b));
    if (!b) return turi_int(0);
    b->len = k;
    b->data = k > 0 ? (int64_t *)malloc((size_t)k * sizeof(int64_t)) : NULL;
    return turi_int((int64_t)(intptr_t)b);
}
static TuriValue native_sbuf_new_zeroed_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t k = (n >= 1) ? a[0].as_int : 0;
    TuriSizedBufRep *b = (TuriSizedBufRep *)malloc(sizeof(*b));
    if (!b) return turi_int(0);
    b->len = k;
    b->data = k > 0 ? (int64_t *)calloc((size_t)k, sizeof(int64_t)) : NULL;
    return turi_int((int64_t)(intptr_t)b);
}
static TuriValue native_sbuf_free_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    TuriSizedBufRep *b = sbuf_of(a[0]);
    if (b) { if (b->data) free(b->data); free(b); }
    return turi_nil();
}
static TuriValue native_sbuf_len_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]);
    return turi_int(b ? b->len : 0);
}
static TuriValue native_sbuf_get_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t i = a[1].as_int;
    if (b && i >= 0 && i < b->len) return turi_int(b->data[i]);
    fprintf(stderr, "sized-buf-get: index out of bounds\n"); _exit(1);
    return turi_int(0);
}
static TuriValue native_sbuf_set_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t i = a[1].as_int, v = a[2].as_int;
    if (b && i >= 0 && i < b->len) { b->data[i] = v; return a[0]; }
    fprintf(stderr, "sized-buf-set!: index out of bounds\n"); _exit(1);
    return a[0];
}
static TuriValue native_sbuf_fill_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t v = a[1].as_int;
    if (b) for (int64_t i = 0; i < b->len; i++) b->data[i] = v;
    return a[0];
}
static TuriValue native_sbuf_copy_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TuriSizedBufRep *d = sbuf_of(a[0]), *s = sbuf_of(a[1]);
    if (!d || !s) return a[0];
    if (d->len != s->len) {
        fprintf(stderr, "sized-buf-copy!: length mismatch (%lld vs %lld)\n",
                (long long)d->len, (long long)s->len); _exit(1);
    }
    if (d->len > 0) memcpy(d->data, s->data, (size_t)d->len * sizeof(int64_t));
    return a[0];
}
static TuriValue native_sbuf_sum_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]); int64_t s = 0;
    if (b) for (int64_t i = 0; i < b->len; i++) s += b->data[i];
    return turi_int(s);
}
static TuriValue native_sbuf_min_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]);
    if (!b || b->len == 0) { fprintf(stderr, "sized-buf-min: empty buffer\n"); _exit(1); return turi_int(0); }
    int64_t m = b->data[0];
    for (int64_t i = 1; i < b->len; i++) if (b->data[i] < m) m = b->data[i];
    return turi_int(m);
}
static TuriValue native_sbuf_max_raw(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TuriSizedBufRep *b = sbuf_of(a[0]);
    if (!b || b->len == 0) { fprintf(stderr, "sized-buf-max: empty buffer\n"); _exit(1); return turi_int(0); }
    int64_t m = b->data[0];
    for (int64_t i = 1; i < b->len; i++) if (b->data[i] > m) m = b->data[i];
    return turi_int(m);
}

/* vec-eq? -- element-wise Vec equality.  vec.tur's body iterates the {data,len,
 * cap} struct and fat-dispatches the element comparator through a C function
 * pointer, which the simple inline-C executor cannot run; this native re-walks
 * the Vec (int64_t[3] = {data,len,cap}) and invokes the comparator (a turi
 * closure under --interpret) via turi_call, mirroring native_map_eq_raw.  This
 * is what makes recursive container values work -- e.g. a Map[K (Vec V)] whose
 * value comparator bottoms out in vec-eq?. */
static TuriValue native_vec_eq(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    int64_t *v1 = (int64_t *)(intptr_t)a[0].as_int;
    int64_t *v2 = (int64_t *)(intptr_t)a[1].as_int;
    TuriValue cmp = a[2];
    int64_t len1 = v1 ? v1[1] : 0;
    int64_t len2 = v2 ? v2[1] : 0;
    if (len1 != len2) return turi_bool(false);
    int64_t *d1 = v1 ? (int64_t *)(intptr_t)v1[0] : NULL;
    int64_t *d2 = v2 ? (int64_t *)(intptr_t)v2[0] : NULL;
    for (int64_t i = 0; i < len1; i++) {
        TuriValue cargs[2];
        cargs[0].tag = TURI_INT; cargs[0].as_int = d1[i];
        cargs[1].tag = TURI_INT; cargs[1].as_int = d2[i];
        TuriValue rv = turi_call(env, cmp, cargs, 2);
        if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
        bool eq = rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0;
        if (!eq) return turi_bool(false);
    }
    return turi_bool(true);
}

/* -------------------------------------------------------------------------
 * MutableMap (stdlib/mutmap.tur) -- open-addressing hash table.  The module's
 * inline-C ops loop and fat-dispatch the value comparator, which the simple
 * inline-C executor cannot run, so these natives re-implement them over the
 * exact same self-contained layout (no runtime HAMT dependency): the wrapper is
 * { void *storage } and storage is { cap, len, tomb, slots[] } with each slot
 * { tag, hash, key, value }.  Only mutmap-eq? needs the interpreter (it invokes
 * the value comparator -- a turi closure -- via turi_call).
 * ---------------------------------------------------------------------- */
enum { TUR_MM_EMPTY = 0, TUR_MM_OCCUPIED = 1, TUR_MM_DELETED = 2 };
typedef struct { uint8_t tag; int64_t hash; int64_t key; int64_t value; } TurMmSlot;
typedef struct { uint64_t cap; uint64_t len; uint64_t tomb; TurMmSlot slots[]; } TurMmStorage;
typedef struct { void *storage; } TurMmWrap;

static TurMmStorage *mm_storage(TuriValue v) {
    TurMmWrap *m = (TurMmWrap *)(intptr_t)v.as_int;
    return m ? (TurMmStorage *)m->storage : NULL;
}
static TurMmStorage *mm_alloc(uint64_t cap) {
    TurMmStorage *s = (TurMmStorage *)malloc(sizeof(*s) + cap * sizeof(TurMmSlot));
    if (!s) return NULL;
    s->cap = cap; s->len = 0; s->tomb = 0;
    for (uint64_t i = 0; i < cap; i++) {
        s->slots[i].tag = TUR_MM_EMPTY;
        s->slots[i].hash = 0; s->slots[i].key = 0; s->slots[i].value = 0;
    }
    return s;
}
static TuriValue native_mutmap_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    TurMmStorage *s = mm_alloc(16);
    if (!s) return turi_nil();
    TurMmWrap *m = (TurMmWrap *)malloc(sizeof(*m));
    if (!m) { free(s); return turi_nil(); }
    m->storage = s;
    return turi_int((int64_t)(intptr_t)m);
}
static TuriValue native_mutmap_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    return turi_int(s ? (int64_t)s->len : 0);
}
static TuriValue native_mutmap_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 4) return turi_nil();
    TurMmWrap *m = (TurMmWrap *)(intptr_t)a[0].as_int;
    if (!m) return turi_nil();
    TurMmStorage *s = (TurMmStorage *)m->storage;
    int64_t h = a[1].as_int, key = a[2].as_int, val = a[3].as_int;
    /* Resize when load factor (len + tomb) / cap >= 0.75. */
    if ((s->len + s->tomb) * 4 >= s->cap * 3) {
        TurMmStorage *ns = mm_alloc(s->cap * 2);
        if (!ns) return turi_nil();
        uint64_t mask = ns->cap - 1;
        for (uint64_t i = 0; i < s->cap; i++) {
            if (s->slots[i].tag != TUR_MM_OCCUPIED) continue;
            uint64_t idx = ((uint64_t)s->slots[i].hash) & mask;
            while (ns->slots[idx].tag == TUR_MM_OCCUPIED) idx = (idx + 1) & mask;
            ns->slots[idx] = s->slots[i];
            ns->len++;
        }
        free(s);
        m->storage = ns;
        s = ns;
    }
    uint64_t mask = s->cap - 1;
    uint64_t idx = ((uint64_t)h) & mask;
    int64_t first_tomb = -1;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) {
            TurMmSlot *dst = (first_tomb >= 0) ? &s->slots[first_tomb] : slot;
            dst->tag = TUR_MM_OCCUPIED; dst->hash = h; dst->key = key; dst->value = val;
            if (first_tomb >= 0) s->tomb--;
            s->len++;
            return turi_nil();
        }
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key) {
            slot->value = val;
            return turi_nil();
        }
        if (slot->tag == TUR_MM_DELETED && first_tomb < 0) first_tomb = (int64_t)idx;
        idx = (idx + 1) & mask;
    }
}
static TuriValue native_mutmap_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t h = a[1].as_int, key = a[2].as_int;
    uint64_t mask = s->cap - 1, idx = ((uint64_t)h) & mask;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) return turi_int(0);
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key)
            return turi_int(slot->value);
        idx = (idx + 1) & mask;
    }
}
static TuriValue native_mutmap_has(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_bool(false);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_bool(false);
    int64_t h = a[1].as_int, key = a[2].as_int;
    uint64_t mask = s->cap - 1, idx = ((uint64_t)h) & mask;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) return turi_bool(false);
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key)
            return turi_bool(true);
        idx = (idx + 1) & mask;
    }
}
static TuriValue native_mutmap_delete(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 3) return turi_bool(false);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_bool(false);
    int64_t h = a[1].as_int, key = a[2].as_int;
    uint64_t mask = s->cap - 1, idx = ((uint64_t)h) & mask;
    for (;;) {
        TurMmSlot *slot = &s->slots[idx];
        if (slot->tag == TUR_MM_EMPTY) return turi_bool(false);
        if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == key) {
            slot->tag = TUR_MM_DELETED; slot->hash = 0; slot->key = 0; slot->value = 0;
            s->len--; s->tomb++;
            return turi_bool(true);
        }
        idx = (idx + 1) & mask;
    }
}
/* mutmap-cap / mutmap-slot-* -- the slot-iteration accessors the pure-Turmeric
 * `mutmap-eq-loop` (stdlib/mutmap.tur) walks.  They replace the old inline-C
 * `mutmap-eq-storage?` carrier core: equality is now expressed in Turmeric over
 * these reads plus mutmap-has?/mutmap-get, and the value comparator (a turi
 * closure) is invoked directly by the interpreted loop -- no native needs to
 * call back into a comparator.  Each is a single bounds-checked field read. */
static TuriValue native_mutmap_cap(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    return turi_int(s ? (int64_t)s->cap : 0);
}
static TuriValue native_mutmap_slot_occupied(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_bool(false);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_bool(false);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_bool(false);
    return turi_bool(s->slots[i].tag == TUR_MM_OCCUPIED);
}
static TuriValue native_mutmap_slot_hash(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_int(0);
    return turi_int(s->slots[i].hash);
}
static TuriValue native_mutmap_slot_key(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_int(0);
    return turi_int(s->slots[i].key);
}
static TuriValue native_mutmap_slot_value(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2) return turi_int(0);
    TurMmStorage *s = mm_storage(a[0]);
    if (!s) return turi_int(0);
    int64_t i = a[1].as_int;
    if (i < 0 || (uint64_t)i >= s->cap) return turi_int(0);
    return turi_int(s->slots[i].value);
}
/* mutmap-storage-field__ -- read the raw `storage` pointer out of a carrier
 * MutableMap wrapper (the inline-C bridge the public mutmap-eq? uses to feed
 * the by-value storage core).  The interpreter holds the wrapper as a TURI_INT
 * pointer to { void *storage }; return word 0 as a TURI_INT ptr<void>. */
static TuriValue native_mutmap_storage_field(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].tag != TURI_INT || a[0].as_int == 0) return turi_int(0);
    TurMmWrap *m = (TurMmWrap *)(intptr_t)a[0].as_int;
    return turi_int((int64_t)(intptr_t)m->storage);
}
/* mutmap-eq-storage? -- structural equality over two raw storage pointers,
 * invoking the value comparator (a turi closure) via turi_call.  Native
 * override for the inline-C core that the public mutmap-eq? wrapper delegates
 * to (the Eq[MutableMap] instance walks the pure-Turmeric mutmap-eq-loop
 * instead, so it never reaches this native). */
static TuriValue native_mutmap_eq_storage(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 3) return turi_bool(false);
    TurMmStorage *sa = (TurMmStorage *)(intptr_t)a[0].as_int;
    TurMmStorage *sb = (TurMmStorage *)(intptr_t)a[1].as_int;
    TuriValue cmp = a[2];
    if (!sa || !sb || sa->len != sb->len) return turi_bool(false);
    for (uint64_t i = 0; i < sa->cap; i++) {
        if (sa->slots[i].tag != TUR_MM_OCCUPIED) continue;
        int64_t h = sa->slots[i].hash, k = sa->slots[i].key, v_a = sa->slots[i].value;
        uint64_t mask = sb->cap - 1, idx = ((uint64_t)h) & mask;
        bool found = false; int64_t v_b = 0;
        for (;;) {
            TurMmSlot *slot = &sb->slots[idx];
            if (slot->tag == TUR_MM_EMPTY) break;
            if (slot->tag == TUR_MM_OCCUPIED && slot->hash == h && slot->key == k) {
                v_b = slot->value; found = true; break;
            }
            idx = (idx + 1) & mask;
        }
        if (!found) return turi_bool(false);
        TuriValue cargs[2];
        cargs[0].tag = TURI_INT; cargs[0].as_int = v_a;
        cargs[1].tag = TURI_INT; cargs[1].as_int = v_b;
        TuriValue rv = turi_call(env, cmp, cargs, 2);
        if (turi_is_error(rv) || env->throwing) return rv;  /* propagate callback error */
        if (!(rv.tag == TURI_BOOL ? rv.as_bool : rv.as_int != 0)) return turi_bool(false);
    }
    return turi_bool(true);
}
static TuriValue native_mutmap_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || a[0].tag != TURI_INT || a[0].as_int == 0) return turi_nil();
    TurMmWrap *m = (TurMmWrap *)(intptr_t)a[0].as_int;
    if (m->storage) free(m->storage);
    free(m);
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
/* tail: return the next pointer field of the first cons cell.  Handles both the
 * interpreter struct representation (tcons -> make-struct Cons; tail is field 1)
 * and the carrier { head, tail } box (untyped cons surface). */
static TuriValue native_list_tail(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    if (a[0].tag == TURI_STRUCT) {
        bool f = false; TuriValue t = turi_struct_field(a[0], 1, &f);
        return f ? t : turi_int(0);
    }
    if (a[0].as_int == 0) return turi_int(0);
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
/* head: return the value field of the first cons cell.  Handles both the
 * interpreter struct representation (tcons -> make-struct Cons; head is field 0,
 * which may carry any TuriValue) and the carrier { head, tail } box. */
static TuriValue native_list_head(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    if (a[0].tag == TURI_STRUCT) {
        bool f = false; TuriValue h = turi_struct_field(a[0], 0, &f);
        return f ? h : turi_nil();
    }
    if (a[0].as_int == 0) return turi_nil();
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
/* float->int: truncate toward zero (math.tur's inline-C `(int64_t)x`). */
static TuriValue native_float_to_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double v = (n > 0) ? a[0].as_float : 0.0;
    return turi_int((int64_t)v);
}
/* sqrt / floor: math.tur's libm wrappers (inline-C the tree-walker cannot run). */
static TuriValue native_math_sqrt(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = sqrt(x); return rv;
}
static TuriValue native_math_floor(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    double x = (n > 0) ? a[0].as_float : 0.0;
    TuriValue rv = {0}; rv.tag = TURI_FLOAT; rv.as_float = floor(x); return rv;
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

/* stdlib/time.tur natives.  time.tur's bodies are inline-C (nanosleep, a
 * __tur_mock_cap singleton struct), which the interpreter cannot run.  The
 * Mock-Time capability is the only deterministic, printable clock; model it as
 * a one-word heap cell holding the current mock time (ms).  sleep-ms is a no-op
 * under --interpret (deterministic; the interpreter has no wall clock to
 * advance), matching the fixtures that only assert on mock time. */
static TuriValue native_time_mock_create(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    int64_t *cell = (int64_t *)malloc(sizeof(int64_t));
    if (!cell) return turi_int(0);
    cell[0] = 0;
    return turi_int((int64_t)(intptr_t)cell);
}
static TuriValue native_time_mock_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_time_mock_set(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 2 && a[0].as_int) ((int64_t *)(intptr_t)a[0].as_int)[0] = a[1].as_int;
    return turi_nil();
}
static TuriValue native_time_mock_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 1 && a[0].as_int) return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
    return turi_int(0);
}
static TuriValue native_time_sleep_ms(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    return turi_nil();  /* deterministic no-op under --interpret */
}

/* stdlib/reactor.tur: the Reactor handle ops are thin shims over the tur_reactor_*
 * runtime (src/async/reactor.c) -- reactor-new's inline-C body and reactor-free/
 * reactor-poll's extern-c calls.  The tree-walker has no extern-c symbol table
 * and cannot run the inline-C, so bind the runtime entry points by their C names
 * (and reactor-new by name, overriding its inline-C body).  The handle is a
 * pointer carried as int64, matching the compiled :ptr<void> carrier. */
extern void *tur_reactor_new(void);
extern void  tur_reactor_free(void *r);
extern int64_t tur_reactor_poll(void *r, int64_t timeout_ms);
static TuriValue native_reactor_new(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)a; (void)n; (void)ud;
    return turi_int((int64_t)(intptr_t)tur_reactor_new());
}
static TuriValue native_reactor_free(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n >= 1 && a[0].as_int) tur_reactor_free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_reactor_poll(TuriEnv *e, TuriValue *a, uint32_t n, void *ud) {
    (void)e; (void)ud;
    if (n < 2 || a[0].as_int == 0) return turi_int(0);
    return turi_int(tur_reactor_poll((void *)(intptr_t)a[0].as_int, a[1].as_int));
}

static void wk_register_stdlib_natives(TuriEnv *env) {
    /* stdlib/time.tur (inline-C; Mock-Time capability + sleep-ms) */
    turi_env_register_native(env, "Mock-Time",       native_time_mock_create, NULL);
    turi_env_register_native(env, "Mock-Time-free",  native_time_mock_free,   NULL);
    turi_env_register_native(env, "mock-time-set",   native_time_mock_set,    NULL);
    turi_env_register_native(env, "mock-time-get",   native_time_mock_get,    NULL);
    turi_env_register_native(env, "sleep-ms",        native_time_sleep_ms,    NULL);
    /* Option/some/none */
    turi_env_register_native(env, "some",            native_some,            NULL);
    turi_env_register_native(env, "option-eq?",      native_option_eq,       NULL);
    turi_env_register_native(env, "none",            native_none,            NULL);
    turi_env_register_native(env, "some?",           native_some_pred,       NULL);
    turi_env_register_native(env, "option-unwrap",   native_option_unwrap,   NULL);
    turi_env_register_native(env, "option-value",    native_option_value,    NULL);
    turi_env_register_native(env, "option-free",     native_option_free,     NULL);
    turi_env_register_native(env, "option-unwrap-or",native_option_unwrap_or,NULL);
    /* option.tur names this inline-C op `unwrap-or` (not `option-unwrap-or`); the
     * override only fires when registered under the real function name. */
    turi_env_register_native(env, "unwrap-or",       native_option_unwrap_or,NULL);
    /* unwrap-or-carrier (option.tur Track A shim) is inline-C with identical
     * semantics to unwrap-or: extract the carrier Option's value or return the
     * default.  Register the same native so --interpret does not trip over the
     * inline-C body. */
    turi_env_register_native(env, "unwrap-or-carrier",native_option_unwrap_or,NULL);
    turi_env_register_native(env, "option-must",     native_option_must,     NULL);
    turi_env_register_native(env, "option-expect",   native_option_expect,   NULL);
    turi_env_register_native(env, "option-map",      native_option_map,      NULL);
    /* kleisli.tur: fat-closure carrier apply (TUR_APPLY1) -> turi_call. */
    turi_env_register_native(env, "k-apply-raw",     native_k_apply_raw,     NULL);
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
    turi_env_register_native(env, "result-eq?",      native_result_eq,       NULL);
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
    turi_env_register_native(env, "set-new",         native_set_new,         NULL);
    turi_env_register_native(env, "set-add",         native_set_add,         NULL);
    turi_env_register_native(env, "set-remove",      native_set_remove,      NULL);
    turi_env_register_native(env, "set-member?",     native_set_member,      NULL);
    turi_env_register_native(env, "set-count",       native_set_count,       NULL);
    turi_env_register_native(env, "set-free",        native_set_free,        NULL);
    turi_env_register_native(env, "set-union",       native_set_union,       NULL);
    turi_env_register_native(env, "set-intersect",   native_set_intersect,   NULL);
    turi_env_register_native(env, "set-diff",        native_set_diff,        NULL);
    turi_env_register_native(env, "set-eq?",         native_set_eq,          NULL);
    turi_env_register_native(env, "set-eq-cmp?",     native_set_eq_cmp,      NULL);
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
    /* tur-vec-homog__ is vec.tur's compile-time element-homogeneity check, an
     * inline-C no-op (`(void)a;(void)b;`) the vec-of macro emits between each
     * element pair.  The simple inline-C executor cannot run a bodyless void,
     * so vec-of (and [...] data literals) failed under --interpret with
     * "inline-C not supported"; a no-op native restores them. */
    turi_env_register_native(env, "tur-vec-homog__", native_json_free,       NULL);
    turi_env_register_native(env, "vec-len",         native_vec_len,         NULL);
    turi_env_register_native(env, "vec-capacity",    native_vec_capacity,    NULL);
    turi_env_register_native(env, "vec-get",         native_vec_get,         NULL);
    turi_env_register_native(env, "vec-push!",       native_vec_push,        NULL);
    turi_env_register_native(env, "vec-push-ptr!",   native_vec_push,        NULL);
    turi_env_register_native(env, "vec-pop!",        native_vec_pop,         NULL);
    turi_env_register_native(env, "vec-set!",        native_vec_set,         NULL);
    turi_env_register_native(env, "vec-free",        native_vec_free,        NULL);
    turi_env_register_native(env, "vec-eq?",         native_vec_eq,          NULL);
    /* Typed-list (list.tur) carrier-level ops: inline-C bodies the interpreter
     * cannot run, bound to the { head, tail } cons-cell box natives.  tcons /
     * list-head / list-tail reuse the existing native_cons / native_list_head /
     * native_list_tail (same box layout as the untyped head/tail/cons surface);
     * list-length is new. */
    turi_env_register_native(env, "tcons",           native_cons,            NULL);
    turi_env_register_native(env, "list-head",       native_list_head,       NULL);
    turi_env_register_native(env, "list-tail",       native_list_tail,       NULL);
    turi_env_register_native(env, "list-length",     native_list_length,     NULL);
    /* stdlib/reactor.tur handle ops (inline-C / extern-c over tur_reactor_*). */
    turi_env_register_native(env, "reactor-new",      native_reactor_new,    NULL);
    turi_env_register_native(env, "tur_reactor_new",  native_reactor_new,    NULL);
    turi_env_register_native(env, "tur_reactor_free", native_reactor_free,   NULL);
    turi_env_register_native(env, "tur_reactor_poll", native_reactor_poll,   NULL);
    /* Free monad (free.tur): #{Unsafe} inline-C bodies re-implemented over the
     * PureFree/Suspend TuriStruct + turi_call continuation. */
    turi_env_register_native(env, "free-bind",       native_free_bind,       NULL);
    turi_env_register_native(env, "free-run",        native_free_run,        NULL);
    turi_env_register_native(env, "str->int-checked", native_str_to_int_checked, NULL);
    /* Grid (grid.tur): raw-buffer inline-C re-implemented over the matching
     * { data, width, height, cx, cy } header. */
    turi_env_register_native(env, "grid-new",        native_grid_new,        NULL);
    turi_env_register_native(env, "grid-get",        native_grid_get,        NULL);
    turi_env_register_native(env, "grid-set!",       native_grid_set,        NULL);
    turi_env_register_native(env, "grid-width",      native_grid_width,      NULL);
    turi_env_register_native(env, "grid-height",     native_grid_height,     NULL);
    turi_env_register_native(env, "grid-free",       native_grid_free,       NULL);
    /* SizedBuf (sized-buf.tur): __sized-buf-*-raw inline-C over { len, data }. */
    turi_env_register_native(env, "__sized-buf-new-raw",        native_sbuf_new_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-new-zeroed-raw", native_sbuf_new_zeroed_raw, NULL);
    turi_env_register_native(env, "__sized-buf-free-raw",       native_sbuf_free_raw,       NULL);
    turi_env_register_native(env, "__sized-buf-len-raw",        native_sbuf_len_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-get-raw",        native_sbuf_get_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-set!-raw",       native_sbuf_set_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-fill!-raw",      native_sbuf_fill_raw,       NULL);
    turi_env_register_native(env, "__sized-buf-copy!-raw",      native_sbuf_copy_raw,       NULL);
    turi_env_register_native(env, "__sized-buf-sum-raw",        native_sbuf_sum_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-min-raw",        native_sbuf_min_raw,        NULL);
    turi_env_register_native(env, "__sized-buf-max-raw",        native_sbuf_max_raw,        NULL);
    turi_env_register_native(env, "mutmap-new",      native_mutmap_new,      NULL);
    turi_env_register_native(env, "mutmap-len",      native_mutmap_len,      NULL);
    turi_env_register_native(env, "mutmap-set!",     native_mutmap_set,      NULL);
    turi_env_register_native(env, "mutmap-get",      native_mutmap_get,      NULL);
    turi_env_register_native(env, "mutmap-has?",     native_mutmap_has,      NULL);
    turi_env_register_native(env, "mutmap-delete!",  native_mutmap_delete,   NULL);
    /* mutmap-eq? / mutmap-eq-loop are pure-Turmeric; the slot accessors they
     * walk are native (the simple inline-C executor reads them, but registering
     * keeps the interpreter on the same self-contained open-addressing layout). */
    turi_env_register_native(env, "mutmap-cap",            native_mutmap_cap,           NULL);
    turi_env_register_native(env, "mutmap-slot-occupied?", native_mutmap_slot_occupied, NULL);
    turi_env_register_native(env, "mutmap-slot-hash",      native_mutmap_slot_hash,     NULL);
    turi_env_register_native(env, "mutmap-slot-key",       native_mutmap_slot_key,      NULL);
    turi_env_register_native(env, "mutmap-slot-value",     native_mutmap_slot_value,    NULL);
    /* Public mutmap-eq? still delegates to the inline-C storage core. */
    turi_env_register_native(env, "mutmap-storage-field__", native_mutmap_storage_field, NULL);
    turi_env_register_native(env, "mutmap-eq-storage?",     native_mutmap_eq_storage,    NULL);
    turi_env_register_native(env, "mutmap-free",     native_mutmap_free,     NULL);
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
    turi_env_register_native(env, "float->int",        native_float_to_int,    NULL);
    turi_env_register_native(env, "sqrt",              native_math_sqrt,       NULL);
    turi_env_register_native(env, "floor",             native_math_floor,      NULL);
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
 * R1 (turi-interpret-flip-residual-plan): comonad.tur native shims.
 *
 * comonad.tur's Identity / Pair cells are malloc'd heap structs whose pointer
 * rides the int64 carrier (the compiled ABI).  The inline-C accessors
 * (__identity_new/_get/_extract/_duplicate, __pair_new/_env/_val/_extract/
 * _duplicate) are re-implemented over the identical layout; the *_extend ops
 * are pure-turi (they call (f wa) then a native constructor) and need no shim.
 *   Identity = { int64_t value; }
 *   Pair     = Tuple2 { int64_t e1 (env); int64_t e2 (val); }
 * ---------------------------------------------------------------------- */
typedef struct { int64_t e1; int64_t e2; } WkTuple2;

static TuriValue wk_int_ptr(void *p) {
    TuriValue v = {0}; v.tag = TURI_INT; v.as_int = (int64_t)(intptr_t)p; return v;
}

static TuriValue native_identity_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    return wk_int_ptr(p);
}
static TuriValue native_identity_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    int64_t *p = (int64_t *)(intptr_t)a[0].as_int;
    return p ? turi_int(*p) : turi_int(0);
}
static TuriValue native_identity_duplicate(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    /* duplicate w = Identity w (wrap the original carrier) */
    int64_t *p = (int64_t *)malloc(sizeof(int64_t));
    if (!p) return turi_nil();
    *p = (n > 0) ? a[0].as_int : 0;
    return wk_int_ptr(p);
}
static TuriValue native_pair_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    WkTuple2 *p = (WkTuple2 *)malloc(sizeof(WkTuple2));
    if (!p) return turi_nil();
    p->e1 = (n > 0) ? a[0].as_int : 0;
    p->e2 = (n > 1) ? a[1].as_int : 0;
    return wk_int_ptr(p);
}
static TuriValue native_pair_env(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    WkTuple2 *p = (WkTuple2 *)(intptr_t)a[0].as_int;
    return p ? turi_int(p->e1) : turi_int(0);
}
static TuriValue native_pair_val(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(0);
    WkTuple2 *p = (WkTuple2 *)(intptr_t)a[0].as_int;
    return p ? turi_int(p->e2) : turi_int(0);
}
static TuriValue native_pair_duplicate(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_nil();
    WkTuple2 *src = (WkTuple2 *)(intptr_t)a[0].as_int;
    if (!src) return turi_nil();
    WkTuple2 *p = (WkTuple2 *)malloc(sizeof(WkTuple2));
    if (!p) return turi_nil();
    p->e1 = src->e1;
    p->e2 = a[0].as_int;  /* inner = the original cell */
    return wk_int_ptr(p);
}

static void wk_register_comonad_natives(TuriEnv *env) {
    turi_env_register_native(env, "__identity_new",       native_identity_new,       NULL);
    turi_env_register_native(env, "__identity_get",       native_identity_get,       NULL);
    turi_env_register_native(env, "__identity_extract",   native_identity_get,       NULL);
    turi_env_register_native(env, "__identity_duplicate", native_identity_duplicate, NULL);
    turi_env_register_native(env, "__pair_new",           native_pair_new,           NULL);
    turi_env_register_native(env, "__pair_env",           native_pair_env,           NULL);
    turi_env_register_native(env, "__pair_val",           native_pair_val,           NULL);
    turi_env_register_native(env, "__pair_extract",       native_pair_val,           NULL);
    turi_env_register_native(env, "__pair_duplicate",     native_pair_duplicate,     NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): mutex.tur native shims.
 *
 * Faithful pthread_mutex_t over the int64 carrier (the compiled ABI lowers
 * Mutex to :ptr<void>).  The interpreter is single-threaded for these fixtures,
 * but a real mutex keeps lock/unlock/try-lock/free semantics honest.
 * ---------------------------------------------------------------------- */
static TuriValue native_mutex_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!m) return turi_nil();
    pthread_mutex_init(m, NULL);
    return wk_int_ptr(m);
}
static TuriValue native_mutex_lock(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) pthread_mutex_lock((pthread_mutex_t *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_mutex_unlock(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) pthread_mutex_unlock((pthread_mutex_t *)(intptr_t)a[0].as_int);
    return turi_nil();
}
static TuriValue native_mutex_try_lock(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_bool(false);
    return turi_bool(pthread_mutex_trylock((pthread_mutex_t *)(intptr_t)a[0].as_int) == 0);
}
static TuriValue native_mutex_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) {
        pthread_mutex_t *m = (pthread_mutex_t *)(intptr_t)a[0].as_int;
        pthread_mutex_destroy(m);
        free(m);
    }
    return turi_nil();
}

static void wk_register_mutex_natives(TuriEnv *env) {
    turi_env_register_native(env, "mutex-new",      native_mutex_new,      NULL);
    turi_env_register_native(env, "mutex-lock",     native_mutex_lock,     NULL);
    turi_env_register_native(env, "mutex-unlock",   native_mutex_unlock,   NULL);
    turi_env_register_native(env, "mutex-try-lock", native_mutex_try_lock, NULL);
    turi_env_register_native(env, "mutex-free",     native_mutex_free,     NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): future.tur FutureCell native shims.
 *
 * Layout-exact replica of future.tur's FutureCell so the refcounted
 * Promise/Future split-free semantics are honest under --interpret:
 * future-handle bumps refcount; future-cell-free / future-free drop a ref and
 * tear down at zero (the historical double-free hazard the fixture guards
 * against).  promise-new / promise-free are pure-turi wrappers over
 * future-cell-new / future-cell-free, so only these four need natives.
 * ---------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  ready;
    int64_t value;
    int64_t exn;
    bool is_set;
    bool is_ok;
    int64_t refcount;
} WkFutureCell;

static TuriValue native_future_cell_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    WkFutureCell *cell = (WkFutureCell *)malloc(sizeof(WkFutureCell));
    if (!cell) return turi_nil();
    pthread_mutex_init(&cell->lock, NULL);
    pthread_cond_init(&cell->ready, NULL);
    cell->value = 0; cell->exn = 0; cell->is_set = false; cell->is_ok = false;
    cell->refcount = 1;
    return wk_int_ptr(cell);
}
static TuriValue native_future_handle(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    __atomic_add_fetch(&fc->refcount, 1, __ATOMIC_ACQ_REL);
    return wk_int_ptr(fc);
}
static TuriValue native_future_cell_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    if (__atomic_sub_fetch(&fc->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        pthread_mutex_destroy(&fc->lock);
        pthread_cond_destroy(&fc->ready);
        free(fc);
    }
    return turi_nil();
}

static TuriValue native_promise_fulfill(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&fc->lock);
    if (!fc->is_set) {
        fc->value = (n > 1) ? a[1].as_int : 0;
        fc->is_set = true;
        fc->is_ok = true;
        pthread_cond_broadcast(&fc->ready);
    }
    pthread_mutex_unlock(&fc->lock);
    return turi_nil();
}
static TuriValue native_future_done(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_bool(false);
    WkFutureCell *fc = (WkFutureCell *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&fc->lock);
    bool done = fc->is_set;
    pthread_mutex_unlock(&fc->lock);
    return turi_bool(done);
}

static void wk_register_future_natives(TuriEnv *env) {
    turi_env_register_native(env, "future-cell-new",  native_future_cell_new,  NULL);
    turi_env_register_native(env, "future-handle",    native_future_handle,    NULL);
    turi_env_register_native(env, "future-cell-free", native_future_cell_free, NULL);
    turi_env_register_native(env, "future-free",      native_future_cell_free, NULL);
    turi_env_register_native(env, "promise-fulfill",  native_promise_fulfill,  NULL);
    turi_env_register_native(env, "future-done?",     native_future_done,      NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): chan.tur bounded-channel shims.
 *
 * The interpreter is single-threaded, so a real cond-var-blocking channel
 * would only ever deadlock; the faithful interpreter semantics for the
 * (single-thread) fixtures is a plain bounded ring buffer guarded by a mutex.
 * One WkChan backs both the sync (chan-*) and async (async-chan-*) surfaces;
 * the natives own both the allocation and every access, so they need not match
 * the compiled ChanBlock layout (which also carries select waiters).
 * ---------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t lock;
    int64_t *buf;
    int64_t head, tail, count, cap;
} WkChan;

static TuriValue native_chan_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t cap = (n > 0) ? a[0].as_int : 0;
    if (cap < 1) cap = 1;
    WkChan *ch = (WkChan *)malloc(sizeof(WkChan));
    if (!ch) return turi_nil();
    ch->buf = (int64_t *)malloc(sizeof(int64_t) * (size_t)cap);
    if (!ch->buf) { free(ch); return turi_nil(); }
    pthread_mutex_init(&ch->lock, NULL);
    ch->head = ch->tail = ch->count = 0; ch->cap = cap;
    return wk_int_ptr(ch);
}
/* push; returns true if there was room */
static bool wk_chan_push(WkChan *ch, int64_t v) {
    bool ok = false;
    pthread_mutex_lock(&ch->lock);
    if (ch->count < ch->cap) {
        ch->buf[ch->tail] = v;
        ch->tail = (ch->tail + 1) % ch->cap;
        ch->count++;
        ok = true;
    }
    pthread_mutex_unlock(&ch->lock);
    return ok;
}
/* pop into *out; returns true if a value was available */
static bool wk_chan_pop(WkChan *ch, int64_t *out) {
    bool ok = false;
    pthread_mutex_lock(&ch->lock);
    if (ch->count > 0) {
        *out = ch->buf[ch->head];
        ch->head = (ch->head + 1) % ch->cap;
        ch->count--;
        ok = true;
    }
    pthread_mutex_unlock(&ch->lock);
    return ok;
}
static TuriValue native_chan_send(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 2 && a[0].as_int) wk_chan_push((WkChan *)(intptr_t)a[0].as_int, a[1].as_int);
    return turi_nil();
}
static TuriValue native_chan_recv(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = 0;
    if (n >= 1 && a[0].as_int) wk_chan_pop((WkChan *)(intptr_t)a[0].as_int, &v);
    return turi_int(v);
}
static TuriValue native_chan_try_send(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2 || !a[0].as_int) return turi_bool(false);
    return turi_bool(wk_chan_push((WkChan *)(intptr_t)a[0].as_int, a[1].as_int));
}
static TuriValue native_chan_try_recv(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = 0;
    if (n >= 1 && a[0].as_int) wk_chan_pop((WkChan *)(intptr_t)a[0].as_int, &v);
    return turi_int(v);
}
static TuriValue native_chan_count(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    WkChan *ch = (WkChan *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&ch->lock);
    int64_t c = ch->count;
    pthread_mutex_unlock(&ch->lock);
    return turi_int(c);
}
static TuriValue native_chan_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n >= 1 && a[0].as_int) {
        WkChan *ch = (WkChan *)(intptr_t)a[0].as_int;
        pthread_mutex_destroy(&ch->lock);
        free(ch->buf);
        free(ch);
    }
    return turi_nil();
}

/* schan.tur synchronous session channels: same ring buffer (SChanBlock matches
 * WkChan's leading fields), plus a one-int64 cell.  send/recv return the channel
 * carrier (the protocol continuation rides the same pointer); recv writes the
 * popped value into *cell. */
static TuriValue native_schan_send(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2 || !a[0].as_int) return turi_int(0);
    wk_chan_push((WkChan *)(intptr_t)a[0].as_int, a[1].as_int);
    return a[0];  /* SChan R continuation */
}
static TuriValue native_schan_recv(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 2 || !a[0].as_int) return turi_int(0);
    int64_t v = 0;
    wk_chan_pop((WkChan *)(intptr_t)a[0].as_int, &v);
    if (a[1].as_int) *(int64_t *)(intptr_t)a[1].as_int = v;  /* write into cell */
    return a[0];  /* SChan R continuation */
}
static TuriValue native_schan_cell_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    int64_t *c = (int64_t *)malloc(sizeof(int64_t));
    if (!c) return turi_nil();
    *c = 0;
    return wk_int_ptr(c);
}
static TuriValue native_schan_cell_get(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    return turi_int(*(int64_t *)(intptr_t)a[0].as_int);
}
static TuriValue native_schan_cell_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

static void wk_register_chan_natives(TuriEnv *env) {
    turi_env_register_native(env, "chan-new",            native_chan_new,      NULL);
    turi_env_register_native(env, "chan-send",           native_chan_send,     NULL);
    turi_env_register_native(env, "chan-recv",           native_chan_recv,     NULL);
    turi_env_register_native(env, "chan-free",           native_chan_free,     NULL);
    turi_env_register_native(env, "async-chan-new",      native_chan_new,      NULL);
    turi_env_register_native(env, "async-chan-try-send", native_chan_try_send, NULL);
    turi_env_register_native(env, "async-chan-try-recv", native_chan_try_recv, NULL);
    turi_env_register_native(env, "async-chan-count",    native_chan_count,    NULL);
    turi_env_register_native(env, "async-chan-free",     native_chan_free,     NULL);
    /* schan.tur synchronous session channels (SChanBlock == WkChan prefix). */
    turi_env_register_native(env, "schan-new",           native_chan_new,        NULL);
    turi_env_register_native(env, "schan-send",          native_schan_send,      NULL);
    turi_env_register_native(env, "schan-recv",          native_schan_recv,      NULL);
    turi_env_register_native(env, "schan-close",         native_chan_free,       NULL);
    turi_env_register_native(env, "schan-cell-new",      native_schan_cell_new,  NULL);
    turi_env_register_native(env, "schan-cell-get",      native_schan_cell_get,  NULL);
    turi_env_register_native(env, "schan-cell-free",     native_schan_cell_free, NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): process.tur + fs.tur OS-handle shims.
 *
 * Faithful syscalls: process/spawn forks+execvp's (ChildHandle = pid), wait
 * reaps it; fs/tmpfile mkstemp's into a { path, fd } pair (TmpFile), with
 * borrow-accessors and a close+free.  argv is a cons list of cstr pointers
 * ({head,tail} cells), matching the compiled inline-C walk.
 * ---------------------------------------------------------------------- */
static TuriValue native_process_spawn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    const char *path = (n > 0 && a[0].tag == TURI_CSTR) ? a[0].as_cstr : NULL;
    if (!path) return turi_int(-1);
    int64_t argv = (n > 1) ? a[1].as_int : 0;
    int argc = 0;
    for (int64_t t = argv; t; t = ((int64_t *)(intptr_t)t)[1]) argc++;
    char **args = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!args) return turi_int(-1);
    { int i = 0; for (int64_t t = argv; t; t = ((int64_t *)(intptr_t)t)[1])
        args[i++] = (char *)(intptr_t)((int64_t *)(intptr_t)t)[0]; }
    args[argc] = NULL;
    pid_t pid = fork();
    if (pid < 0) { free(args); return turi_int(-1); }
    if (pid == 0) { execvp(path, args); _exit(127); }
    free(args);
    return turi_int((int64_t)pid);
}
static TuriValue native_process_wait(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1) return turi_int(-1);
    int status = 0;
    if (waitpid((pid_t)a[0].as_int, &status, 0) < 0) return turi_int(-1);
    if (WIFEXITED(status)) return turi_int((int64_t)WEXITSTATUS(status));
    return turi_int(-1);
}
static TuriValue native_fs_tmpfile(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    char *tmpl = strdup("/tmp/tur_XXXXXX");
    if (!tmpl) return turi_int(0);
    int fd = mkstemp(tmpl);
    if (fd < 0) { free(tmpl); return turi_int(0); }
    int64_t *pair = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!pair) { close(fd); free(tmpl); return turi_int(0); }
    pair[0] = (int64_t)(intptr_t)tmpl;
    pair[1] = (int64_t)fd;
    return wk_int_ptr(pair);
}
static TuriValue native_fs_tmpfile_path(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    return turi_cstr((const char *)(intptr_t)((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_fs_tmpfile_fd(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[1]);
}
static TuriValue native_fs_tmpfile_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    int64_t *pair = (int64_t *)(intptr_t)a[0].as_int;
    close((int)pair[1]);
    free((void *)(intptr_t)pair[0]);
    free(pair);
    return turi_nil();
}

/* -------------------------------------------------------------------------
 * R3 (turi-interpret-flip-residual-plan): backtrack.tur cons-stream monad.
 *
 * A Backtrack value is a linked list of results -- malloc'd { int64 value;
 * int64 next; } cells (next=0 is nil), the exact compiled layout.  All the
 * typeclass instances (Functor/Applicative/Monad/Alternative) and the *-raw
 * workers are pure-turi wrappers over these inline-C primitives, so shimming
 * the primitives makes the whole surface interpretable.  mbind/bt-apply-fat
 * invoke the continuation via turi_call (the interpreter hands it a closure
 * where the compiled path used a fat-closure thunk).
 * ---------------------------------------------------------------------- */
typedef struct { int64_t value; int64_t next; } WkBtCell;

static int64_t wk_bt_cell(int64_t value, int64_t next) {
    WkBtCell *c = (WkBtCell *)malloc(sizeof(WkBtCell));
    if (!c) return 0;
    c->value = value; c->next = next;
    return (int64_t)(intptr_t)c;
}
static TuriValue native_bt_nil(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud; return turi_int(0);
}
static TuriValue native_bt_cons(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    int64_t nx = (n > 1) ? a[1].as_int : 0;
    return turi_int(wk_bt_cell(v, nx));
}
static TuriValue native_bt_mreturn(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    return turi_int(wk_bt_cell((n > 0) ? a[0].as_int : 0, 0));
}
/* mplus: copy xs's spine, then point its tail at ys (ys shared). */
static TuriValue native_bt_mplus(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t xs = (n > 0) ? a[0].as_int : 0;
    int64_t ys = (n > 1) ? a[1].as_int : 0;
    WkBtCell *xc = (WkBtCell *)(intptr_t)xs;
    if (!xc) return turi_int(ys);
    int64_t head = 0; WkBtCell *tail = NULL;
    while (xc) {
        int64_t nc = wk_bt_cell(xc->value, 0);
        if (tail) tail->next = nc; else head = nc;
        tail = (WkBtCell *)(intptr_t)nc;
        xc = (WkBtCell *)(intptr_t)xc->next;
    }
    if (tail) tail->next = ys;
    return turi_int(head);
}
/* mbind: concatMap -- for each value, turi_call fn to get a sub-stream, flatten
 * in order (build reversed, then reverse, matching the compiled body). */
static TuriValue native_bt_mbind(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    WkBtCell *cell = (WkBtCell *)(intptr_t)a[0].as_int;
    int64_t rev = 0;
    while (cell) {
        TuriValue arg = turi_int(cell->value);
        TuriValue sub = turi_call(env, a[1], &arg, 1);
        if (turi_is_error(sub) || env->throwing) return sub;  /* propagate callback error */
        WkBtCell *sc = (WkBtCell *)(intptr_t)sub.as_int;
        while (sc) {
            rev = wk_bt_cell(sc->value, rev);
            sc = (WkBtCell *)(intptr_t)sc->next;
        }
        cell = (WkBtCell *)(intptr_t)cell->next;
    }
    int64_t result = 0;
    WkBtCell *r = (WkBtCell *)(intptr_t)rev;
    while (r) {
        WkBtCell *tmp = (WkBtCell *)(intptr_t)r->next;
        r->next = result;
        result = (int64_t)(intptr_t)r;
        r = tmp;
    }
    return turi_int(result);
}
static TuriValue native_bt_length(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t cnt = 0;
    WkBtCell *c = (n > 0) ? (WkBtCell *)(intptr_t)a[0].as_int : NULL;
    while (c) { cnt++; c = (WkBtCell *)(intptr_t)c->next; }
    return turi_int(cnt);
}
static TuriValue native_bt_print(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    WkBtCell *c = (n > 0) ? (WkBtCell *)(intptr_t)a[0].as_int : NULL;
    while (c) { printf("%lld\n", (long long)c->value); c = (WkBtCell *)(intptr_t)c->next; }
    return turi_nil();
}
static TuriValue native_bt_apply_fat(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 2) return turi_int(0);
    return turi_call(env, a[0], &a[1], 1);
}

/* R6 (turi-interpret-flip-residual-plan): tuple.tur's Eq[Tuple2] instance is
 * pure-turi but bottoms out in `tuple2-eq-carrier?`, an inline-C body that casts
 * each Tuple2 to a { e1, e2 } struct and calls two element comparators through C
 * function pointers.  Re-implement it as a native: read both fields (a make-
 * struct Tuple2 is a TuriStruct; a carrier int points at an int64[2] {e1,e2}),
 * and invoke the comparators (turi closures) via turi_call. */
static int64_t wk_tuple2_field(TuriValue t, int idx) {
    bool found = false;
    TuriValue f = turi_struct_field(t, (uint32_t)idx, &found);
    if (found) return (f.tag == TURI_BOOL) ? (f.as_bool ? 1 : 0) : f.as_int;
    int64_t *p = (int64_t *)(intptr_t)t.as_int;
    return p ? p[idx] : 0;
}
static TuriValue native_tuple2_eq_carrier(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)ud;
    if (n < 4) return turi_bool(false);
    int64_t a1 = wk_tuple2_field(a[0], 0), a2 = wk_tuple2_field(a[0], 1);
    int64_t b1 = wk_tuple2_field(a[1], 0), b2 = wk_tuple2_field(a[1], 1);
    TuriValue c1args[2] = { turi_int(a1), turi_int(b1) };
    TuriValue r1 = turi_call(env, a[2], c1args, 2);
    if (turi_is_error(r1) || env->throwing) return r1;  /* propagate callback error */
    bool e1 = (r1.tag == TURI_BOOL) ? r1.as_bool : (r1.as_int != 0);
    if (!e1) return turi_bool(false);
    TuriValue c2args[2] = { turi_int(a2), turi_int(b2) };
    TuriValue r2 = turi_call(env, a[3], c2args, 2);
    if (turi_is_error(r2) || env->throwing) return r2;  /* propagate callback error */
    bool e2 = (r2.tag == TURI_BOOL) ? r2.as_bool : (r2.as_int != 0);
    return turi_bool(e2);
}

static void wk_register_backtrack_natives(TuriEnv *env) {
    turi_env_register_native(env, "tuple2-eq-carrier?", native_tuple2_eq_carrier, NULL);
    turi_env_register_native(env, "bt-nil",        native_bt_nil,       NULL);
    turi_env_register_native(env, "bt-cons",       native_bt_cons,      NULL);
    turi_env_register_native(env, "mzero",         native_bt_nil,       NULL);
    turi_env_register_native(env, "mreturn",       native_bt_mreturn,   NULL);
    turi_env_register_native(env, "mplus",         native_bt_mplus,     NULL);
    turi_env_register_native(env, "mbind",         native_bt_mbind,     NULL);
    turi_env_register_native(env, "bt-length",     native_bt_length,    NULL);
    turi_env_register_native(env, "bt-print",      native_bt_print,     NULL);
    turi_env_register_native(env, "bt-apply-fat",  native_bt_apply_fat, NULL);
}

static void wk_register_proc_fs_natives(TuriEnv *env) {
    turi_env_register_native(env, "process/spawn",    native_process_spawn,     NULL);
    turi_env_register_native(env, "process/wait",     native_process_wait,      NULL);
    turi_env_register_native(env, "fs/tmpfile",       native_fs_tmpfile,        NULL);
    turi_env_register_native(env, "fs/tmpfile-path",  native_fs_tmpfile_path,   NULL);
    turi_env_register_native(env, "fs/tmpfile-fd",    native_fs_tmpfile_fd,     NULL);
    turi_env_register_native(env, "fs/tmpfile-free",  native_fs_tmpfile_free,   NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): serial.tur Serializable instances.
 *
 * serialize packs a length-prefixed little-endian byte buffer (int64* header
 * word 0 = data length, data starts at word 1 -- the same layout as Bytes);
 * deserialize reads it back.  Registered under the elaborator instance-binding
 * names so (.serialize x) dispatch and the direct __inst_* calls both resolve.
 * ---------------------------------------------------------------------- */
static TuriValue native_serialize_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = (n > 0) ? a[0].as_int : 0;
    int64_t *buf = (int64_t *)malloc(sizeof(int64_t) + 8);
    if (!buf) return turi_nil();
    buf[0] = 8;
    uint8_t *p = (uint8_t *)(buf + 1);
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    return wk_int_ptr(buf);
}
static TuriValue native_deserialize_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    int64_t *b = (int64_t *)(intptr_t)a[0].as_int;
    if (b[0] < 8) return turi_int(0);
    uint8_t *p = (uint8_t *)(b + 1);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
    return turi_int((int64_t)v);
}
static TuriValue native_serialize_bool(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t v = 0;
    if (n > 0) v = (a[0].tag == TURI_BOOL) ? (a[0].as_bool ? 1 : 0) : (a[0].as_int != 0);
    int64_t *buf = (int64_t *)malloc(sizeof(int64_t) + 1);
    if (!buf) return turi_nil();
    buf[0] = 1;
    *(uint8_t *)(buf + 1) = (uint8_t)v;
    return wk_int_ptr(buf);
}
static TuriValue native_deserialize_bool(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    int64_t *b = (int64_t *)(intptr_t)a[0].as_int;
    if (b[0] < 1) return turi_int(0);
    return turi_int(*(uint8_t *)(b + 1) ? 1 : 0);
}

static void wk_register_serial_natives(TuriEnv *env) {
    turi_env_register_native(env, "__inst_Serializable_serialize_int",     native_serialize_int,     NULL);
    turi_env_register_native(env, "__inst_Serializable_deserialize_int",   native_deserialize_int,   NULL);
    turi_env_register_native(env, "__inst_Serializable_serialize_bool",    native_serialize_bool,    NULL);
    turi_env_register_native(env, "__inst_Serializable_deserialize_bool",  native_deserialize_bool,  NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): serial.tur Bytes native shims.
 *
 * Bytes is a malloc'd int64_t* whose word 0 holds the length and whose data
 * starts at word 1 (the compiled ABI).  bytes-alloc/len/data/free re-implement
 * that layout exactly.
 * ---------------------------------------------------------------------- */
static TuriValue native_bytes_alloc(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t len = (n > 0) ? a[0].as_int : 0;
    if (len < 0) len = 0;
    int64_t *buf = (int64_t *)malloc(sizeof(int64_t) + (size_t)len);
    if (!buf) return turi_nil();
    buf[0] = len;
    memset(buf + 1, 0, (size_t)len);
    return wk_int_ptr(buf);
}
static TuriValue native_bytes_len(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_int(0);
    return turi_int(((int64_t *)(intptr_t)a[0].as_int)[0]);
}
static TuriValue native_bytes_data(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    return wk_int_ptr((int64_t *)(intptr_t)a[0].as_int + 1);
}
static TuriValue native_bytes_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n > 0 && a[0].as_int) free((void *)(intptr_t)a[0].as_int);
    return turi_nil();
}

static void wk_register_bytes_natives(TuriEnv *env) {
    turi_env_register_native(env, "bytes-alloc", native_bytes_alloc, NULL);
    turi_env_register_native(env, "bytes-len",   native_bytes_len,   NULL);
    turi_env_register_native(env, "bytes-data",  native_bytes_data,  NULL);
    turi_env_register_native(env, "bytes-free",  native_bytes_free,  NULL);
}

/* -------------------------------------------------------------------------
 * R1 (turi-interpret-flip-residual-plan): taskgroup.tur TaskGroupBlock shims.
 *
 * Replica of taskgroup.tur's TaskGroupBlock.  We include cancel_reason in the
 * allocation (the canonical documented layout) so cancel-with-reason is safe --
 * note the compiled task-group-new omits it, a latent OOB write tracked in
 * docs/reported/taskgroup-block-cancel-reason-layout-overflow.md.
 *
 * The cancel native does NOT touch the per-fiber thread-local cancelled flag
 * (tur_fiber_set_cancelled) that the inline-C body sets: under --interpret no
 * fibers are running, and the fixtures observe only the group's own flag.
 * ---------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    int64_t task_count;
    int64_t completed_count;
    bool cancelled;
    bool done;
    int64_t cancel_reason;
} WkTaskGroup;

static TuriValue native_task_group_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)a; (void)n; (void)ud;
    WkTaskGroup *g = (WkTaskGroup *)malloc(sizeof(WkTaskGroup));
    if (!g) return turi_nil();
    pthread_mutex_init(&g->lock, NULL);
    pthread_cond_init(&g->done_cond, NULL);
    g->task_count = 0; g->completed_count = 0;
    g->cancelled = false; g->done = false; g->cancel_reason = 0;
    return wk_int_ptr(g);
}
static TuriValue native_task_group_cancel(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&g->lock);
    g->cancelled = true; g->done = true; g->cancel_reason = 0;
    pthread_cond_broadcast(&g->done_cond);
    pthread_mutex_unlock(&g->lock);
    return turi_nil();
}
static TuriValue native_task_group_wait(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&g->lock);
    while (!g->done) pthread_cond_wait(&g->done_cond, &g->lock);
    pthread_mutex_unlock(&g->lock);
    return turi_nil();
}
static TuriValue native_task_group_cancelled(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_bool(false);
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_lock(&g->lock);
    bool c = g->cancelled;
    pthread_mutex_unlock(&g->lock);
    return turi_bool(c);
}
static TuriValue native_task_group_free(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n < 1 || !a[0].as_int) return turi_nil();
    WkTaskGroup *g = (WkTaskGroup *)(intptr_t)a[0].as_int;
    pthread_mutex_destroy(&g->lock);
    pthread_cond_destroy(&g->done_cond);
    free(g);
    return turi_nil();
}

static void wk_register_taskgroup_natives(TuriEnv *env) {
    turi_env_register_native(env, "task-group-new",        native_task_group_new,       NULL);
    turi_env_register_native(env, "task-group-cancel",     native_task_group_cancel,    NULL);
    turi_env_register_native(env, "task-group-wait",       native_task_group_wait,      NULL);
    turi_env_register_native(env, "task-group-cancelled?", native_task_group_cancelled, NULL);
    turi_env_register_native(env, "task-group-free",       native_task_group_free,      NULL);
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
    /* NOTE: do NOT register the carrier-fallback `__inst_Show_show_T` here.
     * The `_T` suffix is the ABSTRACT/carrier mangling, not float-specific
     * (TY_FLOAT now mangles to the concrete `_float` suffix above).  Any
     * user-defined `Show` instance over a carrier-typed receiver -- an
     * opaque handle, an `rc<T>`, etc. -- emits its method as
     * `__inst_Show_show_T`, so pre-binding that name to native_show_float
     * silently HIJACKS the user's instance and returns "0" for every
     * non-float receiver (turi-carrier-fallback-instance-method-silent-
     * miscompile / exg5-rc-in-exists).  Leaving it unbound lets the user's
     * own instance method resolve. */
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
    if (turi_is_error(result) || env->throwing) return result;  /* propagate callback error */
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
        /* Register native HAMT implementations so persistent map operations
         * work correctly in interpreter mode. */
        wk_register_hamt_natives(env);
        wk_register_map_natives(env);
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
    "build", "run", "repl", "worker", "interpret", "debug",
    "eval", "doc", "image-info", "image-verify", "explain",
    "format", "fmt", "parse-check", "test",
    "new", "init", "add", "add-cmake", "fetch",
    "install", "uninstall", "list", "upgrade", "experiments",
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
        "  --no-color                       disable colored diagnostics\n"
        "  --json                           structured JSON output (tur doc, tur test, tur check)\n"
        "  --json-diagnostics               output diagnostics as JSON (phase 8)\n"
        "  --explain <TUR-E####>            print explanation for a diagnostic code (HKT-P5)\n"
        "  --explain <snippet>              compile code snippet and explain errors (phase 8)\n"
        "  --dump-kinds                     dump kind annotations after kind-check (HKT-P6)\n"
        "  --strict-effects                 warn on unannotated effectful functions (ER1)\n"
        "  --dump-effects                   print inferred effect row for each defn (ER6)\n"
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
        case XF_SRC_CLI:      return "cli";
        case XF_SRC_MANIFEST: return "manifest";
        default:              return "-";
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
    /* M7: by-value HKT dispatch is now ON by default (the flag-on suite is
     * green; stdlib migration "flip default first").  `TUR_M7_HKT=0` opts back
     * out to the legacy carrier path; `=1` (or unset) keeps the default ON. */
    {
        const char *m7 = getenv("TUR_M7_HKT");
        if (m7 && m7[0] == '0' && m7[1] == '\0') g_m7_hkt_enabled = false;
        else if (m7 && m7[0] == '1' && m7[1] == '\0') g_m7_hkt_enabled = true;
    }
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
            /* XF1: suppress TUR-W006x.  Intended for the Turmeric project's
             * own CI matrix; spice users should not set this. */
            g_allow_experimental = true;
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
            rc = cmd_build(input, out, (const char **)build_inc, n_build_inc,
                           build_target, (const char **)b_rm, b_n);
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
                strncmp(argv[i], "-I", 2) == 0) {
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
     * script can drive it).  See docs/upcoming/debugger-plan.md (Phase 2). */
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

    /* GS-M2: subcommand fallthrough — `tur foo bar` execs `tur-foo bar`
     * from $PATH when "foo" isn't a built-in. Built-ins always win.
     * If exec succeeds, this does not return. */
    return try_external_subcommand(argc, argv);
}
