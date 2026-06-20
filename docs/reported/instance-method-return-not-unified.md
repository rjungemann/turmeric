---
title: Result-position return type is not unified with the function body (language-wide, not instance-specific)
category: Type checking -- return-position unification (carrier-ABI consequence)
severity: Medium. A function body may produce a value of a completely unrelated
  type to its declared return -- an int where `: cstr` is declared, an int where
  `: float` is declared, a distinct nominal struct, even a `float` where a
  by-value struct is declared -- and `tur check` / `tur build` accept it with no
  diagnostic. Originally probed as a `definstance` method gap (a handler whose
  `handle` is declared `: Response` can return a non-Response); on
  investigation the same hole exists for ordinary `defn`s, so the root cause is
  the int64 carrier ABI, not instance-method elaboration.
status: PARTIALLY RESOLVED
---

## Status: reverse cstr/integer returns now rejected for committed defns (2026-06-20)

The carrier-aware return unification work (see
`docs/upcoming/v2/carrier-aware-return-unification-plan.md`, Phase 2) now closes
the REVERSE of the `TUR-E0708` commit direction for the one position where it is
soundly rejectable: a genuinely **committed** `defn` -- monomorphic
(`n_fn_type_params == 0`, implicit type params included) and not `#{Unsafe}`, so
it does not participate in the int64 carrier ABI -- that declares a concrete
integer-family return but whose body yields a concrete `cstr` is now a hard
`TUR-E0709`. A string pointer is never a valid integer, and a committed
monomorphic function has no carrier to bridge it.

- The classification is the mechanism: `elab_defn` tags the position
  `RET_CLASS_COMMITTED` only for a monomorphic non-`#{Unsafe}` defn;
  generic / `#{Unsafe}` defns are `RET_CLASS_CARRIER_FN` and instance methods
  `RET_CLASS_CARRIER_METHOD`, both of which still tolerate the reverse
  direction (the deliberate carrier-handle bridge). The shared dispatcher
  `return_position_conflict` (`src/compiler/elab_core.c`) gates the reverse
  pointer-scalar check (`return_type_pointer_scalar_reverse_conflict`) on
  `RET_CLASS_COMMITTED`.
- New code `TUR_E0709_RETURN_TYPE_MISMATCH` (`src/compiler/diag.{h,c}`) with a
  code string, reverse lookup, and a `tur explain TUR-E0709` long-form entry.
- Fixtures: `errors/return-type-int-cstr-defn` (negative) and
  `return-type-int-cstr-carrier-ok` (positive control: a generic defn and an
  `#{Unsafe}` defn returning a `cstr` under an `int` return are NOT flagged).
  `bash tests/run.sh`: 1706 passed, 0 failed.

The remaining residue is now just the `int`-vs-`bool` same-integer-class pair
(both directions) and the carrier-handle bridge for generic / `#{Unsafe}` /
typeclass code. Rejecting `int`-vs-`bool` soundly needs a carrier-result marker
(a `TY_INT` body may be a genuine int or a carried `bool`), so it is deferred to
a Phase 2b slice; this report stays OPEN for it.

## Status: cstr-commit / integer-body returns now rejected (2026-06-20)

The next carrier-tolerated slice past the nominal and float guards is now fixed
for **both** the `defn` and `definstance` paths: a function (or instance method)
that COMMITS to a `: cstr` return but whose body yields a concrete integer-family
scalar (`int` / `bool` / `intN` / `uintN`) is now a hard `TUR-E0708`. `cstr` (a
`const char*`) and the integer family all ride the same int64 GP register, so
this slips past the `TUR-E0707` register-class check -- but a bare integer is
never a valid string pointer, so committing to `cstr` and handing back an integer
is a genuine type-erasure bug, not a tolerable carrier bridge.

- Sibling helper `return_type_pointer_scalar_conflict` (`src/compiler/elab_core.c`,
  declared in `elab_internal.h`): true iff the declared return is concretely
  `cstr` and the body's kind is an integer-family scalar
  (`ps_is_integer_scalar_kind`). Only this **commit direction** is flagged.
- The reverse (a declared integer *carrier* whose body yields a `cstr` handle) is
  the deliberate int64 carrier-handle bridge -- generic / typeclass code
  routinely returns a pointer handle through an int64 result slot, exactly as it
  returns a struct handle through `int` -- so it stays accepted and is left to a
  future carrier-aware unification, like the same-register `int`-vs-`bool` case.
  The commit-direction gate inside the helper encodes this calibration directly,
  so no float-style path asymmetry was needed: both `defn` and `definstance` call
  the same helper.
- `defn`: checked in `elab_defn` (`src/compiler/elab_fns.c`), after the float
  register-class check, skipping the lazy-probe placeholder and inline-C bodies.
- `definstance`: checked in pass 2 (`src/compiler/elab_typeclasses.c`) against
  `InstMethodPass.ret_kind`, after the float register-class check; inline-C
  bodies (fiat TY_NIL) skipped.
- New code `TUR_E0708_RETURN_POINTER_SCALAR_MISMATCH` (`src/compiler/diag.{h,c}`)
  with code string, reverse lookup, and a `tur explain TUR-E0708` long-form entry.
- Fixtures: `errors/return-type-pointer-scalar-defn`,
  `errors/return-type-pointer-scalar-instance` (negatives), and
  `return-type-pointer-scalar-ok` (positive control: a defn and an instance
  method that commit to `: cstr` and deliver genuine strings are NOT flagged).
  `bash tests/run.sh`: 1706 passed, 0 failed.

## Status: float-vs-non-float register-class returns now rejected (2026-06-20)

The float register-class slice flagged as a candidate follow-up below is now
fixed for **both** the `defn` and `definstance` paths. A float
(`float`/`float32`/`float64`) lives in an xmm register while `int` / `cstr` /
`bool` / opaque / struct / ADT handles all ride the int64 GP register, so a
float-vs-non-float result is a genuine xmm0-vs-rax miscompile (the same hazard
the `TUR-E0705` poly-closure guard rejects), not a soundly-tolerable carrier
bridge. It is now a hard `TUR-E0707`.

- Sibling helper `return_type_register_class_conflict` (`src/compiler/elab_core.c`,
  declared in `elab_internal.h`): true iff exactly one side is a floating kind
  and the other is a concrete, register-pinned non-float; tyvar / unknown /
  never / any sides are tolerated.
- The int-literal -> float coercion is exempted and widened in place by
  `rc_widen_int_literal_to_float_return` (an `EX_INT_LIT` body whose declared
  return is float becomes an `EX_FLOAT_LIT`, so codegen emits `42.0`), mirroring
  numeric-literal coercion in argument/binding positions.
- `defn`: checked in `elab_defn` (`src/compiler/elab_fns.c`), after the nominal
  check, skipping the lazy-probe placeholder and inline-C bodies (fiat TY_NIL).
- `definstance`: checked in pass 2 (`src/compiler/elab_typeclasses.c`) against a
  new `InstMethodPass.ret_kind`. Calibrated for the typeclass int64-carrier ABI:
  a non-float-declared method with a float instance body (e.g. `Num`'s
  `(add [x y] : int)` for a float32 instance) is the deliberate carrier bridge
  and stays accepted; only a method that genuinely COMMITS to a float return and
  whose body yields a concrete non-float is rejected.
- New code `TUR_E0707_RETURN_REGISTER_CLASS_MISMATCH` (`src/compiler/diag.{h,c}`)
  with code string, reverse lookup, and a `tur explain TUR-E0707` long-form entry.
- Fixtures: `errors/return-type-register-class-int-float`,
  `errors/return-type-register-class-float-int`,
  `errors/return-type-register-class-float-struct`,
  `errors/return-type-register-class-instance-float-struct` (negatives), and
  `return-type-register-class-ok` (positive control locking in the
  int-literal -> float widening for both a defn and an instance method).
  `bash tests/run.sh`: 1702 passed, 0 failed.

## Status: nominal-identity conflicts now rejected (2026-06-20)

The carrier-safe slice of this gap is fixed for **both** the ordinary `defn`
path and the `definstance` method path: a body whose actual type is a
**different concrete nominal type** (struct / opaque newtype / ADT, compared by
def-pointer identity) than the declared return is now a hard `TUR-E0001`. Two
distinct nominal defs can never share a carrier representation, so this is
always a real error -- it is the "genuine ground mismatch" the fix direction
called for, with zero suite regressions.

- Shared helper `return_type_nominal_conflict` (`src/compiler/elab_core.c`,
  declared in `elab_internal.h`): true iff the declared return is a concrete
  nominal def and the body yields a *different* concrete nominal. A
  primitive / opaque-int carrier / tyvar / applied / unknown / inline-C body is
  tolerated.
- `defn`: checked in `elab_defn` (`src/compiler/elab_fns.c`) after the body is
  finalized (skips the lazy-probe placeholder).
- `definstance`: the declared nominal def (post Phase RT substitution) is
  stashed on `InstMethodPass` in pass 1 and checked against the elaborated body
  in pass 2 (`src/compiler/elab_typeclasses.c`), after the arrow-head TY_FN
  refinement.
- Fixtures: `errors/return-type-nominal-conflict-defn`,
  `errors/instance-method-return-nominal-conflict` (negatives), and
  `return-type-nominal-ok` (positive control that also locks in the tolerated
  carrier boundary -- a bare int returned where an opaque newtype is declared).
  `bash tests/run.sh`: 1697 passed, 0 failed.

### What remains OPEN (tolerated by carrier-ABI design)

The remaining width-compatible scalar/handle mismatches are still accepted,
because the int64 carrier ABI deliberately unifies their representation:

- `: bool` body `42`, and the reverse `: int` body `(some-bool)` -- `int` and
  `bool` are both integer-family scalars that genuinely share the int64 0/1
  representation, so neither direction can be soundly rejected without a
  carrier-aware unification.
- A declared integer *carrier* whose body yields a `cstr` / opaque / struct /
  ADT handle -- the carrier-handle bridge (`#{Unsafe}` inline-C and generic
  carrier code legitimately return an int64 handle for a wider type). This is
  the tolerated reverse of the `TUR-E0708` commit direction.

(Two earlier register/identity slices are now **resolved**: float register-class
divergence -- `: float` body returning a non-float, or a non-float return with a
float body -- is `TUR-E0707`, and the cstr-commit / integer-body direction --
`: cstr` body `42` / `(+ x 1)` -- is `TUR-E0708`; see the status sections at the
top. The distinct-nominal-identity clash is `TUR-E0001`. Only the same-integer-
class `int`-vs-`bool` residue and the carrier-handle bridge above remain open.)

These are genuinely a carrier-ABI consequence, not an elaboration bug: rejecting
them at the type level would fight the representation bridges the rest of the
language relies on. A complete fix is a carrier-aware return unification (fix
directions below) and remains future work; this report stays OPEN for that
residue.

---

# Result-position return type is not unified with the body

## One-line summary

Turmeric performs **no result-position type unification**: a function's body
type is never checked against its declared return type. This was first noticed
on `definstance` methods (the original framing below), but it is **not
instance-specific** -- ordinary `defn`s accept the same mismatches. The cause
is the int64 carrier ABI, under which int / cstr / bool / opaque handles /
struct handles all share one register-width representation, so the elaborator's
result position has nothing it is willing to reject.

## Corrected scope (verified on this tree, tur 0.21.0)

Both the ordinary `defn` path and the `definstance` method path accept every
one of these return mismatches (`tur check` exits 0, no diagnostic):

| Declared return | Body value | `defn` | instance method |
|---|---|---|---|
| `: cstr`  | `42` (int)            | accepted | accepted |
| `: float` | `42` (int)            | accepted | accepted |
| `: int`   | `"hello"` (cstr)      | accepted | -- |
| `: bool`  | `42` (int)            | accepted | -- |
| `: Other` (struct) | `(make-struct Pt ...)` (distinct struct) | accepted | accepted |
| `: Pt` (by-value struct) | `7.1` (float) | accepted | accepted |

Minimal `defn` repro (no typeclasses needed):

```turmeric
(defn f [x : int] : cstr 42)        ;; declared cstr, returns int
(defn main [] : int 0)
;; => tur check exits 0
```

So the original "instance methods skip the check that ordinary defns perform"
contrast does **not** hold: ordinary defns do not perform it either. The
`e->expected_type` channel that `elab_fns.c` pushes around the body
(`elab_fns.c:2380-2485`) shapes struct/ADT/fn *coercions*; it does not reject a
scalar or nominal result mismatch.

## What IS still true (the elaboration vs unification split)

The body is genuinely elaborated in both paths -- an *unknown call* inside an
instance method body is still rejected:

```turmeric
(definstance Encoder [HandleX]
  (encode [w] (totally-unknown-fn w)))
;; => error: unknown function or operator 'totally-unknown-fn'   (exit 1)
```

So the missing piece is precisely a body-result-vs-declared-return
*unification*, and it is missing everywhere, not just for instances.

## Where the (absent) check would live

Instance methods (`src/compiler/elab_typeclasses.c`, `elab_definstance`):

- `:3415` -- `Type fn_type = type_fn(param_kinds, n_method_params, return_type.kind);`
  keeps only the return *kind*.
- `:3510` -- `method_fd->return_type = type_simple(TY_UNKNOWN, CK_COPY);`
  drops the declared return Type from the FnDef.
- `:3610-3635` -- pass 2 elaborates and stores the body with no
  `type_unify(method_body->type, <declared return>)`.

Ordinary defns (`src/compiler/elab_fns.c`):

- `:2380-2485` -- `e->expected_type` is set to the declared return before the
  body is elaborated, but it drives coercion shaping, not result rejection;
  `:2525` / `:3773` retype bare-fat tails to the return kind rather than
  diagnosing a mismatch.

## Why it is this way (carrier ABI)

Typeclass dispatch and the generic ABI funnel results through an int64 carrier
and reinterpret at the boundary (see
`docs/archive/instance-method-return-carrier-bridge.md`). With int / cstr /
bool / opaque / struct-handle all int64-wide, a kind-level result check would
reject nothing useful, and a stricter identity-level check would fight the
carrier (and the many `result_full_type`/by-value bridges that deliberately let
representation differ from the surface type). A correct fix is a real
type-system feature -- result-position unification that understands the carrier
-- applied **consistently to both paths**, with a fixture regen pass. It is
scoped here as a follow-up, not attempted inline.

## Fix directions (follow-up, both paths)

1. Thread the (tyvar-substituted) declared return Type to the point where the
   body type is known -- on the FnDef for ordinary defns, and into pass 2 for
   instance methods (retain it instead of overwriting with `TY_UNKNOWN` at
   `elab_typeclasses.c:3510`).
2. Add a return-position unification that compares semantic types while
   tolerating the documented carrier/by-value representation bridges (TY_FN
   arrow heads, `result_full_type` carriers, `#{Construct}` by-value tails),
   emitting a `TUR-E*` only on a genuine ground mismatch (e.g. `cstr` vs `int`,
   distinct nominal structs, float vs aggregate).
3. Do it for **both** `defn` and `definstance` together so instance methods do
   not become stricter than ordinary functions, and regenerate fixture
   snapshots in the same change.

## Notes / scope

- Verified against this checkout's `./build/tur` (0.21.0); all repros are
  turmeric-side and self-contained.
- Distinct from `docs/archive/instance-method-return-carrier-bridge.md`, which
  fixed a codegen deref for by-value struct *returns* and notes the elaborator
  already worked for ascribed dispatch. The gap here is the absence of a
  result-position unification at elaboration time -- and, per the corrected
  scope above, it is language-wide.
