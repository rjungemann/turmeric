# libturi — C Embedding API Reference

`libturi` is the Turmeric eval runtime packaged as a static library (`libturi.a`).
It lets C programs evaluate Turmeric expressions, call Turmeric functions, and
expose C functions to Turmeric code — all without spawning a subprocess.

---

## Quick start

```c
#include <turi/eval.h>

int main(void) {
    turi_init(/*use_color=*/false);

    TuriEnv *env = turi_env_new();

    TuriValue v = turi_eval(env, "(+ 1 2)");
    /* v.tag == TURI_INT, v.as_int == 3 */

    turi_env_free(env);
    return 0;
}
```text

Compile and link:

```sh
cc -o demo demo.c -I/usr/local/include/turi \
   -L/usr/local/lib -lturi -lpthread
```

---

## Headers

| Header | Purpose |
|--------|---------|
| `turi/eval.h`  | Main API — eval, init, async helpers |
| `turi/env.h`   | `TuriEnv` type and environment operations |
| `turi/value.h` | `TuriValue` tagged union and constructors |

Include only `turi/eval.h`; it pulls in the others transitively.

---

## Initialisation

### `void turi_init(bool use_color)`

Initialises the diagnostics subsystem.  Call **once** before the first
`turi_eval`.  Pass `use_color = true` to enable ANSI colour in error messages
(suitable when stderr is a terminal).

---

## Environments

An environment (`TuriEnv`) holds all accumulated definitions and per-call
arenas.  Closures captured from an environment must not outlive it.

### `TuriEnv *turi_env_new(void)`

Creates an unrestricted environment.  All builtins — including I/O — are
available.

### `TuriEnv *turi_env_new_sandboxed(void)`

Creates a sandboxed environment.  I/O builtins (`println`, `read-async`,
`write-async`, …) and inline-C expressions are disabled.  Suitable for
evaluating untrusted code.

### `void turi_env_free(TuriEnv *env)`

Frees all resources owned by `env`.  Any `TuriValue` containing a closure,
struct, or future that was produced by this environment becomes invalid after
this call.

### `TuriValue turi_env_get(TuriEnv *env, const char *name)`

Looks up a global binding by name.  Returns a `TURI_ERROR` value if the name
is not bound.

### `void turi_env_set(TuriEnv *env, const char *name, TuriValue value)`

Creates or replaces a global binding.  Useful for injecting values from C into
Turmeric code before evaluating expressions that reference them.

---

## Evaluation

### `TuriValue turi_eval(TuriEnv *env, const char *src)`

Evaluates a Turmeric source string in `env`.  Prior definitions in `env` remain
visible.  New top-level definitions (`defn`, `def`) are stored back into `env`
for subsequent calls.

On parse or elaboration error the diagnostic is emitted to stderr and the
returned value has tag `TURI_ERROR`.

The returned value is valid until `turi_env_free(env)`.

```c
/* Define a function, then call it in a later eval. */
turi_eval(env, "(defn double [x :int] :int (* x 2))");
TuriValue r = turi_eval(env, "(double 21)");
/* r.as_int == 42 */
```turmeric

### `TuriValue turi_eval_file(TuriEnv *env, const char *path)`

Reads `path` and evaluates its contents via `turi_eval`.  Returns a `TURI_ERROR`
if the file cannot be opened.

---

## Values — `TuriValue`

`TuriValue` is a tagged union:

```c
typedef struct TuriValue {
    TuriTag tag;
    union {
        bool             as_bool;
        int64_t          as_int;
        double           as_float;
        const char      *as_cstr;    /* NUL-terminated string */
        TuriClosure     *as_closure;
        const char      *as_error;   /* error message */
        TuriEffectCont  *as_cont;
        TuriStruct      *as_struct;
        TuriThrow       *as_throw;
        TuriFuture      *as_future;
    };
} TuriValue;
```

### Tags

| Tag | Meaning |
|-----|---------|
| `TURI_NIL`    | Unit / void — `()`, `nil` |
| `TURI_BOOL`   | Boolean — `true` / `false` |
| `TURI_INT`    | 64-bit signed integer |
| `TURI_FLOAT`  | 64-bit float (double) |
| `TURI_CSTR`   | NUL-terminated C string |
| `TURI_CLOSURE`| First-class function |
| `TURI_ERROR`  | Runtime / parse error (not catchable by `try/catch`) |
| `TURI_EFFECT_CONT` | Live algebraic-effect continuation |
| `TURI_STRUCT` | Struct instance |
| `TURI_THROW`  | In-flight exception (catchable by `try/catch`) |
| `TURI_FUTURE` | Async future handle |

### Constructors

```c
TuriValue turi_nil(void);
TuriValue turi_bool(bool b);
TuriValue turi_int(int64_t i);
TuriValue turi_float(double f);
TuriValue turi_cstr(const char *s);   /* borrows s — caller keeps it alive */
TuriValue turi_error(const char *msg);
TuriValue turi_errorf(const char *fmt, ...);
```turmeric

### Predicates and accessors

```c
bool        turi_is_error(TuriValue v);
bool        turi_is_truthy(TuriValue v);
bool        turi_is_throw(TuriValue v);
bool        turi_as_bool(TuriValue v);
int64_t     turi_as_int(TuriValue v);
double      turi_as_float(TuriValue v);
const char *turi_as_cstr(TuriValue v);
const char *turi_error_message(TuriValue v);
```

### Printing

```c
void turi_value_repr(char *buf, size_t cap, TuriValue v);
```text

Writes a human-readable representation into `buf` (at most `cap` bytes,
NUL-terminated).  Examples: `"42"`, `"\"hello\""`, `"#<fn double>"`,
`"true"`.

```c
void turi_print_value(FILE *out, TuriValue v);
```

Prints the same representation to `out`.

---

## Registering native C functions

### `void turi_env_register_native(TuriEnv *env, const char *name, TuriNativeFn fn, void *ud)`

Registers a C function so that Turmeric code can call it by `name`.

```c
typedef TuriValue (*TuriNativeFn)(TuriEnv *env, TuriValue *args,
                                   uint32_t n, void *ud);
```text

- `args` — array of evaluated arguments (length `n`).
- `ud` — the `void *ud` passed to `turi_env_register_native`.
- Return `turi_nil()` for void functions; return an appropriate `TuriValue`
  for functions that produce results.
- To raise a catchable exception call `turi_native_throw` then return
  `turi_nil()`.

```c
static TuriValue native_add(TuriEnv *env, TuriValue *args,
                             uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n != 2) return turi_error("add: expected 2 args");
    return turi_int(args[0].as_int + args[1].as_int);
}

turi_env_register_native(env, "my-add", native_add, NULL);
TuriValue r = turi_eval(env, "(my-add 10 32)");
/* r.as_int == 42 */
```

### `void turi_native_throw(TuriEnv *env, const char *msg)`

Raises a catchable Turmeric exception from inside a `TuriNativeFn`.  Sets
`env->throwing` and `env->throw_value`; the native must return `turi_nil()`
immediately afterwards.

```c
static TuriValue native_fail(TuriEnv *env, TuriValue *args,
                              uint32_t n, void *ud) {
    turi_native_throw(env, "something went wrong");
    return turi_nil();
}
```turmeric

---

## Async API

The async scheduler is cooperative and single-threaded.  `(async ...)` spawns
a fiber; `(await ...)` suspends the current fiber until a future resolves.
From C, call `turi_run_event_loop` after evaluating async code to drive
completion.

### `void turi_run_event_loop(TuriEnv *env)`

Runs the cooperative scheduler until all pending fibers, timers, and I/O
callbacks complete.  Call after any `turi_eval` that starts async tasks.

```c
turi_eval(env, "(defn work [] :int (await (async (fn [] :int 42))))");
TuriValue r = turi_eval(env, "(work)");
/* r.as_int == 42  — (await ...) in main context drives the loop inline */
```

### `TuriValue turi_task_spawn(TuriEnv *env, const char *src)`

Evaluates `src` as a zero-argument closure and spawns it as an async task.
Returns a `TURI_FUTURE` value.

### `void turi_task_cancel(TuriEnv *env, TuriFuture *f)`

Cancels the fiber owning `f`.  The future is rejected; any `(await f)` will
throw a cancellation exception.

### `TuriValue turi_future_poll_val(TuriFuture *f)`

Non-blocking poll.  Returns the resolved value, a `TURI_ERROR` if rejected,
or `TURI_NIL` if still pending.

### `TuriValue turi_sleep_async(TuriEnv *env, uint64_t ms)`

Returns a `TURI_FUTURE` that resolves after `ms` milliseconds.  The current
fiber (if any) should `(await)` this future to suspend.

---

## Thread safety

`TuriEnv` is **not** thread-safe.  Use one environment per thread, or protect
access with an external lock.

---

## Error handling pattern

```c
TuriValue v = turi_eval(env, user_input);
if (turi_is_error(v)) {
    fprintf(stderr, "error: %s\n", turi_error_message(v));
}
```turmeric

Note: `TURI_ERROR` is a value-level error (parse failure, unbound variable).
`TURI_THROW` is a Turmeric exception thrown by `(throw ...)` or
`turi_native_throw`.  Uncaught throws surface as `TURI_THROW` values returned
from `turi_eval`; check with `turi_is_throw(v)`.

---

## Full example — sandboxed calculator

```c
#include <stdio.h>
#include <turi/eval.h>

int main(void) {
    turi_init(false);
    TuriEnv *env = turi_env_new_sandboxed();

    const char *exprs[] = {
        "(defn fact [n :int] :int (if (<= n 1) 1 (* n (fact (- n 1)))))",
        "(fact 10)",
        "(fact 20)",
    };

    for (int i = 0; i < 3; i++) {
        TuriValue v = turi_eval(env, exprs[i]);
        if (turi_is_error(v)) {
            fprintf(stderr, "error: %s\n", turi_error_message(v));
        } else {
            char buf[64];
            turi_value_repr(buf, sizeof(buf), v);
            printf("%s => %s\n", exprs[i], buf);
        }
    }

    turi_env_free(env);
    return 0;
}
```
