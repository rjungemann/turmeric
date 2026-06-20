---
title: Constrained existential `pack` of a by-value struct payload emitted an invalid `(int64_t)(struct)` cast
category: Bug Report
status: resolved
component: compiler/emit + compiler/elab (existentials)
affects: turmeric main 0.21.0
resolved-in: claude/spice-reported-issues-cs1utr
severity: medium
---

# Constrained `pack` of a struct payload -- RESOLVED (guarded by a diagnostic)

## Summary

`(pack (make-struct T ...) (exists [a] [(C a)] a))` -- a constraint-carrying
existential whose payload is a by-value struct -- previously emitted C that
cast the struct directly to `int64_t` for the record's `value` field, which
the C compiler rejected:

```
error: aggregate value used where an integer was expected
  __t43->value = (int64_t)((LinesR){.v = INT64_C(5)});
```

It is now rejected at elaboration with an actionable diagnostic that names the
supported workaround, instead of miscompiling to C the backend cannot build.

## How it surfaced

This was the Track C / `turmeric-spices` `plot` P3 finding: the
`plot-renderer-typeclass-plan` boxes mixed renderer structs (`PointsR`,
`LinesR`, `FunctionR`, ...) into a constrained existential `AnyRenderer` so a
`(vec-of (function ...) (points ...))` type-checks under one `Renderer`
dictionary. The plan's Risk 1 explicitly anticipated this and said to "pause
and file a report rather than forcing it." The investigation confirmed the
report and pinned a *second*, deeper layer beyond the cast (see below).

## Repro (turmeric main 0.21.0, pre-fix)

```turmeric
(defmodule p (export)
  (defclass Rdr [a] (rbound [x] : int))
  (defstruct LinesR [v : int])
  (definstance Rdr [LinesR] (rbound [x] (.v x)))
  (defn main [] : int
    (let [e (pack (make-struct LinesR 5) (exists [a] [(Rdr a)] a))]
      0)))
```

Post-fix this emits:

```
error: pack: a by-value struct payload ('LinesR') is not supported in a
constraint-carrying existential -- the record stores its payload in an int64
carrier and dispatches witnesses on it. Wrap the value in a 'defopaque T :int'
newtype (or use a :ptr<T> handle) so the payload is carrier-representable.
```

## Root cause (two layers)

1. **Pack cast (the filed defect).** `emit_value` for `EX_EXISTS_PACK`
   (`src/compiler/emit_expr.c`) handled a by-value aggregate payload only on
   the *unconstrained* path (heap-boxing it via
   `exists_payload_is_byval_aggregate`). The constrained path (`n_witnesses >
   0`) unconditionally stored `%s->value = (int64_t)(%s)`, valid only for
   scalar/pointer payloads -- not a `struct` rvalue.

2. **Dispatch ABI (the deeper layer).** Even with the cast fixed by
   heap-boxing, a *constrained* existential payload is abstract: the only
   thing you can do with the `open`-bound value is call its constraint
   methods. Existential dispatch (`EX_EXISTS_DISPATCH`, `emit_expr.c`) flows
   the receiver through the int64 carrier and erases class-variable-typed
   params to `int64_t`, calling the witness slot cast to `int64_t (*)(int64_t,
   ...)`. But a by-value struct instance method is emitted as
   `int64_t __inst_Rdr_rbound_LinesR(LinesR)` -- a by-value-struct ABI that
   cannot be called through the carrier signature. So heap-boxing alone would
   make `pack` compile but leave `open`+dispatch a runtime-ABI trap. The
   carrier-compatible payload path (plain `:int`, `:ptr<void>`, opaque-over-int
   newtypes) matches the carrier dispatch ABI and already works end to end
   (`tests/fixtures/exists-open-witness-dispatch`).

Because the *constrained* payload is only reachable through dispatch, a
heap-box-only fix has no real use case (it would only help the degenerate
"pack and ignore" form) while introducing the dispatch trap. The bounded,
safe resolution is therefore a clear diagnostic, not a partial lowering.

## Resolution

`elab_pack` (`src/compiler/elab_types.c`) now rejects a by-value `defstruct`
payload (non-opaque, `n_type_params == 0`, not a transparent int newtype) in a
constraint-carrying existential, with a diagnostic that points at the
carrier-representable workaround. Coverage:
`tests/fixtures/errors/exists-pack-byval-struct-payload`.

## Remaining enhancement (not a bug)

Supporting a by-value struct payload through a *constrained* existential
end to end is a real feature, scoped here for whoever picks it up:

- **Pack:** heap-box the struct (mirror the unconstrained
  `exists_payload_is_byval_aggregate` path) and store the pointer in `value`.
- **Dispatch:** emit a carrier-ABI adapter thunk per `(class, struct
  instance)` -- `int64_t __exwit_C_m_T(int64_t boxed) { return
  __inst_C_m_T(*(T *)(intptr_t)boxed); }` -- and point the packed witness
  table at the adapters so the carrier dispatch signature matches. Methods
  that *return* the class variable need the inverse re-box.
- **Open:** read the payload back through the boxed pointer and own it
  (single-use, matching the `:linear` discipline).

Until then, the workaround below is the supported shape (and the natural one
for a "heterogeneous handle collection").

## Workaround

Use a carrier-compatible payload -- a plain `:int`, a `:ptr<void>` handle, or
a `defopaque T :int` newtype -- instead of a bare `defstruct` value. For
`plot`'s `AnyRenderer`, that means each `*R` kind is a `defopaque ... :int`
handle over its heap-allocated payload, exactly as
`tests/fixtures/exists-open-witness-dispatch` demonstrates.
