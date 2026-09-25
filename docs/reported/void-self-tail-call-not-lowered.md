# A self tail call in a `nil`-returning function is not lowered to a loop

**Severity:** medium. Every dialect. A `: nil` function that loops by calling
itself grows the C stack by one frame per iteration unless the C compiler
happens to turn the call into a jump, which gcc does at `-O2` and not at
`-O1`. Found by r7rs-lang-plan T8's sanitizer audit, where an ASan build of
`r7rs-write-labels` overflowed in the printer's `r7rs-cyc-finish__` loop.

## Repro

```turmeric
(defn count-down [n : int] : nil
  (if (= n 0) nil (count-down (- n 1))))
(defn count-down-int [n : int] : int
  (if (= n 0) 0 (count-down-int (- n 1))))
(defn main [] : int
  (count-down-int 10000000)
  (println "int ok")
  (count-down 10000000)
  (println "nil ok")
  0)
```

Measured 2026-09-25, Linux x86-64:

| build | result |
|---|---|
| `TUR_CC_FLAGS=-O2` (the default), gcc 13 | `int ok`, `nil ok` |
| `TUR_CC_FLAGS=-O1`, gcc 13 | segfault (exit 139) in `count-down` |
| `TUR_CC_FLAGS=-O1`, clang 18 | `int ok`, `nil ok` (clang makes the sibling call at -O1) |
| `tur --interpret` | `int ok`, `nil ok` |

`count-down-int` is a loop at every level. `tur emit-c` shows the
difference: `count_down_int` has `__tur_tailcall:` and a `goto`, while
`count_down` ends in a plain `count_down((n) - (INT64_C(1)));`.

The sanitized builds are the ones that meet it: the fixture leak gate
(`tests/run-leak-check.sh`) and T8's audit both build at `-O1` so ASan's
stack traces keep their frames.

## Root cause

`src/compiler/emit_fns.c:5990`, in `emit_fn_def`:

```c
bool tco_spine_ok = !body_diverges && fd->body->kind != EX_INLINE_C &&
    !(result_kind == TY_NIL && !is_main) && !is_main &&
    tco_params_simple(ctx, e, fd);
```

A `TY_NIL` result excludes the function before `tco_mark` looks at it, and
the body is emitted by the `result_kind == TY_NIL` arm at line 6197
(`emit_stmt(ctx, file, fd->body)`), which has no backedge. `emit_tail`, which
builds the loop, assumes every leaf ends in a `return <value>;` or a
backedge.

## Fix directions

- Admit `TY_NIL` in `tco_spine_ok` and teach `emit_tail` a void leaf: emit a
  non-self leaf as a statement followed by a bare `return;` (firing the same
  scope drops the value path fires), and the self leaf as the existing
  backedge. The `tc_check` walker (`^tailcall`) has to agree, as it does for
  the value path.
- Snapshots move for every `: nil` self-recursive function, in every dialect.
- When it lands, do the cleanup in
  [r7rs-prelude-value-returning-loop-workaround](r7rs-prelude-value-returning-loop-workaround.md).

## Workaround

Give the loop a value: `: bool` and `true` at the base case, with a `: nil`
wrapper if callers want nil. The prelude does this today (the cleanup report
above lists where).
