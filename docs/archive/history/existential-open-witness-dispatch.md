---
title: Existential `open` dispatch is not witness-indirected
category: Bug Report
status: resolved
component: compiler/elaborator (typeclasses, existentials)
affects: turmeric main 0.21.0
resolves: turmeric-spices plot Renderer plan P3 (AnyRenderer box)
---

# Existential `open` dispatch now resolves through packed witnesses

## Summary

Method dispatch inside `open` over a constraint-carrying existential
(`(exists [a] [(C a)] a)`) did not resolve through the witnesses packed at
the `pack` site. With >=2 in-scope instances of the constraint class `C`, the
call failed as ambiguous because the receiver type is erased to `int64_t`.
This made heterogeneous existential collections undispatchable -- the very
use case existentials advertise.

**Resolved:** an `open`-bound receiver whose method belongs to one of the
existential's constraint classes now dispatches through the per-constraint
witness vtable bundled in the runtime existential record, independent of how
many instances of the class are in scope.

## Repro (resolved)

```turmeric
(defmodule p (export)
  (defclass Rdr [a] (rbound [x : a] : int))
  (defopaque LinesR  :int)
  (defopaque PointsR :int)
  (definstance Rdr [LinesR]  (rbound [x : LinesR]  (:: x :int)))
  (definstance Rdr [PointsR] (rbound [x : PointsR] (+ 100 (:: x :int))))
  (defn bound-of [which : int] : int
    (if (= which 0)
      (open (pack (:: 5 :LinesR)  (exists [a] [(Rdr a)] a)) [a v] (rbound v))
      (open (pack (:: 7 :PointsR) (exists [a] [(Rdr a)] a)) [a v] (rbound v))))
  (defn main [] : int
    (println (bound-of 0))   ;; 5
    (println (bound-of 1))   ;; 107
    0))
```

Before: `error: ambiguous method dispatch: '.rbound' matches 2 instances`.
After: prints `5` then `107` -- each arm consults its own packed witness.

Regression fixture: `tests/fixtures/exists-open-witness-dispatch/`.

## Root cause

`(open e [a v] ...)` binds `v` to the record's int64 `value` field, typed as
the erased int64 carrier (`elab_open`, `src/compiler/elab_types.c`). A
`(.m v)` call therefore reached `elab_method_call`
(`src/compiler/elab_typeclasses.c`) with an `int64_t` receiver and could only
fall back to a name-only static instance search -- trivially unambiguous with
one instance in scope, `TUR-E0020` ambiguous with two or more. Neither path
consulted the witness packed into the `tur_existential_t` record.

## Fix

- **`Binding.exists_open_type`** (`src/compiler/expr.h`): `elab_open` stamps
  the `v` binding with the scrutinee's `TY_EXISTS` type (carrying the
  constraint classes, in witness order) whenever the existential is
  constraint-carrying.
- **`EX_EXISTS_DISPATCH`** (new expr node): when the receiver of a method
  call is such a binding and the method belongs to one of the constraint
  classes, `elab_method_call` builds this node recording the witness slot
  (constraint index) and the method index within the class.
- **Emit** (`src/compiler/emit_expr.c`): `EX_EXISTS_OPEN` exposes the
  existential record pointer as `__exrec_<v>`; `EX_EXISTS_DISPATCH` lowers to
  an indirect call through `((void **)record->witnesses[slot])[method_idx]`
  using the carrier ABI (class-variable params -> `int64_t`, concrete params
  keep their type). The witnesses array is a flat array of method function
  pointers in class-declaration order, so the method pointer is the
  `method_idx`-th slot of the dict the witness points at.
- **Interpreter** (`src/turi/eval.c`): the existential record is erased under
  `--interpret`, so `EX_EXISTS_DISPATCH` re-dispatches structurally on the
  receiver's runtime type. This is exact for struct/ADT payloads; opaque
  newtypes over a primitive carrier cannot be distinguished there, so the
  heterogeneous-opaque fixture is `requires.compiled`.

## Verification

- `tests/fixtures/exists-open-witness-dispatch/` -- heterogeneous LinesR /
  PointsR collection, inline + through a function parameter; prints
  `5 / 107 / 3 / 109`.
- `bash tests/run.sh`: 1698 passed, 0 failed.

## Known adjacent gap (filed separately)

The *literal* repro in the original report packed a struct payload
(`(make-struct LinesR 5)`). Constrained existential packs of a by-value
struct payload fail to compile independently of dispatch (the pack stores
`(int64_t)(struct)`), so the report's struct repro was reproduced here with
carrier-compatible opaque newtypes. The struct-payload pack defect is tracked
in `docs/reported/constrained-exists-pack-struct-payload-bad-cast.md`.

## Downstream

Unblocks turmeric-spices **plot Renderer plan P3** (`AnyRenderer` box: a
collection of mixed renderer handle kinds dispatching `bounds` / `render`
through the packed `Renderer` dictionary).
