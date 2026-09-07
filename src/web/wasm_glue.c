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
#include "lang_layers.h"
#include "reader.h"
#include "runtime/experiments.h"
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
 * `name` is the full directive tail -- a base name optionally followed by
 * space-separated layer tokens (e.g. "turmeric/sweet stringed"), exactly the
 * text after `#lang `.  Builds a synthetic "#lang <name>" line and runs it
 * through detect_lang_layered so the same validation logic used by the REPL
 * and the interpreter applies here too.
 *
 * The layer set is ASSIGNED, never OR-ed: unlike an inline `#lang` line
 * arriving mid-session (src/turi/eval.c unions layers deliberately), this
 * entry point expresses the caller's complete desired state, so toggling a
 * layer off works.  A layer-set change resets the session the same way a
 * base change does -- losing session state on a language switch is the
 * already-accepted behaviour, and the UI warns before it happens.
 *
 * Returns 0 on success, 1 if the base or any layer token is unknown. */
int turi_wasm_set_lang(const char *name) {
    if (!g_env || !name) return 1;

    /* Build a synthetic "#lang <name>" to reuse detect_lang_layered's
     * validation. */
    char buf[256];
    int written = snprintf(buf, sizeof(buf), "#lang %s", name);
    if (written < 0 || (size_t)written >= sizeof(buf)) return 1;

    const char  *rest;
    size_t       rest_len;
    LangLayerSet layers  = 0;
    const char  *bad     = NULL;
    size_t       bad_len = 0;
    ReaderType rt = detect_lang_layered(buf, (size_t)written,
                                        &rest, &rest_len,
                                        &layers, &bad, &bad_len);

    /* rest == buf means no #lang was recognised (pointer unchanged). */
    if (rest == buf || rt == READER_UNKNOWN || rt == (ReaderType)-1) return 1;
    /* An unknown layer token is a hard reject, matching TUR-E0330 on the
     * compiled path -- silently ignoring it would leave the UI and the
     * environment disagreeing. */
    if (bad) return 1;

    /* Full switch (no-op when nothing changes): keeps the pinned stdlib
     * preload across an explicit UI language switch, for the same reason the
     * inline `#lang` path does (web-repl-lang-switch-drops-stdlib), and wipes
     * the session reader-macro registry so a dropped layer's dispatch
     * genuinely turns off. */
    turi_env_apply_lang(g_env, rt, layers);
    return 0;
}

/* Return the current reader language name as a static string. */
const char *turi_wasm_get_lang(void) {
    if (!g_env) return reader_type_name(READER_TURMERIC);
    return reader_type_name(g_env->reader_type);
}

/* ---------------------------------------------------------------------------
 * Language registry export (try-turmeric-lang-toggle-plan T1)
 * ---------------------------------------------------------------------------
 */

/* Human-readable base labels live HERE, next to the canonical names, not in
 * JS -- the UI renders what this table exports so it can never drift from
 * lang_base_from_name's accepted set.  The legacy `sweet-exp` alias is
 * accepted on input but deliberately not offered. */
static const struct {
    const char *name;
    const char *label;
} WASM_LANG_BASES[] = {
    { "turmeric",             "S-expression" },
    { "turmeric/curly-infix", "Curly-infix" },
    { "turmeric/neoteric",    "Neoteric" },
    { "turmeric/sweet",       "Sweet-expression" },
};

/* Append `s` to `b` as a JSON string body (no surrounding quotes). */
static void wasm_json_escape(Buf *b, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') {
            buf_putc(b, '\\');
            buf_putc(b, c);
        } else if ((unsigned char)c < 0x20) {
            buf_printf(b, "\\u%04x", (unsigned char)c);
        } else {
            buf_putc(b, c);
        }
    }
}

/* Return the `#lang` registry as JSON:
 *
 *   {"bases":[{"name":"turmeric","label":"S-expression"},...],
 *    "layers":[{"name":"stringed","kind":"reader",
 *               "summary":"#s\"...\" owned-String literal","since":"v1",
 *               "available":true}]}
 *
 * Bases come from the WASM_LANG_BASES table above (mirroring
 * lang_base_from_name's canonical set); layers are walked live from
 * LANG_LAYERS[] via the same accessors that back `tur lang-layers`, so the
 * playground picker and the CLI listing cannot disagree.  `available` is
 * false for a semantic layer whose backing experiment no longer exists --
 * such a row renders disabled, never hidden.
 *
 * The returned string is built once and owned by this module; the caller
 * must NOT free it. */
const char *turi_wasm_lang_registry(void) {
    static char *cached = NULL;
    if (cached) return cached;

    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"bases\":[");
    size_t nbases = sizeof(WASM_LANG_BASES) / sizeof(WASM_LANG_BASES[0]);
    for (size_t i = 0; i < nbases; i++) {
        if (i) buf_putc(&b, ',');
        buf_puts(&b, "{\"name\":\"");
        wasm_json_escape(&b, WASM_LANG_BASES[i].name);
        buf_puts(&b, "\",\"label\":\"");
        wasm_json_escape(&b, WASM_LANG_BASES[i].label);
        buf_puts(&b, "\"}");
    }
    buf_puts(&b, "],\"layers\":[");
    size_t nlayers = lang_layers_count();
    for (size_t i = 0; i < nlayers; i++) {
        const LangLayerDescriptor *d = lang_layer_at(i);
        if (!d) continue;
        if (i) buf_putc(&b, ',');
        buf_puts(&b, "{\"name\":\"");
        wasm_json_escape(&b, d->name);
        buf_puts(&b, "\",\"kind\":\"");
        buf_puts(&b, d->kind == LAYER_SEMANTIC ? "semantic" : "reader");
        buf_puts(&b, "\",\"summary\":\"");
        wasm_json_escape(&b, d->summary);
        buf_puts(&b, "\",\"since\":\"");
        wasm_json_escape(&b, d->since);
        buf_puts(&b, "\",\"available\":");
        bool available = true;
        if (d->kind == LAYER_SEMANTIC) {
            /* A semantic layer is only offerable while its EXPERIMENTS[] row
             * exists; the layer IS the enable, so a missing row means the
             * token would hard-error. */
            available = d->experiment && experiment_lookup(d->experiment);
        }
        buf_puts(&b, available ? "true" : "false");
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]}");
    buf_putc(&b, '\0');   /* Buf does not NUL-terminate; strdup needs it */

    cached = turi_wasm_strdup(b.data ? b.data : "{\"bases\":[],\"layers\":[]}");
    buf_free(&b);
    return cached;
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

/* ---------------------------------------------------------------------------
 * Time-travel tracer bridge (docs/archive/try-turmeric-tracer-plan.md, T3)
 *
 * The recorder, the reader and the replay are all in turi/trace.c, which has
 * compiled into this module since it landed -- it was simply dead-stripped,
 * because nothing in -sEXPORTED_FUNCTIONS reached it.  This section is the
 * reach.
 *
 * The page never decodes a .turtrace byte.  A recording stays in wasm memory
 * and the timeline asks turi_trace_replay_* the same questions `tur dap`'s
 * stepBack asks, so there is one decoder for the format rather than a C one
 * and a JavaScript one drifting apart.
 * ---------------------------------------------------------------------------
 */

#include "turi/trace.h"

/* A recording outlives the run that produced it: the replay reads straight out
 * of the trace's buffer, so the TurTrace is stopped but not ended until the
 * next Trace press. */
static TurTrace       *g_trace  = NULL;
static TurTraceReplay *g_replay = NULL;

/* The line in the session's accumulated blob at which THIS run's source began.
 *
 * The browser env accumulates every eval into env->src_acc and hands the whole
 * blob to the reader, so an interpreter line number is absolute in that blob,
 * not relative to what is in the editor -- which is why a five-line tab
 * reports its errors at `<eval>:75`.  A timeline that lit up line 75 of a
 * five-line file would be worse than no timeline, so the base is captured
 * before the run and the page subtracts it. */
static uint32_t g_trace_base_line = 1;

/* Lines already accumulated, +1: where the next appended chunk starts. */
static uint32_t wasm_acc_base_line(void) {
    if (!g_env) return 1;
    if (g_env->acc_next_line > 0) return g_env->acc_next_line;
    uint32_t line = 1;
    for (size_t i = 0; i < g_env->src_acc.len; i++)
        if (g_env->src_acc.data[i] == '\n') line++;
    /* turi_eval_typed joins the accumulated blob and the new chunk with a
     * newline (eval.c: `if (env->src_acc.len > 0) buf_putc(&env->src_acc, '\n')`),
     * so a non-empty accumulator costs one more line than it contains. */
    if (g_env->src_acc.len > 0) line++;
    return line;
}

static void wasm_trace_clear(void) {
    if (g_replay) { turi_trace_replay_free(g_replay); g_replay = NULL; }
    if (g_trace)  { turi_trace_end(g_trace); g_trace = NULL; }
}

/* Record a program.  `has_main` comes from the page, which already decides the
 * same question for Run (definesMainEntry in web/main.js): a program defining
 * a top-level `main` loads its forms and then runs `(main)`.  Keeping the rule
 * on one side means Run and Trace cannot disagree about where a program
 * starts.
 *
 * Returns the number of recorded steps, -1 if the recorder could not attach or
 * the program failed to load, and -2 when `has_main` was claimed and no `main`
 * closure materialized.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int turi_wasm_trace_run(const char *input, uint32_t max_steps, int has_main) {
    if (!g_env || !input) return -1;
    wasm_trace_clear();
    turi_repl_set_last_diag_code("");
    g_trace_base_line = wasm_acc_base_line();

    /* The order is cmd_trace's (src/main.c:7502) by way of cmd_eval_h: attach
     * the debugger, install the recorder on it, load the program UNARMED so
     * the recording is of the program and not of its definitions, then arm and
     * call main. */
    turi_debug_enable(g_env, NULL, NULL);

    TurTraceOpts opts;
    memset(&opts, 0, sizeof opts);
    opts.max_steps = max_steps;
    /* Kept on in the browser.  output_drain writes every captured byte back
     * out to the saved descriptor as well (trace.c: "the tracer is a recorder,
     * not a muzzle"), so the console still streams live while the OUTPUT
     * records accumulate for the scrub. */
    opts.capture_output = true;
    /* The memset leaves grain at TUR_TRACE_GRAIN_NODE, which is what the
     * timeline wants: a browser scrub is the one client that can actually show
     * sub-expression movement, because it owns the editor and the site carries
     * a column range. */

    g_trace = turi_trace_begin(g_env, &opts);
    if (!g_trace) {
        turi_debug_disable(g_env);
        return -1;
    }

    int rc = 0;
    char type_tag[64] = {0};

    if (has_main) {
        TuriValue top = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));
        if (turi_is_error(top)) {
            rc = -1;
        } else {
            TuriValue main_fn = turi_env_get(g_env, "main");
            if (main_fn.tag == TURI_CLOSURE) {
                turi_debug_arm(g_env);
                (void)turi_call(g_env, main_fn, NULL, 0);
            } else {
                rc = -2;
            }
        }
    } else {
        /* No entry point, so the top-level forms are the program.  Arm first
         * or there is nothing to record. */
        turi_debug_arm(g_env);
        TuriValue top = turi_eval_typed(g_env, input, type_tag, sizeof(type_tag));
        if (turi_is_error(top)) rc = -1;
    }

    /* Not optional.  The recorder owns the env's pause handler and this module
     * keeps ONE env for the life of the tab -- a handler left installed turns
     * every later eval at the prompt into a traced one. */
    turi_trace_stop(g_trace);
    turi_debug_disable(g_env);

    if (rc != 0) {
        wasm_trace_clear();
        return rc;
    }

    size_t len = 0;
    const uint8_t *bytes = turi_trace_bytes(g_trace, &len);
    if (bytes) g_replay = turi_trace_replay_open(bytes, len);
    if (!g_replay) {
        wasm_trace_clear();
        return -1;
    }
    return (int)turi_trace_replay_steps(g_replay);
}

/* Recording-level facts, as JSON.  `truncated` is the header flag: the step
 * cap ended the run, and the UI must say so rather than present a partial
 * recording as a whole one. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_stats(void) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_trace || !g_replay) return "{\"steps\":0}";

    TurTraceStats st;
    memset(&st, 0, sizeof st);
    turi_trace_stats(g_trace, &st);
    size_t len = 0;
    (void)turi_trace_bytes(g_trace, &len);

    Buf b;
    buf_init(&b);
    buf_printf(&b,
        "{\"steps\":%u,\"enters\":%u,\"pops\":%u,\"changes\":%u,"
        "\"peakDepth\":%u,\"outputBytes\":%u,\"truncated\":%s,\"bytes\":%u,"
        "\"baseLine\":%u}",
        (unsigned)turi_trace_replay_steps(g_replay), (unsigned)st.enters,
        (unsigned)st.pops, (unsigned)st.changes, (unsigned)st.peak_depth,
        (unsigned)st.output_bytes, st.truncated ? "true" : "false",
        (unsigned)len, (unsigned)g_trace_base_line);
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "{\"steps\":0}");
    buf_free(&b);
    return cached;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint32_t turi_wasm_trace_seek(uint32_t index) {
    if (!g_replay) return 0;
    return turi_trace_replay_seek(g_replay, index);
}

/* The whole cursor as one JSON payload: index, the frame stack innermost
 * first, each frame's bindings, and the program output produced strictly
 * before this step.
 *
 * One message rather than four, because every one of these changes on every
 * seek and a scrub issues a seek per animation frame. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_state(void) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_replay) return "{\"index\":0,\"steps\":0,\"frames\":[],\"output\":\"\"}";

    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"index\":%u,\"steps\":%u,\"frames\":[",
               (unsigned)turi_trace_replay_index(g_replay),
               (unsigned)turi_trace_replay_steps(g_replay));

    int nframes = turi_trace_replay_frame_count(g_replay);
    for (int i = 0; i < nframes; i++) {
        TurTraceFrame f;
        memset(&f, 0, sizeof f);
        if (!turi_trace_replay_frame_at(g_replay, i, &f)) continue;
        if (i) buf_putc(&b, ',');
        buf_puts(&b, "{\"fn\":\"");
        wasm_json_escape(&b, f.fn_name);
        buf_puts(&b, "\",\"file\":\"");
        wasm_json_escape(&b, f.file_path);
        /* endCol is exclusive, and 0 when the recording carries no range (a v1
         * file).  A client that wants to highlight the expression rather than
         * the line checks for 0 rather than assuming a range is there. */
        buf_printf(&b, "\",\"line\":%u,\"col\":%u,\"endCol\":%u,\"locals\":[",
                   (unsigned)f.line, (unsigned)f.col, (unsigned)f.col_end);
        int nlocals = turi_trace_replay_local_count(g_replay, i);
        for (int j = 0; j < nlocals; j++) {
            const char *name = NULL, *repr = NULL;
            if (!turi_trace_replay_local_at(g_replay, i, j, &name, &repr))
                continue;
            if (j) buf_putc(&b, ',');
            buf_puts(&b, "{\"name\":\"");
            wasm_json_escape(&b, name);
            buf_puts(&b, "\",\"repr\":\"");
            wasm_json_escape(&b, repr);
            buf_puts(&b, "\"}");
        }
        buf_puts(&b, "]}");
    }

    buf_puts(&b, "],\"output\":\"");
    size_t olen = 0;
    const char *out = turi_trace_replay_output(g_replay, &olen);
    if (out) {
        /* wasm_json_escape wants a NUL-terminated string and the output buffer
         * is a length-counted blob, so escape it by hand on the same rules. */
        for (size_t i = 0; i < olen; i++) {
            char c = out[i];
            if (c == '"' || c == '\\') { buf_putc(&b, '\\'); buf_putc(&b, c); }
            else if ((unsigned char)c < 0x20) buf_printf(&b, "\\u%04x", (unsigned char)c);
            else buf_putc(&b, c);
        }
    }
    buf_puts(&b, "\"}");
    buf_putc(&b, '\0');

    cached = turi_wasm_strdup(b.data ? b.data : "{\"index\":0,\"steps\":0,\"frames\":[],\"output\":\"\"}");
    buf_free(&b);
    return cached;
}

/* The site and depth of an arbitrary step, WITHOUT seeking.
 *
 * This is the one that makes a timeline affordable: drawing a depth ribbon or
 * marking which lines were ever executed asks this of thousands of indices,
 * and a seek rebuilds state from the start of the stream each time.  trace.h
 * spells out the consequence of confusing the two: "which is not slow, it is a
 * hang". */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_site_at(uint32_t index) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_replay) return "{\"line\":0,\"depth\":0,\"file\":\"\"}";

    const char *file = NULL;
    uint32_t line = 0;
    bool ok = turi_trace_replay_site_at(g_replay, index, &file, &line);
    int depth = turi_trace_replay_depth_at(g_replay, index);

    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"file\":\"");
    if (ok) wasm_json_escape(&b, file);
    buf_printf(&b, "\",\"line\":%u,\"depth\":%d}", (unsigned)(ok ? line : 0), depth);
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "{\"line\":0,\"depth\":0,\"file\":\"\"}");
    buf_free(&b);
    return cached;
}

/* Scan for the next step on `line`, forwards (dir > 0) or backwards.  A miss
 * returns the boundary index with hit=false, which is what a continue with no
 * breakpoint ahead of it should do -- and what stops the UI presenting "ran to
 * the end" as "found it". */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_find_line(int dir, const char *file, uint32_t line) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_replay) return "{\"index\":0,\"hit\":false}";

    bool hit = false;
    uint32_t idx = turi_trace_replay_find_line(g_replay, dir, file ? file : "",
                                               line, &hit);
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"index\":%u,\"hit\":%s}", (unsigned)idx,
               hit ? "true" : "false");
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "{\"index\":0,\"hit\":false}");
    buf_free(&b);
    return cached;
}

/* Every OUTPUT record in the recording, concatenated.
 *
 * turi_trace_replay_output answers "what had been printed by the cursor's
 * step", which is the right question everywhere except at the end of the run:
 * a program whose last act is a println drains it AFTER the final STEP, so the
 * trailing OUTPUT records sit past every stop point and the transcript at the
 * last step is empty.  Scrubbing to the end of `(println (fib 6))` and being
 * told nothing was printed reads as a broken timeline, so the page asks this
 * instead once the cursor is on the last step.
 *
 * Built on the public reader rather than by changing the replay, because the
 * replay's semantics are correct for its own question and `tur dap` depends on
 * them. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const char *turi_wasm_trace_output_full(void) {
    static char *cached = NULL;
    free(cached);
    cached = NULL;
    if (!g_trace) return "";

    size_t len = 0;
    const uint8_t *bytes = turi_trace_bytes(g_trace, &len);
    TurTraceReader r;
    if (!bytes || !turi_trace_open(&r, bytes, len)) return "";

    Buf b;
    buf_init(&b);
    TurTraceRecord rec;
    while (turi_trace_next(&r, &rec)) {
        if (rec.tag != TUR_TRACE_OUTPUT || !rec.payload) continue;
        buf_write(&b, (const char *)rec.payload, rec.payload_len);
    }
    buf_putc(&b, '\0');
    cached = turi_wasm_strdup(b.data ? b.data : "");
    buf_free(&b);
    return cached;
}

/* The raw recording, so the page can offer it as a .turtrace download that
 * `tur trace --dump` reads.  Bytes, not a string: a NUL inside a rendered
 * value would truncate it. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
const uint8_t *turi_wasm_trace_buffer(void) {
    if (!g_trace) return NULL;
    size_t len = 0;
    return turi_trace_bytes(g_trace, &len);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
uint32_t turi_wasm_trace_buffer_len(void) {
    if (!g_trace) return 0;
    size_t len = 0;
    (void)turi_trace_bytes(g_trace, &len);
    return (uint32_t)len;
}

/* Drop the recording.  Called when the timeline closes, so a megabyte of trace
 * does not sit in wasm memory for the rest of the session. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void turi_wasm_trace_release(void) {
    wasm_trace_clear();
}

/* ---------------------------------------------------------------------------
 * SX8c: the solver, answerable from the playground
 *
 * The refinement solver is already IN the wasm module -- `compiler/refine_*.c`
 * are `TUR_CORE_SOURCES`, so every browser session has been shipping S0..S3 all
 * along.  This makes it visible: the "static checking at zero download cost"
 * story is hard to believe from a page that cannot demonstrate it.
 *
 * The semantics are `tur smt`'s, deliberately and to the letter -- the same
 * session reader (so `(push)`/`(pop)` scope assertions and each `(check-sat)`
 * is answered where it appears), the same S0..S3 chain in the same order, the
 * same bounded model search, and the same "a script with no `(check-sat)` is
 * decided once at the end".  A window onto the solver has to show the same
 * solver from every angle; a browser build that answered differently from the
 * CLI would be a second solver wearing the first one's name.
 *
 * Read-only by construction, which is what SX8c's own spec asks for: nothing
 * here touches elaboration or discharge, so no answer from this API can elide
 * a runtime check.  Interrogation proposes; the C chain in a real compile
 * decides.
 * ---------------------------------------------------------------------------
 */

#include "compiler/refine_smtlib.h"
#include "compiler/refine_solver.h"

static void smt_json_str(Buf *out, const char *s) {
    buf_putc(out, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"':  buf_puts(out, "\\\""); break;
            case '\\': buf_puts(out, "\\\\"); break;
            case '\n': buf_puts(out, "\\n");  break;
            case '\r': buf_puts(out, "\\r");  break;
            case '\t': buf_puts(out, "\\t");  break;
            default:
                if (*p < 0x20) buf_printf(out, "\\u%04x", *p);
                else           buf_putc(out, (char)*p);
        }
    }
    buf_putc(out, '"');
}

static void smt_json_model(Buf *out, const RefineModel *m) {
    buf_puts(out, "[");
    for (uint32_t i = 0; m && i < m->n; i++) {
        const RefineModelBinding *b = &m->bindings[i];
        if (i) buf_puts(out, ",");
        buf_puts(out, "{\"name\":");
        smt_json_str(out, b->name);
        if (b->is_real) buf_printf(out, ",\"sort\":\"Real\",\"value\":%g", b->rval);
        else            buf_printf(out, ",\"sort\":\"Int\",\"value\":%lld",
                                   (long long)b->ival);
        buf_puts(out, "}");
    }
    buf_puts(out, "]");
}

/* One `(check-sat)`'s worth of work: run the chain, then the bounded search if
 * nothing proved it.  Mirrors `smt_answer` in main.c. */
static void smt_one_answer(RefineVC *vc, Arena *arena, Buf *out) {
    /* refine-chain-expands-the-same-dnf-four-times: the `_cc` stages, sharing
     * ONE run's cube set.  Each stage used to rebuild the same DNF from an
     * unchanged vc, so a query reaching S3 expanded it four times.  Same
     * verdicts by construction -- the set is identical, it is just built once.
     * The `refine_sN_decide` entry points still exist for a single-stage
     * caller; a chain has no use for them. */
    static const struct { const char *name;
                          RefineDecision (*fn)(RefineVC *, Arena *,
                                               RefineCubeCache *); } CHAIN[] = {
        { "S0 (trivial)",       refine_s0_decide_cc },
        { "S1 (EUF)",           refine_s1_decide_cc },
        { "S2 (arithmetic)",    refine_s2_decide_cc },
        { "S3 (Nelson-Oppen)",  refine_s3_decide_cc },
    };
    const char *decided_by = NULL;
    RefineVerdict v = RT_UNKNOWN;
    RefineCubeCache cc = {0};
    for (size_t i = 0; i < sizeof(CHAIN)/sizeof(CHAIN[0]); i++) {
        RefineDecision d = CHAIN[i].fn(vc, arena, &cc);
        if (d.verdict != RT_UNKNOWN) { v = d.verdict; decided_by = CHAIN[i].name; break; }
    }

    /* The goal is `false`, so VALID means the assertion set is UNSAT.  The
     * stages only ever prove; `sat` has to come from the bounded search, which
     * is the only thing in the solver allowed to answer INVALID -- and it does
     * so with a witness rather than a guess, which is why the model rides
     * along here instead of being a separate call. */
    buf_puts(out, "{\"answer\":");
    if (v == RT_VALID) {
        smt_json_str(out, "unsat");
    } else {
        RefineModel *m = refine_model_search(vc, arena);
        if (m) {
            smt_json_str(out, "sat");
            decided_by = "bounded model search";
            buf_puts(out, ",\"model\":");
            smt_json_model(out, m);
        } else {
            smt_json_str(out, "unknown");
        }
    }
    if (decided_by) {
        buf_puts(out, ",\"decided_by\":");
        smt_json_str(out, decided_by);
    }
    buf_puts(out, "}");
}

/* `Buf` carries a length and does NOT NUL-terminate -- it is written for
 * buf_to_file, where the length is the contract.  Handing `b->data` straight
 * back as a C string is therefore an overread, and a silent one: the JSON
 * parses fine and trailing heap garbage rides along behind the closing brace.
 * Every exit from turi_smt_check goes through here.  (`grow` is realloc-backed,
 * so the result is free()-able, which is what the JS side does.) */
static char *smt_finish(Buf *b) {
    buf_putc(b, '\0');
    return b->data ? b->data : turi_wasm_strdup("");
}

/* Answer an SMT-LIB2 script.  Returns a malloc'd JSON string the caller frees
 * with turi_wasm_free_string (JS: Module._free).
 *
 *   {"schema":1,"results":[{"answer":"unsat","decided_by":"S2 (arithmetic)"}]}
 *   {"schema":1,"results":[],"error":"outside the accepted fragment: ..."}
 *
 * `schema` is 1: the shape is stable since SX9, matching `--dump-refine=json`
 * (schema 0 was the same shape flagged unstable).
 * `results` carries one entry per `(check-sat)`, in script order.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
char *turi_smt_check(const char *smtlib) {
    Buf out; buf_init(&out);
    buf_puts(&out, "{\"schema\":1,\"results\":[");
    if (!smtlib) {
        buf_puts(&out, "],\"error\":\"no input\"}");
        return smt_finish(&out);
    }

    Arena arena;
    arena_init(&arena, 1 << 20);
    SmtlibSession *s = refine_smtlib_session_new(&arena);
    refine_smtlib_session_feed(s, smtlib, strlen(smtlib));

    const char *err = NULL;
    bool answered = false;
    for (bool done = false; !done; ) {
        switch (refine_smtlib_session_step(s)) {
            case SMT_EV_CHECK_SAT:
                if (answered) buf_puts(&out, ",");
                smt_one_answer(refine_smtlib_session_vc(s), &arena, &out);
                answered = true;
                break;
            case SMT_EV_ERROR:
                err = refine_smtlib_session_err(s);
                if (!err) err = "unsupported script";
                done = true;
                break;
            case SMT_EV_END:
            case SMT_EV_EXIT:
                done = true;
                break;
            /* `(get-model)` is a no-op here: the witness already rides along
             * with the `sat` it belongs to, so a caller never has to ask. */
            case SMT_EV_GET_MODEL:
            case SMT_EV_OK:
                break;
        }
    }

    /* A script that never asked is still decided once, exactly as `tur smt`
     * does it -- every corpus benchmark relies on that, and two doors onto one
     * solver should not disagree about what an unasked script means. */
    if (!err && !answered)
        smt_one_answer(refine_smtlib_session_vc(s), &arena, &out);

    buf_puts(&out, "]");
    if (err) {
        buf_puts(&out, ",\"error\":");
        smt_json_str(&out, err);
    }
    buf_puts(&out, "}");
    arena_free(&arena);
    return smt_finish(&out);
}
