# `panic` in a value-position `if`-branch emits `(null) = ((void)0);`

> **Status:** Resolved (2026-07-08)
> **Fix:** `src/compiler/emit_expr.c` `emit_if_value` -- handle the mirror
> asymmetric case (only the else-branch diverges) symmetrically with the
> existing then-branch (`?`-operator) case: create the merge temp, emit the
> diverging branch as a plain statement, and assign the value branch. Also
> return the nil placeholder instead of a bare NULL when no temp is created
> (nil-typed `if`, or both branches diverge). Regression fixture:
> `tests/fixtures/panic-value-if-branch/`.

**Severity:** medium (miscompile -> C won't compile; blocks a natural `panic` shape)

## Summary

A function whose body is (or contains, in value/tail position) an `if` where one
branch is a bare `(panic ...)` and the other yields a value emits invalid C:
`(null) = ((void)0);`, where `null` is undeclared. The C compile then fails with
`'null' undeclared`.

## Minimal repro

```turmeric
(defn describe [r : (Result int int)] : int (if (ok? r) 10 (panic "boom")))
(defn main [] : int (println (describe (ok 1))) 0)
```

```
$ tur build repro.tur -o /tmp/repro
.../repro.tur.c: In function 'describe':
.../repro.tur.c:NNNN:14: error: 'null' undeclared (first use in this function)
     (null) = ((void)0);
```

It reproduces with `.is-ok`/`ok?` alike, and with the `if` in statement position
(as a discarded `do` item) too -- it is the `panic`-in-a-branch shape, not the
condition, that triggers it. Routing the panic through a helper call
(`(defn boom [] : int (panic "boom"))` then `(if (ok? r) 10 (boom))`) sidesteps
it -- a normal call in the branch emits fine.

## Root cause (suspected)

The value/assignment sink for an `if` whose value is consumed appears to emit an
assignment for the diverging (panic) branch using a placeholder destination named
`null` (the nil/void binding) as an lvalue, i.e. `(null) = ((void)0);`. A `panic`
branch diverges and yields no value, so it should emit the panic and no
assignment (like the `EX_PANIC` statement path does) rather than assigning
`(void)0` to a `null` lvalue. Likely in the if/assign lowering in
`src/compiler/emit_stmt.c` / `emit_expr.c` where a branch's value is bound to the
sink temp.

## Not fixed here

Found while adding the BR3b reader-panic fixture
(`stackless-catch-unwind-byref-aggregate-reader-panic`), which works around it by
routing the panic through a helper call. Orthogonal to the by-ref aggregate
trampoline work; filed for a separate fix.
