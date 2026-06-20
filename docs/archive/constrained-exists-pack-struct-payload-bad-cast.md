---
title: Constrained existential `pack` of a by-value struct payload emitted an invalid `(int64_t)(struct)` cast
category: Bug Report
status: resolved
component: compiler/emit (existentials)
affects: turmeric main 0.21.0
resolution: heap-box the by-value aggregate on the constrained pack path (read it back on open) AND route witness dispatch through a carrier-adapter thunk dict; see Resolution below
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

### Witness dispatch -- carrier-adapter thunk dict

Witness-indirected `open`-site dispatch on a by-value-struct receiver works end
to end. Existential dispatch (`EX_EXISTS_DISPATCH`, `emit_expr.c`) flows the
receiver through the int64 carrier and erases class-variable-typed params to
`int64_t`, calling the witness slot cast to `int64_t (*)(int64_t, ...)`. A
by-value struct instance method is emitted as
`int64_t __inst_Rdr_rbound_LinesR(LinesR)` -- a by-value-struct ABI that cannot
be called through that carrier signature. So when the payload is a by-value
aggregate, the constrained `pack` points each witness at a generated
carrier-adapter dict `dict_<Class>_<T>__exbox` instead of the real
`dict_<Class>_<T>` (`ensure_exists_byval_witness_dict`,
`src/compiler/emit_module.c`). Each adapter thunk:

- takes the receiver as the int64 carrier (the heap-box pointer),
  dereferencing it back to the concrete struct (`*(T *)(intptr_t)recv`, or a
  `(const T *)` cast when the instance method is pass-by-ptr);
- forwards concrete arguments unchanged (taking their address when the real fn
  wants `const U *`);
- calls the real `__inst_<Class>_<method>_<T>` and returns its result.

The thunks are emitted to `pending_handler_fns` so they land at file scope after
the `__inst_` forward declarations and before the function bodies that reference
`&dict_<Class>_<T>__exbox_singleton`. A method that returns the class variable
would need an inverse re-box; that case falls back to the real dict (no in-tree
class needs it). The carrier-compatible payload path (plain `:int`,
`:ptr<void>`, opaque-over-int newtypes) matches the carrier dispatch ABI
directly and uses the real dict unchanged.

The report's exact repro compiles and runs (exit 0), and `(rbound x)` /
`(rbox x)` dispatch correctly on the boxed receiver. Regression fixture:
`tests/fixtures/exists-pack-constrained-byval-struct/` (scalar- and
struct-returning methods, opened and dispatched).

(An earlier resolution on `main` -- #455 -- instead *rejected* the by-value
struct payload at `elab_pack` with a diagnostic, on the grounds that the
heap-box alone left this dispatch ABI trap. That guard was removed: the trap is
now closed by the adapter thunks above, so the construct is supported rather
than forbidden.)
