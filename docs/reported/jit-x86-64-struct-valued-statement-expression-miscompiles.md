# JIT (x86-64): a struct-valued `({ ... })` clobbers a sibling local

**Severity: medium (JIT engine, x86-64 only).** Filed 2026-09-10 while
driving the Saffron dynamic-surface PR to green.

## Summary

Under the MIR engine (`tur jit`, `tests/run-jit.sh`) on x86-64 Linux, a GNU
statement expression whose value is a by-value struct -- `tur_tagged_t` (the
`any` box, 16 bytes) or a by-value ADT -- is miscompiled in at least two
positions: as an argument in a call's argument list, and as the initializer of
a local. The observed effect is that a *sibling* value is overwritten: another
argument of the same call, or another parameter of the enclosing function. The
same C is correct under gcc and clang, and the same program is correct in the
engine on arm64 (the macOS runner).

The emitter no longer produces any of the shapes below, so no fixture hits it
today; the engine defect itself is open. It is a MIR / c2mir bug, not a
codegen one, and the Turmeric-side work is to keep the emitter out of the
shape until the engine is fixed upstream.

## Repro (three shapes, all pinned by fixtures that now run in the engine)

1. **Argument position, struct-valued `?:` or if/else inside `({ ... })`.**
   The carrier -> `any` bridge, spelled as
   `f(acc, ({ int64_t p = ...; p ? *(tur_tagged_t *)p : TUR_TAG(nil, 0); }))`,
   reached the callee with the *first* argument replaced by the second:
   `saffron-prelude`'s `(vec-fold [1.5 2.25] 0.0 (fn [a x] (+ a x)))` printed
   `4.5` (2.25 + 2.25) for 3.75; `saffron-container-param-cast-shape` printed
   `1` for `4.25`. Rewriting the bridge's interior as if/else into a local
   changed nothing -- the position is the trigger, not the conditional.
2. **Argument position, malloc'd box.**
   `each(TUR_TAG(fn_id, &fatbox), ({ T *b = malloc(...); *b = xs; TUR_TAG(id, b); }))`
   reached `each` with the first argument's tag wrong
   (`cannot call a unknown value -- it is not a function`).
3. **Initializer position, by-value ADT cast.**
   `tur_adt_Lst s = ({ tur_tagged_t c = xs; __tur_any_cast_check(...); *(tur_adt_Lst *)TUR_UNTAG(c); });`
   as a `match` scrutinee clobbered the function's other parameter `f`, so
   `(defn app [f xs] (match xs (Cons h t) (f 5) (Nil) 0))` panicked at `(f 5)`
   -- while the same match with `(f 5)` moved before it, or the same call
   without the match, was fine.

Minimal program for shape 3 (`tur jit` on x86-64 panics; `tur run` prints 5):

```turmeric
#lang saffron
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn app4 [f xs]
  (match xs
    (Cons h t) (f 5)
    (Nil)      0))
(defn show [x] (println x))
(defn main [] : int
  (app4 show (Cons 1 (Nil)))
  0)
```

`TUR_JIT_DUMP_C=<path>` now writes the exact text handed to c2mir on the
no-split path too, so the shapes can be read off the engine's own input.

## What the emitter does now

Every site that produced one of those shapes builds the value with statements
in the enclosing body and leaves a plain expression behind:

- `emit_core.c` carrier -> `any` bridge: a call to a per-TU `static inline`
  helper `__tur_any_of_carrier(int64_t)` (`ensure_any_carrier_bridge`,
  emit_module.c), emitted on demand.
- `emit_expr.c` dynamic call: the callee box is bound to a fresh temp by a
  statement, the check runs there, and the call is a bare prototype-cast
  call (this also dropped the `(tur_tagged_t)(a)` identity struct casts
  `TUR_APPLYn_T` made, which c2mir rejects outright -- see the JIT guide).
- `emit_expr.c` EX_UNION_INJECT by-value aggregate: the malloc'd box is built
  by statements; the expression is `TUR_TAG(id, tmp)`.
- `emit_expr.c` by-value ADT cast out of `any`: the box read and the tag
  check are statements; the expression is the dereference.

Still emitted as struct-valued statement expressions, none observed to
misbehave yet: the dynamic method call (`__tur_dm`), the union widen
(`__tur_ua`), `dyn_widen_to_any`'s by-value box (`__tur_fb`), and
`emit_core.c`'s `__tur_pbox`. If a fixture that reaches one of them starts
answering differently in the engine on Linux only, this is the first thing to
suspect.

## Fix directions

1. Upstream: reduce shape 3 to plain C (a 16-byte struct returned from a
   `({ ... })` initializer next to another struct local) against c2mir +
   MIR-gen on x86-64 and file it on rjungemann/mir. Likely a temp-slot reuse in
   the statement-expression lowering when the value is a block (BLK) type.
2. Meanwhile, keep the emitter out of the shape (above), and if one of the
   remaining sites is implicated, hoist it the same way.
