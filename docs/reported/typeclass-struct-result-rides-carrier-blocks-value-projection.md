---
title: A typeclass method's struct/ADT result rides the int64 carrier at call sites, blocking value-level projection through it
category: Typeclass dispatch / value-level ABI -- expressiveness hole (not a silent miscompile)
severity: Medium. A typeclass method declared to return a by-value struct/ADT
  (directly, via a class type variable bound to a struct, or via an associated
  type) loses its precise type at the call site: the result is typed by bare
  TypeKind (no struct def), so `(.field (method ...))` fails with "no typeclass
  method found for '<field>'", and a let-bound `p : T` mixes the carrier int64
  with the by-value `T` (a hard cc error, not a silent miscompile). Argument
  position is *by-value*, result position is *carrier* -- the asymmetry is what
  blocks round-tripping a struct element through a dispatched get/insert pair.
status: OPEN. Discovered while landing ECS E2d-P6 (associated-type method
  returns + multi-param storage dispatch). The three reported P6 type-checker
  gaps are fixed; this is the next layer (value-level projection) which the
  assoc-types-plan explicitly scoped OUT, surfaced concretely here.
---

# Typeclass struct/ADT results ride the int64 carrier at call sites

## One-line summary

`elab_method_call` types a dispatched method's result with
`type_from_kind(result_kind)` -- a bare `TypeKind` with no struct/ADT def -- so
a method returning a by-value struct yields a def-less result at the call site,
and `(.field (method ...))` / `(let [p : T (method ...)] ...)` break.

## Minimal repro

```turmeric
(extern-c printf [^cstr fmt ^int v] : int)
(defstruct Pos [x : int])
(defclass Mk [A] (mk [^borrow s : A idx : int] : A))
(definstance Mk [Pos]
  (mk [s idx] (make-struct Pos idx)))
(defn main [] : int
  (let [base : Pos (make-struct Pos 1)
        p    : Pos (mk base 7)]
    (printf "x=%lld\n" (.x p))     ; <-- here
    0))
```

```
$ ./build/tur check /tmp/mk.tur
mk.tur:9:24: error: no typeclass method found for 'x'
```

This is **not** specific to associated types or multi-param classes -- it is a
single-parameter class `Mk [A]` whose `mk` returns the class variable `A` bound
to a struct. The same shape underlies the ECS get/insert accessors: an
accessor declared `: Elem` (associated type) or `: E` (multi-param class var)
that resolves to `Pos` cannot have its struct result consumed at the call site.

## Observed vs expected

- **Observed.** The result of `(mk base 7)` is typed `TY_STRUCT` with a NULL
  `def` (the bare-kind lowering), so the field-access fast path in
  `elab_method_call` bails and reports `no typeclass method found for 'x'`. When
  the result is let-bound to `p : Pos`, codegen emits a `Pos` initialised from an
  `int64_t` (or vice-versa) -> `incompatible types` cc error.
- **Expected.** `(mk base 7) : Pos` (the instance's concrete return), so
  `(.x ...)` resolves and `p : Pos` lowers consistently.

## Root cause (file:line)

`src/compiler/elab_typeclasses.c`, `elab_method_call`, result-type computation:

```c
result_type = method_callable_result_type(
    best_method->binding,
    type_from_kind(best_method->binding->type.as.fn.result_kind));
```

`type_from_kind(result_kind)` keeps only the `TypeKind`; the struct/ADT def is
dropped. `method_callable_result_type` recovers a precise type *only* for a
**boxed TY_FN** return (the arrow-closure case). Struct/ADT/TY_APP returns fall
through to the def-less carrier.

The impl binding only stores a precise `result_full_type` for `TY_FN` returns
(same function, where the impl fn type is built):

```c
if (return_type.kind == TY_FN) { ... fn_type.as.fn.result_full_type = rft; }
```

So even though the instance method's `return_type` is the concrete `Pos` (the
class-var / associated-type substitution computes it correctly), that precise
type is never threaded to the call site.

The asymmetry: a non-parametric struct **parameter** of a typeclass method is
kept *by-value* (the legacy receiver-substitution path leaves
`param_type = elab_param_type` for a non-parametric struct), while the
**result** is erased to the carrier. Round-tripping a struct element through a
dispatched `get` (carrier result) into a dispatched `insert` (by-value struct
param) therefore cannot type/lower consistently.

## Why the obvious fix is not a one-liner

Threading `result_full_type` for struct/ADT/TY_APP returns *and* consuming it at
the call site (attempted during the P6 work) flips the impl's emitted return ABI
from the documented int64 carrier to a by-value struct. That breaks every
instance whose body is an inline-C `return 0;`/carrier shim against a class-var
return (a widely-used convention), and more broadly changes the dictionary-slot
ABI. A real fix has to either:

1. Make the **whole** value-level path consistent -- struct results *and*
   struct params both by-value (or both carrier) across the dict ABI,
   reinterpreting at the boundary -- or
2. Keep the carrier ABI but attach the precise nominal type as *metadata* on the
   call result (so `.field` resolution and let-binding see `Pos` while codegen
   still marshals the int64 carrier with an explicit reinterpret).

Option 2 is the smaller, lower-risk change and matches how the elaborator
already reinterprets carriers elsewhere; it deserves its own focused milestone.

## Scope / relation to ECS E2d-P6

The assoc-types-plan
(`docs/archive/typeclass-associated-types-missing.md`) explicitly recorded that
"projecting through an associated type at the *value* level (a method whose C
signature depends on `Elem`) ... is not in scope; the value-level storage handle
still rides the int64 carrier like any other opaque." This report is the
concrete failure mode behind that note, generalised to *any* struct-returning
typeclass method (not only associated types).

The P6 type-checker gaps that ARE fixed (associated-type method returns,
multi-param dispatch from the storage param, projection-reduction /
opaque-applied-head discrimination during dispatch) unblock the
`StorageOps` *dispatch*; this report tracks the remaining value-level ABI so a
later milestone can let `get` return a real struct the caller can field-access.

## How to validate a fix

- The repro above compiles and prints `x=7`.
- `(.field (method ...))` resolves for a struct-returning typeclass method.
- A get/insert round-trip -- `(sop-insert! s i (sop-get s j))` -- type-checks
  and lowers without a carrier/by-value mismatch.
- Full suite stays green; add a fixture pinning a struct-returning method whose
  result is field-accessed and let-bound.

## Related

- `docs/archive/typeclass-associated-types-missing.md` (value-level projection
  explicitly out of scope).
- `tests/fixtures/typeclass-assoc-type-method-return/`,
  `tests/fixtures/typeclass-multiparam-storage-dispatch/`,
  `tests/fixtures/typeclass-typed-method-param/` (the P6 dispatch fixtures; they
  deliberately avoid the struct round-trip this report covers).
