---
title: Constrained existential `pack` of a by-value struct payload emitted an invalid `(int64_t)(struct)` cast
category: Bug Report
status: resolved
component: compiler/emit (existentials)
affects: turmeric main 0.21.0
resolution: heap-box the by-value aggregate on the constrained pack path (and read it back through the pointer on open); see Resolution below
severity: medium
---

# Constrained `pack` of a struct payload does not heap-box -- emits `(int64_t)(struct)`

## Summary

`(pack (make-struct T ...) (exists [a] [(C a)] a))` -- a constraint-carrying
existential whose payload is a by-value struct -- emitted C that cast the
struct directly to `int64_t` for the record's `value` field, which the C
compiler rejects. This is independent of `open`/dispatch: it reproduces with
no `open` at all.

## How it surfaced

This was the Track C / `turmeric-spices` `plot` P3 finding: the
`plot-renderer-typeclass-plan` boxes mixed renderer structs (`PointsR`,
`LinesR`, `FunctionR`, ...) into a constrained existential `AnyRenderer` so a
`(vec-of (function ...) (points ...))` type-checks under one `Renderer`
dictionary. The investigation confirmed the report and pinned a *second*,
deeper layer beyond the cast (the dispatch ABI -- see Known follow-up).

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

```
error: aggregate value used where an integer was expected
  __t43->value = (int64_t)((LinesR){.v = INT64_C(5)});
```

## Root cause

`emit_value` for `EX_EXISTS_PACK` (`src/compiler/emit_expr.c`) handled a
by-value aggregate payload only on the *unconstrained* path (it heap-boxes the
aggregate and stores the pointer -- see `exists_payload_is_byval_aggregate`).
The constrained path (`n_witnesses > 0`) unconditionally stored
`%s->value = (int64_t)(%s)`, which is valid only for scalar/pointer payloads,
not for a `struct` rvalue.

## Resolution

Fixed by mirroring the unconstrained by-value-aggregate handling inside the
`n_witnesses > 0` branch of `EX_EXISTS_PACK` (`src/compiler/emit_expr.c`):

- **pack** -- when `exists_payload_is_byval_aggregate(payload)` holds, the
  constrained path now `malloc`s a copy of the aggregate and stores the box
  pointer (`(int64_t)(intptr_t)box`) in the record's `value` slot instead of
  the invalid `(int64_t)(struct)` cast.
- **drop** -- the rc-managed record uses a new drop hook
  `tur_existential_drop_byval` (`src/compiler/emit_module.c`) that `free`s the
  box when the record is reclaimed; the no-op default would leak it. The block
  stays tagged `RCEXP_OPAQUE` so the cycle walker never follows the plain
  (non-rc) box.
- **open** -- `EX_EXISTS_OPEN` reads the struct back through the
  record -> box indirection for both the rc-managed and `:linear` record
  paths; the `:linear` path frees the box before reclaiming the bare record.

The report's exact repro now compiles and runs (exit 0). Regression fixture:
`tests/fixtures/exists-pack-constrained-byval-struct/`.

(An earlier resolution on `main` -- #455 -- instead *rejected* the by-value
struct payload at `elab_pack` with a diagnostic, on the grounds that the
heap-box alone left the dispatch ABI trap below. That guard was removed in
favor of this end-to-end heap-box support; the dispatch ABI is tracked as the
follow-up below rather than forbidding the construct outright.)

## Known follow-up (separate report)

Witness-indirected `open`-site dispatch on a by-value-struct receiver is still
unsupported. Existential dispatch (`EX_EXISTS_DISPATCH`, `emit_expr.c`) flows
the receiver through the int64 carrier and erases class-variable-typed params
to `int64_t`, calling the witness slot cast to `int64_t (*)(int64_t, ...)`. But
a by-value struct instance method is emitted as
`int64_t __inst_Rdr_rbound_LinesR(LinesR)` -- a by-value-struct ABI that cannot
be called through the carrier signature -- so `(rbound x)` after opening a
boxed-struct existential reads garbage. The pack/open round-trip itself is
correct; only the witness call ABI needs reconciling (emit a carrier-ABI
adapter thunk per `(class, struct instance)` and point the packed witness table
at the adapters). Tracked in
`docs/reported/constrained-exists-open-dispatch-byval-struct-receiver.md`.

The carrier-compatible payload path (plain `:int`, `:ptr<void>`,
opaque-over-int newtypes) matches the carrier dispatch ABI and already works
end to end.

## Workaround (when dispatch is needed)

Use a carrier-compatible payload -- a plain `:int`, a `:ptr<void>` handle, or
a `defopaque T :int` newtype -- instead of a bare `defstruct` value when the
existential needs witness dispatch. This is also the natural shape for the
"heterogeneous handle collection" use case.
