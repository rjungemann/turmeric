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
#include "arena.h"
#include "buf.h"
#include "diag.h"
#include "fmt.h"
#include "reader.h"
#include "symbols.h"

/* ---------------------------------------------------------------------------
 * Global state for the WASM module
 * ---------------------------------------------------------------------------
 */

/* The single evaluation environment for the WASM module.
 * This is initialized once and reused across all eval calls. */
static TuriEnv *g_env = NULL;

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
    
    /* Initialize diagnostics (no color in WASM - output goes to JS console) */
    turi_init(false);
    
    return 0;
}

/* Reset the evaluation environment to a fresh state.
 * This clears all definitions and returns to a clean slate. */
void turi_wasm_reset(void) {
    if (g_env) {
        turi_env_free(g_env);
        g_env = turi_env_new();
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
    
    TuriValue result = turi_eval(g_env, input);
    
    /* Convert result to string representation */
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
    
    TuriValue result = turi_eval(g_env, input);
    
    if (turi_is_error(result)) {
        char buf[2048];
        turi_value_repr(buf, sizeof(buf), result);
        *out_error = turi_wasm_strdup(buf);
        return 1;
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
