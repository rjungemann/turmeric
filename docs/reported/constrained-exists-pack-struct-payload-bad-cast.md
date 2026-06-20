---
title: Constrained existential `pack` of a by-value struct payload emits an invalid `(int64_t)(struct)` cast
category: Bug Report
status: reported
component: compiler/emit (existentials)
affects: turmeric main 0.21.0
severity: medium
---

# Constrained `pack` of a struct payload does not heap-box -- emits `(int64_t)(struct)`

## Summary

`(pack (make-struct T ...) (exists [a] [(C a)] a))` -- a constraint-carrying
existential whose payload is a by-value struct -- emits C that casts the
struct directly to `int64_t` for the record's `value` field, which the C
compiler rejects. This is independent of `open`/dispatch: it reproduces with
no `open` at all.

## Repro (turmeric main 0.21.0)

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

`emit_value` for `EX_EXISTS_PACK` (`src/compiler/emit_expr.c`) handles a
by-value aggregate payload only on the *unconstrained* path (it heap-boxes the
aggregate and stores the pointer -- see `exists_payload_is_byval_aggregate`).
The constrained path (`n_witnesses > 0`) unconditionally stores
`%s->value = (int64_t)(%s)`, which is valid only for scalar/pointer payloads,
not for a `struct` rvalue.

## Fix directions

Mirror the unconstrained by-value-aggregate handling inside the
`n_witnesses > 0` branch: heap-box the aggregate, store the pointer in
`value`, and read it back through the pointer in `EX_EXISTS_OPEN`. Note that
witness-indirected `open`-site dispatch (now implemented, see
`docs/archive/existential-open-witness-dispatch.md`) lowers the receiver
through the int64 carrier; a by-value-struct receiver would additionally need
the dispatch site to reconstruct the struct (or pass the boxed pointer) to
match the instance method's ABI. The carrier-compatible payload path (plain
`:int`, `:ptr<void>`, opaque-over-int newtypes) already works end to end.

## Workaround

Use a carrier-compatible payload -- a plain `:int`, a `:ptr<void>` handle, or
a `defopaque T :int` newtype -- instead of a bare `defstruct` value. This is
also the natural shape for the "heterogeneous handle collection" use case.
