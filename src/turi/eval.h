#ifndef TURI_EVAL_H
#define TURI_EVAL_H

#include "env.h"
#include "fiber.h"
#include "value.h"

/* ---------------------------------------------------------------------------
 * Public eval API (Phase S0)
 * --------------------------------------------------------------------------- */

/* Evaluate a Turmeric source string in the given environment.
 * Prior definitions in `env` remain visible.  New top-level definitions
 * (defn, def) are stored back into `env` for subsequent calls.
 *
 * On parse/elaboration error the diagnostic is already emitted to stderr;
 * the returned value has tag TURI_ERROR with a short description.
 *
 * The returned value is valid until `turi_env_free(env)`.  Closures hold
 * internal pointers into env-owned arenas and must not outlive env. */
TuriValue turi_eval(TuriEnv *env, const char *src);

/* Evaluate the contents of a file. */
TuriValue turi_eval_file(TuriEnv *env, const char *path);

/* Call a closure value directly with the given arguments.
 * Bypasses re-elaboration, so macros from previous elaboration calls
 * remain usable inside the closure body.  The closure must have been
 * obtained from turi_env_get() after a turi_eval_file() call.
 * Returns TURI_ERROR on arity mismatch or runtime error. */
TuriValue turi_call(TuriEnv *env, TuriValue fn, TuriValue *args, uint32_t n_args);

/* Initialise the diagnostics subsystem for standalone libturi use.
 * Call once before the first turi_eval.  `use_color` enables ANSI colour
 * in error messages; pass false when output is not a terminal. */
void turi_init(bool use_color);

/* Write a human-readable REPL representation of v into buf (at most cap
 * bytes, NUL-terminated). */
void turi_value_repr(char *buf, size_t cap, TuriValue v);

/* ---------------------------------------------------------------------------
 * Phase S7: Async C API
 * --------------------------------------------------------------------------- */

/* Register a native C function as a named global in env.
 * After this call, Turmeric code can call the function by name. */
void turi_env_register_native(TuriEnv *env, const char *name,
                               TuriNativeFn fn, void *ud);

/* Run the cooperative event loop until all async fibers and timers complete. */
void turi_run_event_loop(TuriEnv *env);

/* Spawn a new async task from Turmeric source; returns TURI_FUTURE value.
 * src must evaluate to a zero-argument closure or a direct expression. */
TuriValue turi_task_spawn(TuriEnv *env, const char *src);

/* Cancel a task future (marks owner fiber as cancelled, rejects future). */
void turi_task_cancel(TuriEnv *env, TuriFuture *f);

/* Non-blocking poll of a future; returns result/error or TURI_NIL if pending. */
TuriValue turi_future_poll_val(TuriFuture *f);

/* Start an async sleep for ms milliseconds; returns TURI_FUTURE → nil. */
TuriValue turi_sleep_async(TuriEnv *env, uint64_t ms);

/* Throw a catchable exception from a native (TuriNativeFn) function.
 * Sets env->throwing and env->throw_value; the native should return
 * turi_nil() immediately after calling this. */
void turi_native_throw(TuriEnv *env, const char *msg);

/* Fire all remaining top-level/module-level deferred actions.
 * Call after turi_call(main) to honour module-level (defer ...) forms. */
void turi_run_pending_defers(TuriEnv *env);

#endif /* TURI_EVAL_H */
