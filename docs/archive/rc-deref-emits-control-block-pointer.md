# `@x` on an `rc<T>` emits the control-block pointer, not the value

> **RESOLVED 2026-09-25.** `EX_DEREF` has a `TY_RC` arm
> (`src/compiler/emit_expr.c`) that reads the payload through
> `rc_get_value`, in the layout `EX_RC_OF` chose: a boxed or `:heap` ADT's
> control block adopts the ctor's pointer, so `cb->value` IS the carrier;
> every other payload is `*(T *)cb->value`.  Two more halves turned up while
> fixing it: `elab_deref` typed `@` on an `rc<ADT>` as a def-less `TY_ADT`
> (so `(match @s ...)` and a field read of the result could not resolve --
> it now uses the rc's `adt_def`), and the interpreter had the same bug as
> the emitter (`EX_DEREF` returned the `__rc` pair, which printed as the
> counter's address) -- it now returns the pair's value.  The CPS emitter has
> no `EX_DEREF` lowering of its own (it walks the operand and defers to
> `emit_value`), so it needed nothing.  Pinned by
> `tests/fixtures/rc-deref-reads-payload` (int, float, record, sum, `:heap`,
> and `rc/from-ref`; identical under `--interpret`; leak-checked).


**Severity: high** (silent wrong answer, no diagnostic). Found 2026-09-23 while
landing proper-tail-calls T4; pre-existing on `main` (reproduced with the T4
changes stashed).

## Repro

```turmeric
(defn main [] : int
  (let [x (rc/of 5)]
    (println (+ @x 1)))
  0)
```

`tur run` prints `94030894977800` (an address, varying run to run) instead of
`6`. `tur check` and `tur build` both exit 0, and the C compiles without a
warning, because the pointer lands in an `int64_t` expression.

A loop that tests `(= @x 0)` never terminates for the same reason.

## Root cause

`src/compiler/emit_expr.c`, the `EX_DEREF` arm of `emit_value` (~line 12377).
It has an arm for `ref<T>` / `lref<T>` (`*((T *)inner)`) and one for `&T` /
`&mut T`, and everything else falls into the `ptr<T>` arm, which returns the
operand unchanged. `rc<T>` reaches `EX_DEREF` too -- `expr.h` says so ("(@ r)
for rc<T> reuses EX_DEREF") -- so its `RcControlBlock *` is handed back as the
value.

## Fix direction

Give `TY_RC` its own arm: read the payload through the control block (the
`rc_set_value` counterpart the runtime already has, or the field the other rc
readers use), then cast to `type_c_name(e->type)` the way the `ref<T>` arm
does. A fixture should read `@x` for an `int` and a `float` payload (lead with
`7.1`, per CLAUDE.md), and check the CPS emitter's `EX_DEREF` handling for the
same gap.

T4's dead-local rule (`tco_drop_use_ok` in `emit_fns.c`) already treats
`@x` on an `rc` as a scalar read -- correct for the fixed emitter, and harmless
now, since the pointer is read before the frame fires.
