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
#include "symbols.h"
/* Phase S0: eval API for tur repl */
#include "turi/eval.h"
/* Phase S1: REPL with libedit, multi-line input, :type/:doc/:reload */
#include "turi/repl.h"
/* Phase PKG-1: Spice package manager */
#include "pkg.h"
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
 * When true: tur doc prints JSON; tur test prints JSON; tur check uses JSON diag. */
static bool use_json_output = false;

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
                                          NULL);
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
                        const char **include_dirs, int n_include_dirs);

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
    int rc = compile_to_c(path, &discard, NULL, 0);
    buf_free(&discard);
    g_collect_syms_out   = NULL;
    g_collect_syms_cap   = 0;
    g_collect_syms_count = NULL;
    return rc;
}

/* Reads a .tur file and emits its C source into `out_c`. Returns 0 on success,
 * nonzero on error (diagnostics already emitted).
 * include_dirs/n_include_dirs: additional module search paths for (import ...). */
static int compile_to_c(const char *path, Buf *out_c,
                         const char **include_dirs, int n_include_dirs) {
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

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

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
        /* Phase P3: HAMT lowering - auto-load hamt.tur and map.tur */
        "hamt.tur",
        "map.tur",
        /* "gen.tur" - GF2 generator stdlib; not auto-loaded to avoid polluting
         * all programs.  Load explicitly with (load "stdlib/gen.tur"). */
        /* "vec.tur" - has typeclass dependencies, not auto-loaded */
        /* Phase PTC4: typeclass-eq.tur defines only the Eq class skeleton so that
         * typed-collection definstances (Eq[Vec], Eq[Map], etc.) have Eq in scope.
         * The full typeclass.tur (with all primitive instances) remains on-demand. */
        "typeclass-eq.tur",
        /* Bug-5 follow-up: result.tur is auto-loaded so its `ok` / `ok?` /
         * `ok-val` / `err` / `err?` / `err-val` helpers are globally
         * available without an explicit `(import result ...)`.  Mirrors how
         * tresult.tur exposes its typed counterpart. */
        "result.tur",
        /* Phase TM0/TC1/TC2: typed parameterized collection stdlib files. */
        "tmap.tur",
        "tvec.tur",
        "tslice.tur",
        "toption.tur",
        "tresult.tur",
        "tpair.tur",
        "tlist.tur",
        "tgrid.tur",
        "tzipper.tur",
        "tset.tur",
        /* Phase F5 (cross-plan-followups): mutable open-addressed hash table. */
        "tmutmap.tur",
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

    for (int i = 0; stdlib_files[i] != NULL; i++) {
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

/* Compile a .tur file to a C header (.h). Returns 0 on success. */
static int compile_to_h(const char *path, Buf *out_h, const char *module_name) {
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

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

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
        ctx.separate_compilation = true;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_header(out_h, module_name, ctx.prog, true) != 0) {
            rc = 1;
        }
    }

    symtab_free(&st);
    arena_free(&arena);
    free(src);
    return rc;
}

/* Compile a .tur file to a C implementation (.c). Returns 0 on success. */
static int compile_to_implementation(const char *path, Buf *out_c, const char *module_name) {
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

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

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
        ctx.separate_compilation = true;
        rc = run_core_passes(&ctx);
        if (rc == 0 && emit_implementation(out_c, module_name, ctx.prog, true) != 0) {
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
 * Used by the LSP server. Must be called with diag_lsp_begin active. */
int tur_check_only(const char *path) {
    Buf discard;
    buf_init(&discard);
    int rc = compile_to_c(path, &discard, NULL, 0);
    buf_free(&discard);
    return rc;
}

static int cmd_emit_c(const char *path) {
    Buf out;
    buf_init(&out);
    int rc = compile_to_c(path, &out, NULL, 0);
    if (rc == 0) buf_to_file(&out, stdout);
    buf_free(&out);
    return rc;
}

/* Phase B: emit per-module .h and .c files to a directory.
 * Usage: tur emit-c --output-dir <dir> <file1.tur> [<file2.tur> ...]
 * Each input produces <dir>/<module>.h and <dir>/<module>.c. */
static int cmd_emit_c_to_dir(const char *out_dir, char **inputs, int n_inputs) {
    if (n_inputs < 1) {
        fprintf(stderr, "tur emit-c --output-dir: at least one input required\n");
        return 1;
    }

    /* Create output directory if it doesn't exist */
    struct stat st;
    if (stat(out_dir, &st) != 0) {
        if (mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "tur emit-c: cannot create '%s': %s\n",
                    out_dir, strerror(errno));
            return 2;
        }
    }

    int rc = 0;
    for (int i = 0; i < n_inputs && rc == 0; i++) {
        const char *input = inputs[i];
        const char *base = basename_of(input);
        size_t base_len = strlen(base);
        char mod_name[256];
        size_t n = (base_len >= 4 && strcmp(base + base_len - 4, ".tur") == 0)
                   ? base_len - 4 : base_len;
        if (n >= sizeof(mod_name)) n = sizeof(mod_name) - 1;
        memcpy(mod_name, base, n);
        mod_name[n] = '\0';

        char h_path[1024], c_path[1024];
        snprintf(h_path, sizeof(h_path), "%s/%s.h", out_dir, mod_name);
        snprintf(c_path, sizeof(c_path), "%s/%s.c", out_dir, mod_name);

        Buf h_buf, c_buf;
        buf_init(&h_buf);
        buf_init(&c_buf);

        int h_rc = compile_to_h(input, &h_buf, mod_name);
        int c_rc = compile_to_implementation(input, &c_buf, mod_name);

        if (h_rc != 0 || c_rc != 0) {
            rc = (h_rc != 0) ? h_rc : c_rc;
        } else if (buf_to_path(&h_buf, h_path) != 0 ||
                   buf_to_path(&c_buf, c_path) != 0) {
            fprintf(stderr, "tur emit-c: failed to write output for '%s'\n",
                    input);
            rc = 2;
        }
        buf_free(&h_buf);
        buf_free(&c_buf);
    }
    return rc;
}

/* Phase M3: emit the .h for a single module file to stdout. */
static int cmd_emit_h(const char *path) {
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
    int rc = compile_to_h(path, &out, mod_name);
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
                     const char *target);
static char *find_project_root(const char *start);

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
                     const char *target) {
    Buf csrc;
    buf_init(&csrc);
    int rc = compile_to_c(input, &csrc, include_dirs, n_include_dirs);
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

static int decode_exit_status(int status) {
    if (status == -1) return 127;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static int cmd_run(int argc, char **argv) {
    /* tur run [--release] [--offline] [<file>] [-- <args>...] */
    bool        release           = false;
    bool        offline           = false;
    const char *explicit_file     = NULL;
    int         passthrough_start = -1;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--release") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--offline") == 0) {
            offline = true;
        } else if (strcmp(argv[i], "--") == 0) {
            passthrough_start = i + 1;
            break;
        } else if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
            if (!explicit_file) explicit_file = argv[i];
        }
    }
    (void)release; /* passed to compiler when --release build is supported */

    /* spice_inc_dirs: populated below during project-mode setup.
     * RUN_ENTRY captures these via the enclosing scope. */
    const char **spice_inc_dirs = NULL;
    int          n_spice_inc_dirs = 0;

    /* Helper: build 'entry', exec with optional passthrough args. */
#define RUN_ENTRY(entry_path) do {                                       \
        char out_path[] = "/tmp/tur-run-XXXXXX";                         \
        int _fd = mkstemp(out_path);                                     \
        if (_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); return 2; } \
        close(_fd);                                                      \
        int _rc = cmd_build((entry_path), out_path,                      \
                            spice_inc_dirs, n_spice_inc_dirs, NULL);     \
        if (_rc != 0) { unlink(out_path); free(spice_inc_dirs); return _rc; } \
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
        return decode_exit_status(_sys);                                 \
    } while (0)

    /* Single-file mode: explicit file provided, skip project lookup. */
    if (explicit_file) {
        /* E6: treat "-" as stdin -- buffer it into a temp .tur file first. */
        if (strcmp(explicit_file, "-") == 0) {
            char src_tmp[] = "/tmp/tur-stdin-XXXXXX.tur";
            int src_fd = mkstemps(src_tmp, 4);
            if (src_fd < 0) { fprintf(stderr, "tur: mkstemp failed\n"); return 2; }
            char ibuf[4096]; size_t nr;
            int ok = 1;
            while ((nr = fread(ibuf, 1, sizeof(ibuf), stdin)) > 0)
                if ((size_t)write(src_fd, ibuf, nr) != nr) { ok = 0; break; }
            close(src_fd);
            if (!ok) {
                unlink(src_tmp);
                fprintf(stderr, "tur: error reading stdin\n");
                return 2;
            }
            char out_path[] = "/tmp/tur-run-XXXXXX";
            int out_fd = mkstemp(out_path);
            if (out_fd < 0) { unlink(src_tmp); fprintf(stderr, "tur: mkstemp failed\n"); return 2; }
            close(out_fd);
            int brc = cmd_build(src_tmp, out_path, spice_inc_dirs, n_spice_inc_dirs, NULL);
            unlink(src_tmp);
            if (brc != 0) { unlink(out_path); free(spice_inc_dirs); return brc; }
            Buf run_cmd; buf_init(&run_cmd);
            buf_printf(&run_cmd, "'%s'", out_path);
            if (passthrough_start >= 0)
                for (int i = passthrough_start; i < argc; i++)
                    buf_printf(&run_cmd, " '%s'", argv[i]);
            buf_putc(&run_cmd, '\0');
            int sys = system(run_cmd.data);
            buf_free(&run_cmd);
            unlink(out_path);
            free(spice_inc_dirs);
            return decode_exit_status(sys);
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

        /* Build spice include-path array.
         * Convention: spices/<name>-<ref>/src/ if it exists, else spices/<name>-<ref>/ */
        int inc_cap = m.n_spices;
        spice_inc_dirs = (const char **)malloc((size_t)inc_cap * sizeof(char *));
        n_spice_inc_dirs = 0;
        if (spice_inc_dirs) {
            for (int i = 0; i < m.n_spices; i++) {
                const PkgSpice *s = &m.spices[i];
                char dep_dir[4096];
                if (s->path) {
                    /* Local path dep: resolve relative to root */
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s", root, s->path);
                } else if (s->ref) {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                             spices_dir, s->name, s->ref);
                } else {
                    snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                             spices_dir, s->name);
                }
                /* For monorepo sub-packages, descend into the subdir */
                if (s->subdir) {
                    char tmp[4096];
                    snprintf(tmp, sizeof(tmp), "%s/%s", dep_dir, s->subdir);
                    strncpy(dep_dir, tmp, sizeof(dep_dir) - 1);
                    dep_dir[sizeof(dep_dir) - 1] = '\0';
                }
                /* Prefer dep_dir/src if it exists */
                char src_sub[4096];
                snprintf(src_sub, sizeof(src_sub), "%s/src", dep_dir);
                struct stat _ss;
                const char *chosen = (stat(src_sub, &_ss) == 0 && S_ISDIR(_ss.st_mode))
                                     ? src_sub : dep_dir;
                spice_inc_dirs[n_spice_inc_dirs++] = strdup(chosen);
            }
        }
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
    {
        /* find_project_root only walks up from the given path, so it never
         * finds a build.tur sitting in the current working directory when
         * `dir` is a relative subpath like "tests/plutovg".  Resolve to an
         * absolute path first. */
        char abs_dir[4096];
        if (!realpath(dir, abs_dir)) {
            strncpy(abs_dir, dir, sizeof(abs_dir) - 1);
            abs_dir[sizeof(abs_dir) - 1] = '\0';
        }
        char *proj_root = find_project_root(abs_dir);
        if (proj_root) {
            char manifest_path[4096];
            snprintf(manifest_path, sizeof(manifest_path),
                     "%s/build.tur", proj_root);
            PkgManifest m;
            if (pkg_manifest_read(manifest_path, &m)) {
                /* Capacity: own src + one entry per :spice. */
                int cap = 1 + m.n_spices;
                spice_inc_dirs = (const char **)malloc((size_t)cap * sizeof(char *));
                if (spice_inc_dirs) {
                    /* Project's own src/, if it exists. */
                    {
                        char own_src[4096];
                        snprintf(own_src, sizeof(own_src), "%s/src", proj_root);
                        struct stat ss;
                        if (stat(own_src, &ss) == 0 && S_ISDIR(ss.st_mode))
                            spice_inc_dirs[n_spice_inc_dirs++] = strdup(own_src);
                    }
                    /* Each declared :spice's resolved src/.  Mirrors the
                     * convention used by `cmd_run`: spices/<name>-<ref>/
                     * (optionally /<subdir>) /src, falling back to the
                     * dep dir if `src/` is absent. */
                    char spices_dir[4096];
                    snprintf(spices_dir, sizeof(spices_dir),
                             "%s/spices", proj_root);
                    for (int i = 0; i < m.n_spices; i++) {
                        const PkgSpice *s = &m.spices[i];
                        char dep_dir[4096];
                        if (s->path) {
                            snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                                     proj_root, s->path);
                        } else if (s->ref) {
                            snprintf(dep_dir, sizeof(dep_dir), "%s/%s-%s",
                                     spices_dir, s->name, s->ref);
                        } else {
                            snprintf(dep_dir, sizeof(dep_dir), "%s/%s",
                                     spices_dir, s->name);
                        }
                        if (s->subdir) {
                            char tmp[4096];
                            snprintf(tmp, sizeof(tmp), "%s/%s",
                                     dep_dir, s->subdir);
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

                        /* Monorepo fallback: when the fetched copy is
                         * absent (e.g. optional spice never cloned) but a
                         * workspace ancestor contains the `:subdir` as a
                         * sibling member, use that instead.  We walk up to
                         * three directory levels to handle the
                         * `turmeric-spices/spices/<this>` -> `:subdir
                         * "spices/test"` -> `turmeric-spices/spices/test`
                         * shape used by the first-party spices repo. */
                        bool fallback_added = false;
                        if (!chosen && s->subdir) {
                            char ancestor[4096];
                            strncpy(ancestor, proj_root, sizeof(ancestor) - 1);
                            ancestor[sizeof(ancestor) - 1] = '\0';
                            for (int up = 0; up < 4 && !fallback_added; up++) {
                                char *slash = strrchr(ancestor, '/');
                                if (!slash || slash == ancestor) break;
                                *slash = '\0';
                                char sib_src[4096];
                                snprintf(sib_src, sizeof(sib_src),
                                         "%s/%s/src", ancestor, s->subdir);
                                if (stat(sib_src, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                                    spice_inc_dirs[n_spice_inc_dirs++] = strdup(sib_src);
                                    fallback_added = true;
                                    break;
                                }
                                char sib_dir[4096];
                                snprintf(sib_dir, sizeof(sib_dir),
                                         "%s/%s", ancestor, s->subdir);
                                if (stat(sib_dir, &ss) == 0 && S_ISDIR(ss.st_mode)) {
                                    spice_inc_dirs[n_spice_inc_dirs++] = strdup(sib_dir);
                                    fallback_added = true;
                                    break;
                                }
                            }
                        }
                        if (fallback_added) continue;

                        if (chosen)
                            spice_inc_dirs[n_spice_inc_dirs++] = strdup(chosen);
                    }
                }
                pkg_manifest_free(&m);
            }
            free(proj_root);
        }
    }

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

        int build_rc = cmd_build(tur_files[i], out_path,
                                  spice_inc_dirs, n_spice_inc_dirs, NULL);
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
static int cmd_build_multi(const char *dir, const char *out_path) {
    int n_files;
    char **tur_files = collect_tur_files(dir, &n_files);
    if (!tur_files || n_files == 0) {
        fprintf(stderr, "tur: no .tur files found in '%s'\n", dir);
        free_tur_files(tur_files, n_files);
        return 1;
    }

    char chosen_out[1024];
    if (!out_path) {
        default_output_name(dir, chosen_out, sizeof(chosen_out));
        out_path = chosen_out;
    }

    /* Allocate arrays for .h and .c filenames */
    char **h_files = (char **)malloc(n_files * sizeof(char *));
    char **c_files = (char **)malloc(n_files * sizeof(char *));
    if (!h_files || !c_files) { fprintf(stderr, "tur: oom\n"); return 2; }

    /* Generate module names (basename without .tur) */
    for (int i = 0; i < n_files; i++) {
        const char *base = basename_of(tur_files[i]);
        size_t len = strlen(base);
        h_files[i] = (char *)malloc(len + 4);
        c_files[i] = (char *)malloc(len + 4);
        snprintf(h_files[i], len + 4, "%.*s.h", (int)(len - 4), base);
        snprintf(c_files[i], len + 4, "%.*s.c", (int)(len - 4), base);
    }

    /* Compile each .tur file to .h and .c */
    for (int i = 0; i < n_files; i++) {
        const char *module_name = basename_of(tur_files[i]);
        size_t len = strlen(module_name);
        char mod_name_no_ext[256];
        snprintf(mod_name_no_ext, sizeof(mod_name_no_ext), "%.*s", (int)(len - 4), module_name);

        Buf h_out;
        buf_init(&h_out);
        if (compile_to_h(tur_files[i], &h_out, mod_name_no_ext) != 0) {
            fprintf(stderr, "tur: failed to compile %s to header\n", tur_files[i]);
            buf_free(&h_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&h_out, h_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", h_files[i]);
            buf_free(&h_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&h_out);

        Buf c_out;
        buf_init(&c_out);
        if (compile_to_implementation(tur_files[i], &c_out, mod_name_no_ext) != 0) {
            fprintf(stderr, "tur: failed to compile %s to implementation\n", tur_files[i]);
            buf_free(&c_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        if (buf_to_path(&c_out, c_files[i]) != 0) {
            fprintf(stderr, "tur: failed to write %s\n", c_files[i]);
            buf_free(&c_out);
            for (int j = 0; j < i; j++) { free(h_files[j]); free(c_files[j]); }
            free(h_files); free(c_files);
            free_tur_files(tur_files, n_files);
            return 1;
        }
        buf_free(&c_out);
    }

    /* Generate _main.c */
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
    buf_printf(&cmd, "%s %s -o %s", cc, cc_flags, out_path);
    /* Add _main.c first */
    buf_printf(&cmd, " _main.c");
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
    for (int i = 0; i < n_files; i++) { free(h_files[i]); free(c_files[i]); }
    free(h_files); free(c_files);
    free_tur_files(tur_files, n_files);
    unlink("_main.c");

    if (sys_rc != 0) {
        fprintf(stderr, "tur: cc invocation failed (status %d)\n", sys_rc);
        return 2;
    }
    return 0;
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

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

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

static int cmd_repl(void) {
    return turi_repl_run();
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
                "map.tur",
                /* Bug-5 follow-up: result.tur preloaded so ok/ok?/ok-val are
                 * globally available in the worker eval path too. */
                "result.tur",
                /* Phase TM0/TC1/TC2: typed parameterized collection stdlib files. */
                "tmap.tur",
                "tvec.tur",
                "tslice.tur",
                "toption.tur",
                "tresult.tur",
                "tpair.tur",
                "tlist.tur",
                "tgrid.tur",
                "tzipper.tur",
                "tset.tur",
                "tmutmap.tur",
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
        int rc = compile_to_c(input, &cbuf, NULL, 0);
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
        "\n"
        "package management (Spice, Phase PKG-1):\n"
        "  tur init [--bin|--lib] <name>     create a new project\n"
        "  tur add <url> [--ref <tag>]       add a Turmeric spice\n"
        "  tur add <path> --path             add a local spice\n"
        "  tur add-cmake <url> [--ref <tag>] add a C/CMake dependency\n"
        "  tur fetch [--update]              download / update all spices\n"
        "  tur emit-cmake [--output-dir <d>] generate CMakeLists.txt + config for CMake consumers\n"
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
    return 64;
}

/* E1: per-subcommand help strings */
static int usage_build(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur build <file.tur> [-o <out>]   build a single file\n"
        "  tur build <dir> [-o <out>]        build all .tur files in directory\n"
        "\n"
        "flags:\n"
        "  -o <out>          output file path\n"
        "  -I <dir>          add include directory\n"
        "  --target wasm     compile to WebAssembly via emcc (requires Emscripten)\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
}

static int usage_run(void) {
    fprintf(stderr,
        "usage:\n"
        "  tur run <file.tur> [-- <args...>]   build and execute a single file\n"
        "  tur run - [-- <args...>]            read source from stdin, build and execute\n"
        "\n"
        "flags:\n"
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
        "  tur check <file.tur>   type-check only, no codegen\n"
        "\n"
        "Try 'tur --help' for global options.\n");
    return 0;
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
        "  tur repl   start the interactive REPL\n"
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
                                    NULL, 0, NULL);
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
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--panic-abort") == 0) {
            /* Already parsed, remove from argv */
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
        /* tur emit-c --output-dir <dir> <file1> [<file2> ...] */
        if (argc >= 4 && strcmp(argv[2], "--output-dir") == 0) {
            const char *out_dir = argv[3];
            return cmd_emit_c_to_dir(out_dir, argv + 4, argc - 4);
        }
        /* tur emit-c <file> -- single file to stdout (legacy) */
        if (argc != 3) return usage_build();
        return cmd_emit_c(argv[2]);
    }
    if (strcmp(cmd, "emit-h") == 0) {
        if (argc != 3) return usage_build();
        return cmd_emit_h(argv[2]);
    }
    if (strcmp(cmd, "check") == 0) {
        /* Phase 8: tur check subcommand - type-check only, no codegen */
        if (argc == 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_check();
        if (argc != 3) return usage_check();
        if (use_json_output) {
            diag_lsp_begin();
            Buf out;
            buf_init(&out);
            compile_to_c(argv[2], &out, NULL, 0);
            buf_free(&out);
            diag_lsp_flush(stdout);
            diag_lsp_end();
            return diag_had_error() ? 1 : 0;
        }
        Buf out;
        buf_init(&out);
        int rc = compile_to_c(argv[2], &out, NULL, 0);
        buf_free(&out);
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
        /* Collect -I flags from the command line */
        int     n_build_inc = 0;
        int     build_inc_cap = 4;
        char  **build_inc = (char **)malloc((size_t)build_inc_cap * sizeof(char *));
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                free(build_inc); return usage_build();
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out = argv[++i];
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                build_target = argv[++i];
                if (strcmp(build_target, "wasm") != 0) {
                    fprintf(stderr, "tur build: unknown target '%s' (supported: wasm)\n", build_target);
                    free(build_inc); return 1;
                }
            } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
                if (n_build_inc >= build_inc_cap) {
                    build_inc_cap *= 2;
                    build_inc = (char **)realloc(build_inc,
                        (size_t)build_inc_cap * sizeof(char *));
                }
                build_inc[n_build_inc++] = argv[++i];
            } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
                if (n_build_inc >= build_inc_cap) {
                    build_inc_cap *= 2;
                    build_inc = (char **)realloc(build_inc,
                        (size_t)build_inc_cap * sizeof(char *));
                }
                build_inc[n_build_inc++] = argv[i] + 2;
            } else if (argv[i][0] != '-') {
                if (input) { free(build_inc); return usage_build(); }
                input = argv[i];
            } else {
                free(build_inc); return usage_build();
            }
        }
        if (!input) { free(build_inc); return usage_build(); }
        /* Check if input is a directory - use multi-file build */
        int rc;
        if (is_directory(input)) {
            rc = cmd_build_multi(input, out);
        } else {
            rc = cmd_build(input, out, (const char **)build_inc, n_build_inc, build_target);
        }
        free(build_inc);
        return rc;
    }
    if (strcmp(cmd, "run") == 0) {
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--") == 0) break;
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
                return usage_run();
        }
        return cmd_run(argc, argv);
    }
    if (strcmp(cmd, "repl") == 0) {
        /* Phase S0: interactive REPL */
        if (argc >= 3 && (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-h") == 0))
            return usage_repl();
        return cmd_repl();
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
    return usage();
}
