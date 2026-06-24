#ifndef TURI_EVAL_H
#define TURI_EVAL_H

#include <stdio.h>

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

/* Like turi_eval, but also fills out_type_tag with the elaborated type name
 * of the last top-level expression (e.g. "int", "bool", "Point", "ptr<void>").
 * Passing NULL/0 for out_type_tag / tag_cap is equivalent to turi_eval. */
TuriValue turi_eval_typed(TuriEnv *env, const char *src,
                           char *out_type_tag, size_t tag_cap);

/* Attempt to call the Show typeclass instance for val.
 * Returns a heap-allocated C string (caller must free() it), or NULL when
 * no Show instance is registered for this value's concrete type.
 * Currently handles TURI_STRUCT values; primitives fall back to NULL. */
const char *turi_try_show(TuriEnv *env, TuriValue val);

/* SI4-C: Show a heap-pointer stdlib value using type_tag from turi_eval_typed.
 * Handles TURI_INT values whose elaborated type is "Pair" or "Cons".
 * Returns a heap-allocated C string (caller must free() it), or NULL when
 * the type_tag is not a known heap-pointer type. */
const char *turi_show_result(TuriEnv *env, TuriValue val, const char *type_tag);

/* ---------------------------------------------------------------------------
 * Debugger Phase 2: interactive interpreter debugger
 * --------------------------------------------------------------------------- */

/* Attach an interactive debugger to env.  After this call the eval loop checks
 * each AST node against the breakpoint table / step predicate and, on a hit,
 * yields to a command REPL on `in` (commands) / `out` (prompts + listings).
 * Pass NULL for in/out to use stdin/stdout.  Also registers the `(break)`
 * builtin (an explicit source-driven breakpoint).  Idempotent: a second call
 * is a no-op.  The debugger starts UNARMED -- no node will stop until
 * turi_debug_arm() is called, so prelude/top-level loading runs uninterrupted. */
void turi_debug_enable(TuriEnv *env, FILE *in, FILE *out);

/* Arm the attached debugger so it stops at the first eval node it sees (the
 * program-entry stop).  Call right before running the program (e.g. main). */
void turi_debug_arm(TuriEnv *env);

/* Detach and free the debugger (no-op if none attached). */
void turi_debug_disable(TuriEnv *env);

/* Register the `(break)` builtin without attaching a debugger.  With no
 * debugger present it is a no-op (returns nil), so a program containing
 * `(break)` runs unchanged under plain `tur --interpret`; under `tur debug`
 * the same call forces a pause.  turi_debug_enable also registers it. */
void turi_debug_register_break_builtin(TuriEnv *env);

/* ---------------------------------------------------------------------------
 * SB3 / SB4: Sandbox resource-limit and capability API
 * --------------------------------------------------------------------------- */

/* Default limits applied by turi_env_new_sandboxed (overridable at compile time). */
#ifndef TURI_DEFAULT_SANDBOX_FUEL
#  define TURI_DEFAULT_SANDBOX_FUEL  10000000u   /* 10M eval steps */
#endif
#ifndef TURI_DEFAULT_SANDBOX_DEPTH
#  define TURI_DEFAULT_SANDBOX_DEPTH 256u        /* max recursion frames */
#endif

/* Set the step-fuel limit for env.  Each call to the evaluator consumes one
 * unit.  When fuel reaches 0, turi_eval returns TURI_ERROR.
 * Pass 0 to disable fuel checking (default for unrestricted environments).
 * turi_env_new_sandboxed sets a default of TURI_DEFAULT_SANDBOX_FUEL. */
void turi_env_set_fuel(TuriEnv *env, uint64_t steps);

/* Override the maximum recursion depth.
 * Default: derived from the C stack limit (getrlimit(RLIMIT_STACK)) for
 * unrestricted envs so the guard fires before the native stack overflows;
 * TURI_DEFAULT_SANDBOX_DEPTH for sandboxed. */
void turi_env_set_max_depth(TuriEnv *env, uint32_t depth);

/* Grant a capability to an environment (no-op if already granted). */
void turi_env_allow(TuriEnv *env, TuriCaps cap);

/* Revoke a capability from an environment (no-op if already absent). */
void turi_env_deny(TuriEnv *env, TuriCaps cap);

/* Return true if the environment currently holds the given capability. */
bool turi_env_has_cap(TuriEnv *env, TuriCaps cap);

/* ---------------------------------------------------------------------------
 * Phase S7: Async C API
 * --------------------------------------------------------------------------- */

/* Register a native C function as a named global in env.
 * After this call, Turmeric code can call the function by name. */
void turi_env_register_native(TuriEnv *env, const char *name,
                               TuriNativeFn fn, void *ud);

/* Register eval-layer native builtins (struct-aware predicates, etc.). */
void turi_eval_register_builtins(TuriEnv *env);

/* Phase R2: raise a catchable interpreter panic (recoverable by catch-unwind,
 * with the standard message + double-panic guard).  Used by native functions
 * such as result-must / option-must instead of _exit(1).  Does not return. */
void turi_runtime_panic(TuriEnv *env, const char *msg);

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

/* DEPR-D0: turi_native_throw deleted with the (throw)/(try)/(catch) front
 * end; no callers remain.  See docs/upcoming/throw-deprecation-plan.md. */

/* Fire all remaining top-level/module-level deferred actions.
 * Call after turi_call(main) to honour module-level (defer ...) forms. */
void turi_run_pending_defers(TuriEnv *env);

/* W1b: read field `idx` of a struct VALUE as a TuriValue.  Lets the
 * Result/Option native shims (in main.c, where TuriStruct is incomplete) accept
 * a make-struct TuriStruct in addition to their native int64 box.  Sets *found
 * to true and returns the field when v is a TURI_STRUCT with idx in range; sets
 * *found to false and returns nil otherwise. */
TuriValue turi_struct_field(TuriValue v, uint32_t idx, bool *found);

/* Returns the constructor/struct name of a TURI_STRUCT value (e.g. "PureFree"),
 * or NULL when v is not a struct.  Lets natives in main.c dispatch on an ADT
 * constructor without the opaque TuriStruct layout. */
const char *turi_struct_name(TuriValue v);

/* Builds a TURI_STRUCT carrying constructor `name` and `n` fields (copied).
 * `name` must be a stable string (a literal or interned symbol); the value
 * matches a `(name ...)` pattern by name, so natives can return an ADT value
 * (e.g. a Left/Right) without the opaque TuriStruct layout. */
TuriValue turi_make_struct(TuriEnv *env, const char *name, TuriValue *fields, uint32_t n);

/* SEQ: advance a generator VALUE (carrier holds the TuriGen*) one step; returns
 * the yielded value and sets *done to 1 when the generator just exhausted.
 * Lets the seq inline-C natives (main.c) drive a TURI_GEN. */
TuriValue turi_gen_advance_val(TuriEnv *env, TuriValue gen, int *done);
/* True if the generator value has run off its end (or is null). */
bool turi_gen_done_val(TuriValue gen);

#endif /* TURI_EVAL_H */
