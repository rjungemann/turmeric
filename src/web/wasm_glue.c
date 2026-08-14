/* wasm_glue.c - WASM-friendly API wrapper for libturi
 *
 * This file provides a C API that is designed to be easily callable from
 * WebAssembly via Emscripten. It wraps the existing libturi eval API with
 * functions that:
 *   - Have simple, flat signatures (no struct pointers in args)
 *   - Allocate and free memory in a WASM-compatible way
 *   - Return results as strings for easy JS interop
 */

#include "wasm_glue.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* We need to include the turi headers. src/ is in the include path. */
#include "turi/env.h"
#include "turi/eval.h"
#include "turi/value.h"
#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "fmt.h"
#include "reader.h"
#include "symbols.h"
#include "turi/preload.h"
#include "turi/interpreter_natives.h"
#include "turi/repl.h"
#include "elab.h"
#include "expr.h"
#include "forms.h"
#include "types.h"

/* ---------------------------------------------------------------------------
 * Global state for the WASM module
 * ---------------------------------------------------------------------------
 */

/* The single evaluation environment for the WASM module.
 * This is initialized once and reused across all eval calls. */
static TuriEnv *g_env = NULL;

static void wasm_diag_sink(struct TuriEnv *env, int level, const char *code,
                           const char *file, uint32_t line,
                           uint32_t col_start, uint32_t col_end,
                           const char *message, void *ud);

static void type_of_diag_sink(struct TuriEnv *env, int level, const char *code,
                              const char *file, uint32_t line,
                              uint32_t col_start, uint32_t col_end,
                              const char *message, void *ud);

/* ---------------------------------------------------------------------------
 * Memory management helpers
 * ---------------------------------------------------------------------------
 */

/* Allocate memory that can be freed by JavaScript via _free().
 * Emscripten's EM_MALLOC_POOL is used by malloc in WASM builds. */
char *turi_wasm_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/* Free memory allocated by turi_wasm_strdup. */
void turi_wasm_free(void *p) {
    free(p);
}

/* Root of the stdlib inside the WASM MEMFS.  The Emscripten link step bundles
 * the host `stdlib/` tree here via `--embed-file <src>/stdlib@/stdlib`, and the
 * module's default working directory is "/", so a cwd-relative `stdlib/<file>`
 * load (the form the stdlib modules use internally for their transitive loads)
 * and this top-level root resolve to the same `/stdlib/<file>` -- keeping the
 * (load ...) dedup keys consistent. */
#define WASM_STDLIB_ROOT "stdlib"

/* Bring a freshly-created env up to interpreter parity with the native
 * `tur --interpret` entry point: load the core macros (when/cond/for/and/or +
 * assert!/require!/...) and the typed-collection stdlib so reader-macro data
 * literals (`#map{...}`, `#set{...}`, `[...]`) and the rest of the stdlib
 * resolve instead of failing "unknown function or operator 'hamt-of'".  The
 * underlying Vec/Set/Map/HAMT runtime ops are already registered as natives by
 * turi_env_new (turi_register_collection_natives).  Shares the load list with
 * src/main.c via src/turi/preload.c so the two entry points cannot drift.
 * See docs/archive/history/web-repl-missing-stdlib-preload.md. */
static void wasm_preload_stdlib(TuriEnv *env) {
    if (!env) return;
    turi_env_preload_macros(env, WASM_STDLIB_ROOT);
    /* Typed native-function stubs (nil-value/cons/head/tail + benchmark helpers)
     * in the same after-macros/before-collections slot as --interpret and the
     * native REPL, so `(list-head (cons 65 (cons 66 0)))` evaluates to 65 at the
     * browser prompt instead of nil (cons would otherwise be an elaborator
     * builtin the tree-walker cannot execute). */
    turi_env_preload_native_stubs(env);
    turi_env_preload_collections(env, WASM_STDLIB_ROOT);
    /* Preload the REPL-only Show slice (Show [Vec] / [Set] / [Map]) so a
     * collection result renders through its Show instance via
     * turi_try_show_by_tag below, matching the native `tur repl`
     * (src/turi/repl.c, right after turi_env_preload_collections). */
    turi_env_preload_typeclasses(env, WASM_STDLIB_ROOT);
    /* Pin the preloaded prefix: the browser REPL is the long-lived env this
     * matters most for -- running an editor program that opens with
     * `#lang turmeric/sweet` used to wipe src_acc, so the next `#map{}` at the
     * prompt failed as "unknown function or operator 'hamt-of'"
     * (web-repl-lang-switch-drops-stdlib). */
    turi_env_pin_prelude(env);
    /* Register the interpreter native overrides (sym/:Sym, contracts, seq,
     * json/schema, safe box/unbox, comonad/mutex/future/chan/...) so the WASM
     * REPL can evaluate ops whose stdlib body is inline-C -- e.g. `#map{:a 1}`
     * needs the Hash[Sym]/Eq[Sym] natives, `assert!` needs the contract natives.
     * Relocated from src/main.c into tur_core so this entry point registers the
     * same block as --interpret and cannot drift (web-repl-repl-inline-c-native-
     * gap).  Runs AFTER the preload so the native shims win over the loaded
     * inline-C bodies. */
    turi_env_register_interpreter_natives(env);
}

/* ---------------------------------------------------------------------------
 * Public API functions (exported to WASM)
 * ---------------------------------------------------------------------------
 */

/* Initialize the Turmeric runtime for WASM.
 * Must be called once before any other turi_wasm_* function.
 * Returns 0 on success, non-zero on failure. */
int turi_wasm_init(void) {
    if (g_env) return 0; /* Already initialized */
    
    /* Use the unrestricted environment — the browser's WASM sandbox already
     * prevents real filesystem/network access.  Turmeric's sandboxed flag
     * would block println/print which we want routed to the JS print callback. */
    g_env = turi_env_new();
    if (!g_env) return 1;
    turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);

    /* Initialize diagnostics (no color in WASM - output goes to JS console) */
    turi_init(false);

    /* Preload the stdlib (macros + typed collections) so the REPL matches the
     * native --interpret path -- without this, `#map{}`/`#set{}` and every
     * macro (when/cond/for/...) failed as "unknown function or operator". */
    wasm_preload_stdlib(g_env);

    return 0;
}

/* Reset the evaluation environment to a fresh state.
 * This clears all definitions and returns to a clean slate. */
void turi_wasm_reset(void) {
    if (g_env) {
        turi_env_free(g_env);
        g_env = turi_env_new();
        if (g_env) {
            turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);
            /* Re-establish stdlib parity on the fresh env (matches init). */
            wasm_preload_stdlib(g_env);
        }
    }
}

/* Evaluate a Turmeric source string and return the result as a string.
 * 
 * Arguments:
 *   input - The Turmeric source code to evaluate (NUL-terminated)
 * 
 * Returns:
 *   A malloc'd string containing the result representation, or NULL on error.
 *   The caller is responsible for freeing this with turi_wasm_free().
 * 
 * On parse/runtime error, the error message is returned as the result string.
 */
char *turi_wasm_eval(const char *input) {
    if (!g_env) return turi_wasm_strdup("Error: Turmeric runtime not initialized. Call turi_wasm_init() first.");
    if (!input) return turi_wasm_strdup("Error: NULL input");

    turi_repl_set_last_diag_code("");

    char type_tag[64] = {0};
    TuriValue result = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));

    /* SI4: four-tier display:
     *   1. turi_try_show        -- TURI_STRUCT with Show instance
     *   2. turi_show_result     -- TURI_INT heap-pointer (Pair, Cons)
     *   3. turi_try_show_by_tag -- TURI_INT named ADT/struct/coll
     *                              (Vec, Set, Map, ...) via its Show
     *   4. turi_value_repr      -- default repr (fallback below) */
    const char *show_str = turi_try_show(g_env, result);
    if (!show_str)
        show_str = turi_show_result(g_env, result, type_tag);
    if (!show_str)
        show_str = turi_try_show_by_tag(g_env, result, type_tag);
    if (show_str) {
        char *ret = turi_wasm_strdup(show_str);
        free((char *)show_str);
        return ret;
    }

    char buf[2048];
    turi_value_repr(buf, sizeof(buf), result);
    return turi_wasm_strdup(buf);
}

/* Evaluate a Turmeric source string and return both result and any error.
 * 
 * This is useful when you want to distinguish between a successful evaluation
 * that returns a value, and an error case.
 * 
 * Arguments:
 *   input - The Turmeric source code to evaluate (NUL-terminated)
 *   out_result - Output pointer for the result string (malloc'd, NULL on error)
 *   out_error - Output pointer for the error string (malloc'd, NULL on success)
 * 
 * Returns:
 *   0 on success (result in *out_result, *out_error is NULL)
 *   1 on error (error message in *out_error, *out_result is NULL)
 * 
 * Caller must free both *out_result and *out_error with turi_wasm_free().
 */
int turi_wasm_eval_ex(const char *input, char **out_result, char **out_error) {
    if (!g_env) {
        *out_result = NULL;
        *out_error = turi_wasm_strdup("Error: Turmeric runtime not initialized. Call turi_wasm_init() first.");
        return 1;
    }
    if (!input) {
        *out_result = NULL;
        *out_error = turi_wasm_strdup("Error: NULL input");
        return 1;
    }
    
    *out_result = NULL;
    *out_error = NULL;

    turi_repl_set_last_diag_code("");

    char type_tag[64] = {0};
    TuriValue result = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));

    if (turi_is_error(result)) {
        char buf[2048];
        turi_value_repr(buf, sizeof(buf), result);
        *out_error = turi_wasm_strdup(buf);
        return 1;
    }

    /* SI4: four-tier display:
     *   1. turi_try_show        -- TURI_STRUCT with Show instance
     *   2. turi_show_result     -- TURI_INT heap-pointer (Pair, Cons)
     *   3. turi_try_show_by_tag -- TURI_INT named ADT/struct/coll
     *                              (Vec, Set, Map, ...) via its Show
     *   4. turi_value_repr      -- default repr (fallback below) */
    const char *show_str = turi_try_show(g_env, result);
    if (!show_str)
        show_str = turi_show_result(g_env, result, type_tag);
    if (!show_str)
        show_str = turi_try_show_by_tag(g_env, result, type_tag);
    if (show_str) {
        *out_result = turi_wasm_strdup(show_str);
        free((char *)show_str);
        return 0;
    }

    char buf[2048];
    turi_value_repr(buf, sizeof(buf), result);
    *out_result = turi_wasm_strdup(buf);
    return 0;
}

/* Free a string allocated by turi_wasm_eval or turi_wasm_eval_ex. */
void turi_wasm_free_string(char *s) {
    free(s);
}

/* Get the version of the Turmeric runtime.
 * Returns a static string - do not free. */
const char *turi_wasm_version(void) {
    return "Turmeric WASM " TURMERIC_VERSION;
}

/* ---------------------------------------------------------------------------
 * Multi-expression evaluation helpers
 * ---------------------------------------------------------------------------
 */

/* Evaluate multiple expressions in sequence.
 * Useful for batch execution.
 * 
 * Arguments:
 *   inputs - Array of NUL-terminated Turmeric source strings
 *   count - Number of inputs in the array
 *   outputs - Pre-allocated array of char* with count elements
 *             Each element will be set to a malloc'd result string
 *             (or NULL on error for that expression)
 * 
 * Returns:
 *   The number of successfully evaluated expressions.
 *   Caller must free each non-NULL output string with turi_wasm_free().
 */
int turi_wasm_eval_batch(const char **inputs, int count, char **outputs) {
    if (!g_env) return 0;
    if (!inputs || count <= 0 || !outputs) return 0;
    
    int success_count = 0;
    for (int i = 0; i < count; i++) {
        outputs[i] = NULL;
        if (!inputs[i]) {
            outputs[i] = turi_wasm_strdup("Error: NULL input");
            continue;
        }
        
        TuriValue result = turi_eval(g_env, inputs[i]);
        char buf[2048];
        turi_value_repr(buf, sizeof(buf), result);
        outputs[i] = turi_wasm_strdup(buf);
        
        if (!turi_is_error(result)) {
            success_count++;
        }
    }
    
    return success_count;
}

/* Format a Turmeric source string.
 * Returns a malloc'd formatted string, or NULL on parse error.
 * Caller must free with turi_wasm_free_string(). */
char *turi_wasm_format(const char *input) {
    if (!input) return NULL;

    size_t input_len = strlen(input);

    SourceFile file = {0};
    file.path        = "<format>";
    file.src         = input;
    file.len         = input_len;
    file.file_id     = 0;
    file.reader_type = READER_TURMERIC;
    diag_register_file(&file);

    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);

    char *result = NULL;

    if (forms && !diag_had_error()) {
        FmtOptions opts = {0};
        opts.indent_width = 2;
        opts.line_width   = 80;
        opts.src          = input;
        opts.src_len      = input_len;

        Buf out;
        buf_init(&out);
        if (fmt_print(&out, forms, nforms, opts) == 0) {
            result = turi_wasm_strdup(out.data ? out.data : "");
        }
        buf_free(&out);
    }

    symtab_free(&st);
    arena_free(&arena);
    /* Reset error state so subsequent eval calls are not poisoned */
    diag_reset();

    return result;
}

/* Shutdown the Turmeric WASM runtime.
 * Frees all resources. Call turi_wasm_init() again to restart. */
void turi_wasm_shutdown(void) {
    if (g_env) {
        turi_env_free(g_env);
        g_env = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Reader language mode (Phase S2)
 * ---------------------------------------------------------------------------
 */

/* Set the reader language mode for subsequent evaluations.
 *
 * Builds a "#lang <name>" string and runs it through detect_lang so the same
 * validation logic used by the REPL applies here too.
 *
 * Returns 0 on success, 1 if name is unknown. */
int turi_wasm_set_lang(const char *name) {
    if (!g_env || !name) return 1;

    /* Build a synthetic "#lang <name>" to reuse detect_lang's validation. */
    char buf[128];
    int written = snprintf(buf, sizeof(buf), "#lang %s", name);
    if (written < 0 || (size_t)written >= sizeof(buf)) return 1;

    const char *rest;
    size_t      rest_len;
    ReaderType rt = detect_lang(buf, (size_t)written, &rest, &rest_len);

    /* rest == buf means no #lang was recognised (pointer unchanged). */
    if (rest == buf || rt == READER_UNKNOWN || rt == (ReaderType)-1) return 1;

    if (rt != g_env->reader_type) {
        /* Keep the pinned stdlib preload across an explicit UI language switch,
         * for the same reason the inline `#lang` path does
         * (web-repl-lang-switch-drops-stdlib).  This site used to reset only the
         * source and the two counters, leaving n_acc_forms / acc_next_line / the
         * elaboration session describing forms read under the OLD reader. */
        turi_env_reset_to_prelude(g_env);
        g_env->reader_type = rt;
    }
    return 0;
}

/* Return the current reader language name as a static string. */
const char *turi_wasm_get_lang(void) {
    if (!g_env) return reader_type_name(READER_TURMERIC);
    return reader_type_name(g_env->reader_type);
}

/* ---------------------------------------------------------------------------
 * Doc lookup bridge (D6: autodoc integration)
 * ---------------------------------------------------------------------------
 */

/* Look up documentation for a Turmeric stdlib name.
 *
 * Evaluates (doc-lookup name) in the current runtime environment to retrieve
 * the pre-built docstring from stdlib/docstrings.tur.
 *
 * Returns a pointer to the static string stored in the docstrings table, or
 * NULL if the name is not found.  The returned pointer is owned by the
 * docstrings table (a C static array) and must NOT be freed by the caller.
 *
 * In WASM builds this function is kept alive by EMSCRIPTEN_KEEPALIVE so that
 * JavaScript can call it via Module.ccall / Module._turi_doc_lookup.
 */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_doc_lookup(const char *name) {
    if (!g_env || !name) return NULL;

    /* Check built-in documentation first (e.g. +, -, let, fn, etc.) */
    const char *builtin_doc = turi_doc_lookup_builtin(name);
    if (builtin_doc) return builtin_doc;

    /* Build a Turmeric expression: (doc-lookup "name") */
    size_t name_len = strlen(name);
    /* Allocate enough room for (doc-lookup "...") + escaping headroom */
    size_t buf_size = name_len * 2 + 32;
    char *expr = (char *)malloc(buf_size);
    if (!expr) return NULL;

    /* Build the expression, escaping backslashes and double-quotes */
    size_t pos = 0;
    const char prefix[] = "(doc-lookup \"";
    memcpy(expr + pos, prefix, sizeof(prefix) - 1);
    pos += sizeof(prefix) - 1;
    for (size_t i = 0; i < name_len; i++) {
        char c = name[i];
        if (c == '\\' || c == '"') {
            expr[pos++] = '\\';
        }
        expr[pos++] = c;
    }
    expr[pos++] = '"';
    expr[pos++] = ')';
    expr[pos]   = '\0';

    TuriValue result = turi_eval(g_env, expr);
    free(expr);

    if (turi_is_error(result)) return NULL;

    /* doc-lookup returns :cstr which is represented at runtime as TURI_INT
     * holding a const char * cast to int64_t.  A 0 value means "not found". */
    if (result.tag == TURI_CSTR) {
        return result.as_cstr;
    }
    if (result.tag == TURI_INT) {
        if (result.as_int == 0) return NULL;
        return (const char *)(intptr_t)result.as_int;
    }
    return NULL;
}

static void wasm_diag_sink(struct TuriEnv *env, int level, const char *code,
                           const char *file, uint32_t line,
                           uint32_t col_start, uint32_t col_end,
                           const char *message, void *ud) {
    (void)env; (void)file; (void)line; (void)col_start; (void)col_end; (void)ud;

    /* Capture first TUR-E#### or TUR-W#### code into the REPL's slot */
    const char *last = turi_repl_get_last_diag_code();
    if ((!last || last[0] == '\0') && code && (strncmp(code, "TUR-E", 5) == 0 || strncmp(code, "TUR-W", 5) == 0)) {
        turi_repl_set_last_diag_code(code);
    }

    /* Print diagnostic to stderr so it shows up in Web Console or printErr */
    const char *lvl_str = (level == 0) ? "error" : (level == 1) ? "warning" : (level == 2) ? "note" : "help";
    if (file && file[0] != '\0') {
        if (code && code[0] != '\0') {
            fprintf(stderr, "%s:%u:%u: %s [%s]: %s\n", file, line, col_start, lvl_str, code, message);
        } else {
            fprintf(stderr, "%s:%u:%u: %s: %s\n", file, line, col_start, lvl_str, message);
        }
    } else {
        if (code && code[0] != '\0') {
            fprintf(stderr, "%s [%s]: %s\n", lvl_str, code, message);
        } else {
            fprintf(stderr, "%s: %s\n", lvl_str, message);
        }
    }
}

static void type_of_diag_sink(struct TuriEnv *env, int level, const char *code,
                              const char *file, uint32_t line,
                              uint32_t col_start, uint32_t col_end,
                              const char *message, void *ud) {
    (void)env; (void)code; (void)file; (void)line; (void)col_start; (void)col_end;
    char *buf = (char *)ud;
    if (level == 0 && buf[0] == '\0') {
        snprintf(buf, 2048, "error: %s", message);
    }
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_type_of(const char *expr_src) {
    static char static_buf[2048];
    static_buf[0] = '\0';

    if (!g_env || !expr_src) return "";

    /* Install temporary diagnostic sink to capture type-elaboration errors */
    turi_env_set_diag_sink(g_env, type_of_diag_sink, static_buf);
    diag_reset();

    /* Build combined source so previous definitions are visible */
    Buf combined;
    buf_init(&combined);
    if (g_env->src_acc.len > 0) {
        buf_write(&combined, g_env->src_acc.data, g_env->src_acc.len);
    }
    buf_puts(&combined, expr_src);
    buf_putc(&combined, '\0');

    Arena    arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);

    SourceFile sfile = {0};
    sfile.path        = "<type>";
    sfile.src         = combined.data;
    sfile.len         = combined.len - 1;
    sfile.file_id     = 0;
    sfile.reader_type = g_env->reader_type;
    diag_register_file(&sfile);

    uint32_t nforms = 0;
    Form **forms = read_all_with_registry(&arena, &st, &sfile,
                                          g_env->reader_macros, &nforms);
    if (!forms || diag_had_error() || static_buf[0] != '\0') {
        goto cleanup;
    }

    Expr *prog = elaborate_program(&arena, &st, forms, nforms,
                                   /*stdlib_prefix=*/0, ".",
                                   /*separate_compilation=*/false,
                                   /*sandboxed=*/false,
                                   /*tc_env=*/NULL,
                                   /*include_dirs=*/NULL,
                                   /*n_include_dirs=*/0,
                                   /*out_n_fsd=*/NULL,
                                   g_env->reader_macros);
    if (!prog || diag_had_error() || static_buf[0] != '\0') {
        goto cleanup;
    }

    uint32_t n = prog->as.program.n;
    if (n == 0) {
        snprintf(static_buf, sizeof(static_buf), "empty expression");
    } else {
        Expr *last = prog->as.program.items[n - 1];
        snprintf(static_buf, sizeof(static_buf), "%s", type_name(last->type));
    }

cleanup:
    /* Restore diagnostic sink to default WASM sink */
    turi_env_set_diag_sink(g_env, wasm_diag_sink, g_env);

    arena_free(&arena);
    buf_free(&combined);

    return static_buf;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_explain(const char *code_or_null) {
    static char static_buf[8192];
    static_buf[0] = '\0';

    const char *code = code_or_null;
    if (!code || code[0] == '\0') {
        code = turi_repl_get_last_diag_code();
    }

    if (!code || code[0] == '\0') {
        return ":explain -- no recent diagnostic to explain. Try :explain TUR-E#### for a specific code.";
    }

    /* Normalise code to upper case */
    char norm_code[16];
    size_t len = strlen(code);
    if (len >= sizeof(norm_code)) {
        snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", code);
        return static_buf;
    }
    for (size_t i = 0; i < len; i++) {
        char c = code[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        norm_code[i] = c;
    }
    norm_code[len] = '\0';

    if (diag_looks_like_code(norm_code)) {
        DiagCode dc = diag_code_from_string(norm_code);
        if (dc != DIAG_CODE_NONE) {
            FILE *f = tmpfile();
            if (f) {
                if (diag_explain(dc, f)) {
                    rewind(f);
                    size_t bytes = fread(static_buf, 1, sizeof(static_buf) - 1, f);
                    static_buf[bytes] = '\0';
                } else {
                    snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", norm_code);
                }
                fclose(f);
            } else {
                snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", norm_code);
            }
        } else {
            snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", norm_code);
        }
    } else {
        snprintf(static_buf, sizeof(static_buf), "unknown diagnostic code '%s'", code);
    }

    return static_buf;
}
