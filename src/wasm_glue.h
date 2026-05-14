#ifndef TURI_WASM_GLUE_H
#define TURI_WASM_GLUE_H

/* Version string for the WASM module */
#define TURMERIC_VERSION "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Initialization and lifecycle
 * ---------------------------------------------------------------------------
 */

/* Initialize the Turmeric runtime for WASM.
 * Must be called once before any other turi_wasm_* function.
 * Returns 0 on success, non-zero on failure. */
int turi_wasm_init(void);

/* Reset the evaluation environment to a fresh state.
 * This clears all definitions and returns to a clean slate. */
void turi_wasm_reset(void);

/* Shutdown the Turmeric WASM runtime.
 * Frees all resources. Call turi_wasm_init() again to restart. */
void turi_wasm_shutdown(void);

/* Get the version of the Turmeric runtime.
 * Returns a static string - do not free. */
const char *turi_wasm_version(void);

/* ---------------------------------------------------------------------------
 * Evaluation functions
 * ---------------------------------------------------------------------------
 */

/* Evaluate a Turmeric source string and return the result as a string.
 * 
 * Arguments:
 *   input - The Turmeric source code to evaluate (NUL-terminated)
 * 
 * Returns:
 *   A malloc'd string containing the result representation, or NULL on error.
 *   The caller is responsible for freeing this with turi_wasm_free_string().
 * 
 * On parse/runtime error, the error message is returned as the result string.
 */
char *turi_wasm_eval(const char *input);

/* Evaluate a Turmeric source string and return both result and any error.
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
 * Caller must free both *out_result and *out_error with turi_wasm_free_string().
 */
int turi_wasm_eval_ex(const char *input, char **out_result, char **out_error);

/* Evaluate multiple expressions in sequence.
 * 
 * Arguments:
 *   inputs - Array of NUL-terminated Turmeric source strings
 *   count - Number of inputs in the array
 *   outputs - Pre-allocated array of char* with count elements
 *             Each element will be set to a malloc'd result string
 * 
 * Returns:
 *   The number of successfully evaluated expressions.
 *   Caller must free each non-NULL output string with turi_wasm_free_string().
 */
int turi_wasm_eval_batch(const char **inputs, int count, char **outputs);

/* ---------------------------------------------------------------------------
 * Memory management
 * ---------------------------------------------------------------------------
 */

/* Free a string allocated by turi_wasm_eval, turi_wasm_eval_ex, or
 * turi_wasm_eval_batch. */
void turi_wasm_free_string(char *s);

/* ---------------------------------------------------------------------------
 * Helper functions (internal to the glue layer)
 * ---------------------------------------------------------------------------
 */

/* Allocate a copy of a string using malloc.
 * The result must be freed with turi_wasm_free_string(). */
char *turi_wasm_strdup(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* TURI_WASM_GLUE_H */
