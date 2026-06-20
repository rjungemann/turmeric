---
title: Existential `open`-site witness dispatch on a by-value-struct receiver uses the wrong ABI
category: Bug Report
status: reported
component: compiler/emit (existentials, witness dispatch)
affects: turmeric main 0.21.0
severity: medium
---

# Witness dispatch on a boxed by-value-struct receiver reads garbage

## Summary

After the constrained-pack heap-box fix
(`docs/archive/constrained-exists-pack-struct-payload-bad-cast.md`), a
constraint-carrying existential whose payload is a by-value `defstruct`
*compiles and round-trips through pack/open*, but calling a witness method on
the opened value (`(rbound x)`) produces a wrong result. The pack stores a
box pointer in the existential record's int64 `value`; `EX_EXISTS_DISPATCH`
hands that pointer to the instance method as if it were the carrier value,
but the instance method's C signature takes the struct **by value**.

## Repro (turmeric main 0.21.0)

```turmeric
(defmodule p (export)
  (defclass Rdr [a] (rbound [x] : int))
  (defstruct LinesR [v : int w : int])
  (definstance Rdr [LinesR] (rbound [x] (+ (.v x) (.w x))))
  (defn main [] : int
    (let [e (pack (make-struct LinesR 5 7) (exists [a] [(Rdr a)] a))]
      (open e [a x]
        (println (rbound x))))   ; expected 12; prints a garbage pointer value
    0))
```

## Root cause

`EX_EXISTS_DISPATCH` (`src/compiler/emit_expr.c`) builds the witness-call
function-pointer type with the **carrier ABI**: every class-variable-typed
parameter erases to `int64_t`, so the receiver slot is `int64_t`. It emits

```c
((int64_t (*)(int64_t))(((void **)(__exrec_x->witnesses[0]))[0]))(x);
```

But the instance method generated for a monomorphic by-value struct takes the
struct by value (`type_uses_carrier_abi(LinesR)` is `false`):

```c
static int64_t __inst_Rdr_rbound_LinesR(LinesR x) { ... }
```

and the witness vtable stores that exact pointer. With the pack fix, `x` (the
open binding) is the int64 carrier holding the heap-boxed struct pointer, so
the call passes a pointer where the callee expects a `LinesR` by value -- the
callee reads `x.v`/`x.w` off the wrong bytes.

The open site cannot fix this on its own: the payload type is fully abstract
(`a`) there, so it has no way to know the receiver was boxed. The
normalization has to happen on the pack-time-known witness side.

## Fix directions

Make the witness present a carrier-ABI vtable for by-value-aggregate payloads.
At the constrained pack site (`EX_EXISTS_PACK`, when
`exists_payload_is_byval_aggregate(payload)` holds), point each witness at a
generated **carrier-thunk dict** instead of `&dict_<Class>_<T>_singleton`. Each
thunk has the carrier signature `RET (*)(<carrier-sig>)` the dispatch already
assumes; its body dereferences the boxed receiver
(`*(T *)(intptr_t)arg`) -- and any other class-variable-typed (hence boxed)
argument -- and forwards to the real `__inst_<Class>_<method>_<T>`, matching
that instance's actual by-value / by-pointer parameter ABI (mirror
`type_struct_pass_by_ptr` from the dict-struct emit in `emit_stmt.c`).
Emit the thunk dict once per `(instance, payload-type)` (dedup like
`ensure_typed_thunk_typedef`). The carrier-compatible payload path (`:int`,
`:ptr<void>`, opaque-over-int newtypes) is unaffected -- gate the thunk
strictly on the by-value-aggregate predicate.

## Workaround

Use a carrier-compatible payload -- a plain `:int`, a `:ptr<void>` handle, or
a `defopaque T :int` newtype -- when the existential needs witness dispatch.
