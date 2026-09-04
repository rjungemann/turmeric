---
title: Sandboxing Guide
category: Interoperability
description: Running untrusted Turmeric code safely inside a C host using turi_env_new_sandboxed, capability flags, and resource limits
---

# Sandboxing Guide

The libturi embedding API provides a sandboxed evaluation environment for
running untrusted Turmeric code -- REPL widgets, plug-in scripts, user-supplied
formulas -- inside a C host process without exposing I/O, FFI, or unsafe memory
operations.

See [eval-api.md](eval-api.md) for the full C embedding API reference.

---

## Quick Start

```c
#include <turi/eval.h>

int main(void) {
    turi_init(false);
    TuriEnv *env = turi_env_new_sandboxed();

    TuriValue v = turi_eval(env, "(+ 1 2)");
    /* v.tag == TURI_INT, v.as_int == 3 */

    /* I/O is blocked */
    TuriValue bad = turi_eval(env, "(println-int 42)");
    /* bad.tag == TURI_ERROR */

    turi_env_free(env);
    return 0;
}
```

`turi_env_new_sandboxed()` returns an environment identical to `turi_env_new()`
except that the operations listed below are disabled.  Arithmetic, closures,
recursion, structs, algebraic effects, and pure stdlib functions all work
without restriction.

---

## What the Sandbox Blocks

### I/O builtins

All `println-*` variants (`println-int`, `println-float`, `println-bool`,
`println-cstr`, `println-uint`, `println-float32`) return `TURI_ERROR` in a
sandboxed environment.  File and socket I/O builtins (`read-async`,
`write-async`) are similarly blocked.

### FFI (dynamic loading)

`dlopen`, `dlsym`, and `dlclose` are blocked.  A sandboxed script cannot load
native shared libraries.

### Inline-C expressions

Any expression of the form `` (` ``c ... `` `) `` is rejected at evaluation
time with `TURI_ERROR`.

### Async forms

`(async ...)` is blocked.  Sandboxed code cannot spawn fibers or use the
cooperative scheduler.

---

## Resource Limits

A sandboxed environment caps total evaluation work so an infinite loop cannot
hang the host.

### Step fuel

The step-fuel counter decrements once per AST node evaluated.  When it reaches
zero, `turi_eval` returns `TURI_ERROR` immediately.

```c
/* Default sandboxed fuel: TURI_DEFAULT_SANDBOX_FUEL (10,000,000 steps).
 * Override after creating the environment: */
turi_env_set_fuel(env, 500000);   /* tighter limit for short formulas */
turi_env_set_fuel(env, 0);        /* 0 = unlimited (not recommended) */
```

Fuel checking is skipped entirely in unrestricted environments
(`step_fuel_limit == 0`), so there is no performance cost to non-sandboxed code.

### Recursion depth

There is no recursion-depth guard: interpreter recursion is heap-bounded, so
deep recursion cannot overflow the host's C stack. `turi_env_set_max_depth`
is retained as a **no-op** for API/ABI compatibility -- bound total work with
`turi_env_set_fuel` instead.

`TURI_DEFAULT_SANDBOX_FUEL` is `#define`d in `eval.h` and can be overridden at
compile time.

---

## Capability Allow-List

When you need a mix of restrictions -- for example, async is safe for your use
case but filesystem I/O is not -- use the capability API instead of the boolean
`sandboxed` flag.

```c
typedef uint32_t TuriCaps;

#define TURI_CAP_IO        (1u << 0)  /* println-*, file/socket I/O builtins */
#define TURI_CAP_FFI       (1u << 1)  /* dlopen/dlsym/dlclose */
#define TURI_CAP_INLINE_C  (1u << 2)  /* inline-C expressions */
#define TURI_CAP_ASYNC     (1u << 3)  /* (async ...) forms */
#define TURI_CAP_UNSAFE    (1u << 4)  /* raw-malloc, ptr-deref, ... */
#define TURI_CAP_IMPORT    (1u << 5)  /* (import ...) module loading */
#define TURI_CAP_ALL       (~(TuriCaps)0)
#define TURI_CAP_NONE      ((TuriCaps)0)
```

`turi_env_new()` grants `TURI_CAP_ALL`.  `turi_env_new_sandboxed()` starts with
`TURI_CAP_NONE`.  Use `turi_env_allow` and `turi_env_deny` to adjust:

```c
/* Sandbox that permits async but not I/O or FFI. */
TuriEnv *env = turi_env_new_sandboxed();
turi_env_allow(env, TURI_CAP_ASYNC);

TuriValue ok  = turi_eval(env, "(async (fn [] :int 42))"); /* allowed */
TuriValue bad = turi_eval(env, "(println-int 1)");         /* TURI_ERROR */
```

```c
/* Unrestricted env with I/O revoked -- useful inside a plugin host. */
TuriEnv *env = turi_env_new();
turi_env_deny(env, TURI_CAP_IO);
turi_env_deny(env, TURI_CAP_FFI);
```

Query whether a capability is active:

```c
if (!turi_env_has_cap(env, TURI_CAP_IO)) {
    /* safe to expose to user input */
}
```

### API summary

```c
/* Grant a capability to a sandboxed environment. */
void turi_env_allow(TuriEnv *env, TuriCaps cap);

/* Revoke a capability from any environment. */
void turi_env_deny(TuriEnv *env, TuriCaps cap);

/* Query whether a capability is currently granted. */
bool turi_env_has_cap(TuriEnv *env, TuriCaps cap);

/* Adjust the step-fuel limit. */
void turi_env_set_fuel(TuriEnv *env, uint64_t steps);

/* Retained as a no-op for API/ABI compatibility (the recursion-depth
 * guard is retired; interpreter recursion is heap-bounded). */
void turi_env_set_max_depth(TuriEnv *env, uint32_t depth);
```

---

## Blocking `(import ...)` and Pre-Loading Stdlib

`(import ...)` triggers filesystem reads.  In a sandboxed environment the
`TURI_CAP_IMPORT` bit is unset, so any `(import ...)` expression returns
`TURI_ERROR` without touching the filesystem.

If sandboxed scripts need stdlib functions, pre-load them in an unrestricted
environment and copy the bindings via `turi_env_set` before sandboxing:

```c
TuriEnv *boot = turi_env_new();
turi_eval_file(boot, "stdlib/list.tur");
TuriValue list_map = turi_env_get(boot, "list/map");

TuriEnv *sandbox = turi_env_new_sandboxed();
turi_env_set(sandbox, "list/map", list_map);   /* inject binding */

/* Sandboxed code can now call list/map without importing anything. */
turi_eval(sandbox, "(list/map (fn [x :int] :int (* x 2)) ...)");

turi_env_free(boot);
```

Do not grant `TURI_CAP_IMPORT` in a sandboxed environment unless you fully
control the filesystem paths reachable from the script.

---

## Native Functions in Sandboxed Environments

Sandboxed environments accept `turi_env_register_native` calls.  Your native
functions run with the same privileges as the host process; the sandbox only
constrains what *Turmeric code* can call.  Validate arguments carefully:

```c
static TuriValue safe_sqrt(TuriEnv *env, TuriValue *args,
                            uint32_t n, void *ud) {
    (void)ud;
    if (n != 1 || args[0].tag != TURI_FLOAT)
        return turi_error("sqrt: expected one float");
    double x = args[0].as_float;
    if (x < 0.0) return turi_error("sqrt: negative argument");
    return turi_float(sqrt(x));
}

turi_env_register_native(sandbox, "safe-sqrt", safe_sqrt, NULL);
```

Only expose native functions you are willing to let untrusted code call.

---

## Error Handling

Every blocked operation returns a `TURI_ERROR` value rather than aborting the
process.  Always check the return value:

```c
TuriValue v = turi_eval(env, user_input);
if (turi_is_error(v)) {
    fprintf(stderr, "sandbox error: %s\n", turi_error_message(v));
    /* recover, report, or discard */
}
```

Step-fuel exhaustion also surfaces as `TURI_ERROR`:

```
sandbox error: eval: step fuel exhausted
```

---

## Full Example -- Sandboxed Formula Evaluator

```c
#include <stdio.h>
#include <turi/eval.h>

int main(void) {
    turi_init(false);

    TuriEnv *env = turi_env_new_sandboxed();
    turi_env_set_fuel(env, 1000000);

    /* Expose a safe native. */
    turi_env_register_native(env, "safe-sqrt", safe_sqrt, NULL);

    /* User-supplied expressions evaluated in isolation. */
    const char *formulas[] = {
        "(defn hyp [a :float b :float] :float"
        "  (safe-sqrt (+ (* a a) (* b b))))",
        "(hyp 3.0 4.0)",
        "(hyp 5.0 12.0)",
        "(println-float 1.0)",   /* blocked -- no IO cap */
    };

    for (int i = 0; i < 4; i++) {
        TuriValue v = turi_eval(env, formulas[i]);
        if (turi_is_error(v)) {
            fprintf(stderr, "[error] %s\n", turi_error_message(v));
        } else {
            char buf[64];
            turi_value_repr(buf, sizeof(buf), v);
            printf("%s\n", buf);
        }
    }

    turi_env_free(env);
    return 0;
}
```

Expected output:

```
#<fn hyp>
5.0
13.0
[error] eval: builtin not allowed in sandboxed environment
```

---

## Capability Reference Table

| Capability | `TURI_CAP_*` bit | Blocked by default | What it covers |
|---|---|---|---|
| I/O | `TURI_CAP_IO` | yes | `println-*`, file/socket builtins |
| FFI | `TURI_CAP_FFI` | yes | `dlopen`, `dlsym`, `dlclose` |
| Inline-C | `TURI_CAP_INLINE_C` | yes | `` (` ``c ... `` `) `` expressions |
| Async | `TURI_CAP_ASYNC` | yes | `(async ...)` forms |
| Unsafe memory | `TURI_CAP_UNSAFE` | yes | `raw-malloc`, `ptr-deref`, `ptr-write`, `raw-memset`, ... |
| Import | `TURI_CAP_IMPORT` | yes | `(import ...)` module loading |

All six capabilities are denied when you call `turi_env_new_sandboxed()`.
Use `turi_env_allow` to selectively re-enable any subset.

---

## See Also

- [eval-api.md](eval-api.md) -- full C embedding API reference
- [c-integration-guide.md](c-integration-guide.md) -- FFI and inline-C
- [compiler-flags-guide.md](compiler-flags-guide.md) -- `-X` feature flags
