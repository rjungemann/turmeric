---
title: Closure-carrier return/arg emit omits the int<->pointer bridging cast (-Wint-conversion noise)
category: Reported
severity: low
description: Two cosmetic `-Wint-conversion` warnings in `emit-c` output for the two-level Signal-Function shape. (1) A `defn` whose inferred result is itself a function type `(fn ...)` is lowered to the `int64_t` closure carrier, but when its body is a bare thin-fn reference the emitter writes `return __fn_NNN;` with no `(int64_t)(intptr_t)` cast -- clang warns "returning 'void * (*)(...)' from a function with return type 'int64_t' makes integer from pointer without a cast". (2) After the two-level-SF fix retypes an already-fat `^fat` argument to the `:ptr<void>` carrier, the thin local-fn call site casts the callee's parameter to `void *` but passes the arg's underlying `int64_t` C variable uncoerced -- "passing argument 1 ... makes pointer from integer without a cast". Both are benign on LP64 (pointer and int64 are the same width) and the program runs correctly; they are codegen-hygiene warts, not miscompiles.
---

# Closure-carrier return/arg emit omits the int<->pointer bridging cast

## Summary

`emit-c` on the two-level Signal-Function shape (the
`sf-let-bind-with-inner-call` fixture) compiles and runs correctly, but the
generated C draws two `-Wint-conversion` warnings. Both are the *same* root
phenomenon -- a value whose Turmeric type is a function/closure (`TY_FN`) is
carried in C as the `int64_t` closure carrier, but at one boundary the
emitter forgets the `(int64_t)(intptr_t)` / `(void *)(intptr_t)` bridge cast
that reconciles the pointer form with the carrier.

Severity: **low / cosmetic.** On any LP64 target a function pointer and an
`int64_t` are the same width, so the implicit conversion is value-preserving
and the program behaves correctly (the `sf-let-bind-with-inner-call` fixture
prints `7`). It is codegen hygiene -- noise that hides genuine warnings and
that a stricter compiler (`-Werror`, some Apple clang configs) would reject.

Found while executing
`docs/reported/sf-two-level-closure-return-miscompiles-out-binding.md`.
Warning (1) is pre-existing; warning (2) was newly *introduced* by that fix
(it replaced a hard segfault, so it is a strict improvement, but it is still
a wart worth closing).

## Observed

Building the fixture (or `/tmp/sfcap.tur`):

```turmeric
(defn make-sf []
  (fn [^fat sig : (fn [float] float)]
    (fn [t : float] : float (sig t))))
(defn scale2 [x : float] : float (* x 2.0))
(defn drive [^fat input : (fn [float] float)] : float
  (let [sf  (make-sf)
        out (sf input)]
    (out 3.5)))
(defn main [] : int (println (drive scale2)) 0)
```

emits:

```c
static int64_t make_hysf() {
        return __fn_898;          /* __fn_898 : void *(int64_t) */
}
...
static double drive(int64_t input) {
        ...
        int64_t out_903 = (int64_t)(intptr_t)(((void * (*)(void *))(intptr_t)sf)(input));
        ...
}
```

```
warning: returning 'void * (*)(int64_t)' from a function with return type
  'int64_t' makes integer from pointer without a cast [-Wint-conversion]
    return __fn_898;
           ^~~~~~~~
warning: passing argument 1 of '(void * (*)(void *))sf' makes pointer from
  integer without a cast [-Wint-conversion]
    int64_t out_903 = ... ((void * (*)(void *))(intptr_t)sf)(input) ...
```

## Expected

```c
static int64_t make_hysf() {
        return (int64_t)(intptr_t)__fn_898;
}
...
        int64_t out_903 = (int64_t)(intptr_t)(
            ((void * (*)(void *))(intptr_t)sf)((void *)(intptr_t)input));
```

No `-Wint-conversion` warnings; identical runtime behavior.

## Root cause

### (1) Closure-returning `defn` return value -- `src/compiler/emit_fns.c:630`

The trailing-return emitter already special-cases a bare-fn-reference body
returned through the `int64_t` carrier, but the guard only fires for
`result_kind == TY_INT`:

```c
} else if (result_kind == TY_INT && fd->body->type.kind == TY_FN) {
    /* ... A bare non-capturing function reference (TY_FN) returned as the
     * int64_t function-pointer carrier needs an explicit cast. ... */
    buf_printf(file, "return (int64_t)(intptr_t)%s;\n", ret_val);
} else {
    buf_printf(file, "return %s;\n", ret_val);
}
```

`make-sf` has no declared return type; its inferred result is the outer
closure `(fn [sig] (fn [t] float))`, so `result_kind == TY_FN` (not
`TY_INT`). `type_c_name(TY_FN)` lowers a function-returning-function to the
`int64_t` carrier (`src/compiler/types.c:1941`), so the C return type is
`int64_t` while the body value `__fn_898` is a `void *(...)` pointer -- but
the `TY_INT`-only guard skips the coercion and the plain `return %s;` branch
runs. Widening the guard to also accept `result_kind == TY_FN` (any closure
whose carrier C type is `int64_t`/`void *` while the body yields a thin fn
pointer) closes it.

### (2) Already-fat arg into a carrier param -- `src/compiler/emit_expr.c` (thin local-fn call branch, arg cast ~line 2055)

The two-level-SF fix retypes an already-fat `^fat` argument to the
`:ptr<void>` carrier in `elab_call.c` so it is not double-boxed. That makes
the call-site arg-type lower to `void *`:

```c
buf_puts(&out, type_c_name(e->as.call_.args[i]->type));   /* now "void *" */
...
buf_puts(&out, arg_strs[i]);                               /* still "input" : int64_t */
```

but the underlying C variable (`drive`'s parameter `input`) is declared
`int64_t`, so the emitted value is an `int64_t` handed to a `void *` slot
with no `(void *)(intptr_t)` cast. The arg-emission loop in that branch
should bridge a pointer-typed param fed an `int64_t`-carrier value the same
way the surrounding code already coerces carrier mismatches (e.g. the CC2
path at `emit_expr.c:~1872`).

## Minimal reproducer

`tests/fixtures/sf-let-bind-with-inner-call/input.tur` (already in tree).
Build it and observe the two warnings; the binary still prints `7`.

## Proposed fix directions

- **(1)** In `emit_fns.c` extend the bare-fn-return guard from
  `result_kind == TY_INT` to also cover `result_kind == TY_FN` (closure
  carrier). Use `(int64_t)(intptr_t)` for an `int64_t` carrier and
  `(void *)(intptr_t)` for a `void *` carrier, keyed off the function's C
  return type (`type_c_name(result_full_type)`), not just the kind.
- **(2)** In the thin local-fn call branch of `emit_expr.c`, when a formal
  param lowers to a pointer C type (`void *`, `... *`) and the actual arg's
  emitted C value is an `int64_t` carrier (`TY_PTR_VOID`/`TY_FN`/`TY_INT`
  carrier-typed binding), wrap the arg in `(void *)(intptr_t)(...)` -- the
  same coercion the closure-thunk call path already performs for the reverse
  (pointer -> int64_t) direction.

## Validation of a fix

- `tur run tests/fixtures/sf-let-bind-with-inner-call/input.tur 2>&1 | grep
  -c Wint-conversion` is `0`.
- The fixture still prints `7`; `bash tests/run.sh` stays green.
- Spot-check that no fixture `expected.c` snapshot churns in an unintended
  way (regenerate per the Fixture Snapshots rule if the boilerplate shifts).

## Related

- `docs/reported/sf-two-level-closure-return-miscompiles-out-binding.md`
  -- the miscompile this was found alongside (now fixed).
