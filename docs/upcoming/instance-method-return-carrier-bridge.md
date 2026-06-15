---
title: Instance-method return carrier bridge -- unblock by-value Result/Option payloads from typeclass instance bodies
category: Planning -- ABI / Codegen rework (M3 → M4 interim)
description: Typeclass instance methods whose declared return type is a polymorphic by-value struct (Result, Option, ...) still emit `tur_ok` / `tur_err` from inline-C bodies. Their dispatch shim returns int64 carrier, but call sites ascribed to the declared by-value type fail to compile when passed through typed helpers. This plan defines the discrete interim work: a one-pass shim that reinterprets the carrier payload as the by-value struct at the dispatch boundary, until M4 (per-method instance ABI) lands the structural fix.
---

# Instance-method return carrier bridge

## TL;DR

Right now, you cannot write a typeclass instance method whose declared
return type is a polymorphic by-value struct (`(Result A B)`,
`(Option A)`, ...) and call it through an ascribed call site that then
passes the value by value to a typed helper. The body returns int64
carrier; the call site's static type is the by-value struct; clang
rejects the mismatch.

This is one discrete piece of work. It is **not** M2 (M2 shipped --
`#{Construct}` + `make-struct` are live in `stdlib/result.tur`
`stdlib/option.tur`). It is **not** the full M4 (per-method instance
ABI in `docs/upcoming/m4-typeclass-per-method-abi-plan.md`). It is the
**interim bridge** that sits between M3 and M4: reuse the existing
`emit_carrier_bridge` / by-value-twin machinery (landed in PRs #364 and
#369) to reinterpret the dispatch shim's `int64_t` return as the
declared by-value struct type on emit. Spice-side bool / cstr
decoders compile; M4 can land later without rolling spices back.

## The exact bug it fixes

### Minimal repro (already in-tree, currently red)

`../turmeric-spices/spices/json/tests/decode-bool.tur` exercises this
exact seam today. `Decode [bool]` ships in `spices/json/src/json/encode.tur:278`,
so the instance is no longer missing — the remaining failure is purely the
return-carrier seam:

```turmeric
(defn show [b : bool] : void
  (if b (println "1") (println "0")))

;; in main:
(show (ok-val (:: (decode doc a) (Result bool cstr))))
```

Observed at 2026-06-15 against `./build/tur`:

```
tests/decode-bool.tur:32:13: error [TUR-E0001]:
  function 'show' arg 1: expected bool, got int
```

The error fires at the **elaborator**, not at clang -- `ok-val r` resolves
its declared `:A` return against the carrier-typed value the dispatch shim
yields (an `int64_t` carrier) rather than the by-value struct's payload
type. The elaborator sees `int`, the call site declares `bool`, type-check
rejects it.

**Why this matters for the plan's shape.** The plan was originally framed
around a C-level mismatch; the in-tree failure is one elaboration step
earlier. The fix still routes through `emit_carrier_bridge`, but the
predicate has to participate in **typing** (so `ok-val r` resolves to the
declared `A`, not the carrier `int`), not just in emission. Concretely: the
dispatch-call type returned to the elaborator must be the substituted
by-value type, and the emit-side bridge then has to honor that type when
lowering to C. Body-side carrier emission (`emit_fns.c:493-512`) is
already correct -- this is purely a consumer-side fix.

A separate, deeper sub-bug surfaces in
`../turmeric-spices/spices/json/tests/derive-decode-struct.tur` (clang
error in `tests_derive-decode-struct_tur.c`: `Result__User__cstr` where
arithmetic/pointer required). The emitted C wraps an already-by-value
constructor return in a redundant `*(Result__User__cstr *)(intptr_t)`
deref. This is the SAME root cause -- a missing or wrongly-directed
bridge at an ascribed `(Result T cstr)` site -- but the wrong direction
(consumer side did NOT need the deref; producer side already by-value).
The predicate the plan adds must also AVOID inserting a bridge when the
producer already returns by-value. Treat this as a second acceptance
fixture, not a separate work item.

### Where the seam lives

| Layer | What it does | Status |
|---|---|---|
| Polymorphic stdlib constructors (`ok`, `err`, `some`, `none`) | `#{Construct}` + `make-struct` -- by-value codegen | **Done (M2).** See `stdlib/result.tur:36-66`. |
| Instance-body carrier-return signature (`emit_fns.c:493-512`) | `__inst_*` whose declared return uses carrier-ABI emits `int64_t` return type | **Live.** Body-side already shaped correctly. |
| Typeclass dispatch dict slot type (`emit_stmt.c:518`, `emit_module.c:2377`) | Uniform `int64_t (*)(...)` slot per method | **Open (M4).** Tracked in `docs/upcoming/m4-typeclass-per-method-abi-plan.md`. |
| Carrier-bridge / by-value twin (`emit_carrier_bridge` in `emit_core.c:2410`; predicate `type_uses_carrier_in_dispatch` in `emit_expr.c:17`) | Reinterpret int64 carrier as by-value struct at known boundaries | **Live.** Already used at five sites in `emit_expr.c` (lines 1809, 2640, 2704, 4458, 4484). |
| Existing fn-typed return carrier helper (`emit_inst_fn_return_carrier`, `emit_internal.h:317`) | Returns the carrier C-type for an `__inst_*` method whose return is a fn value | **Live.** Direct precedent for the new value-side helper this plan adds. |
| **This plan** -- extend the consumer-side bridge to typeclass instance method returns | Resolve the dispatch call's elaborated type to the substituted by-value struct AND insert `emit_carrier_bridge` at the call-result lowering | **Open. Subject of this doc.** |

The fourth and fifth rows are what we reuse; the bottom row is what we add. Note the file rename: the original draft cited `emit_typeclasses.c` -- no such file exists in this tree.

## Why the bridge (and not "just do M4")

- M4 is a multi-session restructuring of dict generation and call-site
  dispatch. It is the right end state. It is not the right thing to do
  *this week* if a spice surface is blocked on the bug.
- The bridge is one emit-time predicate (`instance method's declared
  return type is by-value polymorphic struct?`) and one call to the
  existing `emit_carrier_bridge` helper. Estimated ~1 session.
- The bridge is **explicitly scheduled to die** under M4: once dict
  fields hold per-instance typed function pointers, no reinterpret is
  needed. The diff is small and the deletion is mechanical.

If a future contributor reaches for the bridge to fix some *other* seam
(non-instance-method carrier mismatch), they should be redirected to
M4; this is a single-seam fix, not a general escape hatch.

## What lands

### 1. Detection

Two participants:

**At the elaborator** (`src/compiler/elab_typeclasses.c`, dispatch-call
type resolution -- search for the call sites that mint
`__inst_<Class>_<method>_<T>` references): when the method's declared
return type after instance-type substitution is a by-value polymorphic
struct (Result, Option, Pair, Tuple, Either, Slice per
`docs/parametric-type-abi-matrix.md`), record that the call's *result
type* is the substituted by-value type, NOT the carrier `int`. Without
this, the type-check that rejects `decode-bool.tur:32` never has the
information to accept the call.

**At emission** (`src/compiler/emit_expr.c`, EX_CALL lowering for
`__inst_*` callees): when the elaborator marked a call as by-value-typed
but the callee's C signature still returns `int64_t` (which it does
until M4 lands), insert an `emit_carrier_bridge` at the call-result
position. Predicate: reuse `type_uses_carrier_in_dispatch` -- this is
interim work; do not introduce a new predicate.

Model the new helper on `emit_inst_fn_return_carrier`
(`emit_internal.h:317`): same shape (FnDef + result-full-type -> C-type
decision), called from the same emit sites that the fn-typed twin is
called from (emit_fns.c:514, emit_module.c:2377, emit_stmt.c:518).

### 2. Reinterpret

If the predicate fires:

- The instance body is still emitted as-is (inline-C returning
  `int64_t`). No change to user-written instance bodies.
- The dispatch shim's call-site wrapper -- the per-instance C function
  pointer the dict holds -- gets a thin `static inline` wrapper that:

  ```c
  static inline Result__bool__cstr __inst_Decode_decode_bool__bridge(
      int64_t doc, int64_t val) {
    int64_t __c = __inst_Decode_decode_bool(doc, val);
    return *(Result__bool__cstr *)(intptr_t)__c;
  }
  ```

  This is exactly the shape `emit_carrier_bridge` already produces for
  stdlib accessors. Reuse the helper; do not duplicate the cast logic.

### 3. Dict slot type

The dict field for that method now holds a function pointer typed
`Result__bool__cstr (*)(int64_t, int64_t)`, not the uniform
`int64_t (*)(int64_t, int64_t)`. This is **per-instance**, not
per-class -- different instances of the same class may have different
dict slot types. That is the M4 direction in miniature, scoped to the
one method per instance whose return type triggers the bridge.

### 4. Call-site emit

Call sites of the method dispatch through the typed slot. No
reinterpret at the call site -- the shim absorbed it. Ascribed call
sites compile by C-level identity (the slot returns the declared
struct type directly).

## What does not land

- **No change to user-written instance bodies.** A `Decode bool`
  instance can keep its inline-C `return tur_ok(...)` exactly as
  written.
- **No change to stdlib `ok` / `err` / `some` / `none`.** They are
  already by-value via M2.
- **No change to non-instance-method polymorphic callsites.** Those
  already work through M2 + the existing twin path.
- **No general "carrier → by-value at any seam" facility.** The
  predicate is specifically "this emit is a typeclass instance method
  whose declared return is a by-value polymorphic struct."
- **Not HKT.** HKT class methods stay carrier-only until M7.

## Validation

1. **Spice-side decoder fixture.**
   `../turmeric-spices/spices/json/tests/decode-bool.tur` already
   exists and is currently red (TUR-E0001 at line 32). Expected:
   flips from FAIL to PASS, with stdout `1\n0\nerr\nerr\n`.
2. **`derive-decode-struct.tur`.** Same root cause, currently red at
   cc with a `Result__User__cstr` arithmetic-type error. Expected:
   flips from FAIL to PASS as a side effect. If it doesn't, the bridge
   predicate is not catching the relevant seam; investigate before
   merging. (See the in-tree clang trace in "the exact bug" section.)
3. **Full `bash tests/run.sh`.** Zero `FAIL` regressions. Snapshot
   regen if dispatch-shim wrappers move into emitted C (likely; budget
   for it per `CLAUDE.md`'s Fixture Snapshots rule).
4. **No new carrier bridges leak into non-instance-method emits.**
   Audit by greping the regenerated snapshots for the bridge wrapper
   name pattern and confirming every hit is an instance method.

## Deletion plan (when M4 lands)

When `docs/upcoming/m4-typeclass-per-method-abi-plan.md` ships:

1. Dict slots already hold per-instance typed function pointers
   (M4's whole job).
2. The instance method bodies get rewritten in M4 to return by-value
   directly (no `tur_ok` int64 carrier; same path stdlib already uses).
3. The bridge wrapper from this plan becomes dead. Delete the predicate
   call site, delete the wrapper-emit branch, and confirm `tests/run.sh`
   stays green. One PR.

## Prereqs done (2026-06-15 refinement pass)

- File references corrected: original draft cited `emit_typeclasses.c`
  (no such file). Real participants are `elab_typeclasses.c`,
  `emit_expr.c`, `emit_fns.c`, `emit_stmt.c`, `emit_module.c`.
- Error site corrected: the in-tree failure is TUR-E0001 at the
  elaborator, not a clang error. Predicate must participate in typing,
  not just emission.
- Fixture status corrected: `decode-bool.tur` is in-tree and red, not
  "intentionally not yet in-tree." `Decode [bool]` already ships at
  `spices/json/src/json/encode.tur:278`.
- Direction lock confirmed: Result/Option are in the by-value-struct
  camp per `docs/parametric-type-abi-matrix.md`, locked 2026-06-15 in
  `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`.
- Helper precedent identified: model new bridge on
  `emit_inst_fn_return_carrier` (the fn-typed twin already wired through
  the same three emit sites this work will touch).
- Sub-bug catalogued: the `derive-decode-struct.tur` clang failure is
  the same root cause in a wrong-direction shape (already-by-value
  producer being re-deref'd). The predicate must AVOID inserting a
  bridge when the producer already returns by-value; treat the fixture
  as a second acceptance gate.

## Cross-references

- `docs/upcoming/end-to-end-monomorphization-plan.md` -- parent plan.
- `docs/upcoming/m2b-make-struct-design.md` -- M2 design (shipped).
- `docs/upcoming/m4-typeclass-per-method-abi-plan.md` -- the structural
  successor that retires this bridge.
- `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
  -- explains why the bridge is *kept alive* under M3 instead of
  deleted; this plan is the extension that makes the bridge cover
  typeclass instance method returns too.
- `docs/reported/json-spice-missing-decode-bool-instance.md` --
  surfaced this gap on 2026-06-14; the `decode-bool.tur` fixture
  promised there is blocked on this work.

## Estimated cost

One session. Predicate detection + helper call + dict slot type
threading + snapshot regen. Smaller than M2b, smaller than M3, much
smaller than M4 / M7.
