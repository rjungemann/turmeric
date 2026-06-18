---
title: `(:: x :A)` ascription value-casts (not bit-reinterprets) when the tyvar A monomorphizes to float, miscompiling NonEmpty/float round-trips
category: Compiler / Codegen -- polymorphic int64 carrier <-> float
severity: Medium -- silent miscompile. A polymorphic value that round-trips
  through the int64 carrier via `(:: x :A)` ascriptions (store as carrier with
  `(:: x :int)`, read back with `(:: carrier :A)`) is corrupted whenever the
  type parameter A resolves to `:float` at the monomorphized call. The store
  side truncates the double to an integer (`1.5` -> `1`); the read side
  value-casts the int64 carrier to a double (`bits(1.5)` -> `4.6e18`). Integer
  element types are unaffected (identity cast), so the bug hides until a float
  element is used.
status: OPEN, pre-existing (independent of the ne-from? by-value retype). Repro
  uses only the long-standing `ne-of` / `ne-singleton` / `ne-head` surface in
  `stdlib/refined.tur`, none of which the retype touched. Surfaced while landing
  the typed-list `ne-from?` retype (Phase 1.1 of
  `docs/upcoming/end-to-end-monomorphization-plan.md`): that plan's validation
  asks for a `(ne-from? (list-of 1.5 2.5))` fixture proving `A = float` threads
  through `ne-head` without truncation. The int half of the retype landed and is
  clean; the float fixture is blocked here.
---

# `(:: x :A)` value-casts instead of bit-reinterpreting for float A

## Repro

```turmeric
(load "stdlib/refined.tur")
(defn main [] : int
  (println (ne-head (ne-singleton 1.5)))   ; expected 1.5, got 1
  0)
```

```turmeric
(load "stdlib/refined.tur")
(defn main [] : int
  (let [o (ne-from? (list-of 1.5 2.5))]
    (if (some? o) (println (ne-head (ne-unwrap o))) (println -1.0)))  ; expected 1.5, got 4.60943e+18
  0)
```

Both print garbage; the integer analogues (`(ne-head (ne-singleton 42))`,
`(ne-from? (list-of 7))`) are correct.

## Root cause

`NonEmpty A` stores its elements in an int64 cons-list carrier. The accessors
bit-cast through `:int`:

- `ne-of`  : `(:: (tcons (:: x :int) xs) :NonEmpty)` -- store: `(:: x :int)`
  where `x : A`.
- `ne-head`: `(:: (list-head (:: ne :int)) :A)` -- read: `(:: <int64> :A)`.

At **elaboration** (`elab_ascribe`, `src/compiler/elab_types.c:2407`) an
`EX_REINTERPRET` is only inserted when the source and target are *concrete*
same-size scalar kinds. When one side is a bare type variable `A`, its scalar
kind is unknown, so the node degrades to a type-erased `EX_ASCRIBE`.

At **monomorphization** (`emit_value` `case EX_ASCRIBE`,
`src/compiler/emit_expr.c:4855`) the ascription is treated as a pure relabel for
a TY_TYVAR / def-less-carrier target (the `ascribe_to_opaque` path). The int64
carrier value then flows into a `double` C slot (or vice versa) and C applies a
*value* conversion:

- store `(:: x:float :int)` -> `(int64_t)1.5` == `1` (truncation),
- read `(:: <int64> :A=float)` -> `(double)bits(1.5)` == `4.6e18` (value cast,
  not bit reinterpret).

The fix needs the EX_ASCRIBE lowering to consult the *resolved* (post-spec)
kinds of both the inner expression and the ascribed type, and emit a union
bit-reinterpret (mirroring `EX_REINTERPRET`'s same-size path and
`EX_UNION_INJECT`'s float path at `emit_expr.c:1481`) when exactly one side
resolves to an 8-byte float and the other to the 8-byte int carrier. A first
attempt that keyed off `emit_resolve_type(ctx, inner->type)` / `e->type` did
**not** fire: at this site the polymorphic element is represented as a
def-less carrier (not a clean `TY_FLOAT`/`TY_TYVAR`), so `emit_resolve_type`
does not surface the `float` kind. Pinpointing where the concrete `A = float`
binding is reachable from the EX_ASCRIBE node (likely
`ctx->current_abi_specialization`, cross-referenced against the function's
type-param list) is the open work.

## Proposed fix directions

1. **Emit-side (preferred):** in `emit_value`'s `EX_ASCRIBE` case, resolve the
   inner and target kinds against the active monomorphization spec (not just
   `emit_resolve_type`). When one is an 8-byte float and the other the 8-byte
   int carrier, emit `((union { double d; int64_t i; }){.d = (X)}).i` (store)
   or `((union { int64_t i; double d; }){.i = (X)}).d` (read).
2. **Elab-side:** when ascribing to/from a tyvar that is constrained or later
   bound, defer the reinterpret decision to a monomorphization-time rewrite
   rather than erasing to EX_ASCRIBE.

The cons-list collector half of this round-trip was a *separate* truncation
bug and is already fixed (see Related): `EX_CONS_LIST` now stores a float head's
bit pattern via a union reinterpret instead of `(int64_t)` truncation, so
`(:: (list-head (list->carrier (list-of 1.5))) :float)` (a *concrete*
read-back) is correct. Only the *polymorphic* `(:: <carrier> :A)` read-back
inside `ne-head` remains broken.

## Validation for a fix

- `(ne-head (ne-singleton 7.1))` prints `7.1`.
- `(ne-head (ne-unwrap (ne-from? (list-of 1.5 2.5))))` prints `1.5`.
- A new fixture `tests/fixtures/refined-nonempty-typed-list/` exercising the
  float case (currently withheld because it would fail) is added and passes.
- Integer NonEmpty fixtures (`tests/fixtures/refined-nonempty/`) stay green.
- Full suite green; no other ascription codegen regresses (the rewrite must
  fire only for the float<->int8byte carrier mismatch, never for same-kind or
  pointer/struct relabels).

## Related

- `docs/reported/ne-from-byvalue-option-nonempty-element-type-uninferable.md`
  -- the typed-list retype whose float-validation fixture this blocks.
- `docs/upcoming/end-to-end-monomorphization-plan.md` Phase 1.1.
- `src/compiler/emit_expr.c` `EX_CONS_LIST` (the already-fixed collector half)
  and `EX_ASCRIBE` (the remaining read-back half).
- `src/compiler/elab_types.c:2407` (`elab_ascribe`, where the reinterpret is
  skipped for tyvar targets).
