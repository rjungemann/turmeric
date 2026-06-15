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

### Minimal repro (intentionally not yet in-tree)

```turmeric
(definstance Decode [bool] (decode [doc val]
  ```c
  /* ... probe "true" / "false" ... */
  return tur_ok((int64_t)1);
  ```))

(defn show [b : bool] : void
  (if b (println "1") (println "0")))

(defn main [] : int
  (let [r (:: (decode doc handle) (Result bool cstr))]
    (show (ok-val r))   ;; <-- clang error here
    0))
```

clang reports:

```
error: passing 'int64_t' to parameter of incompatible type 'Result__bool__cstr'
```

The instance body produced an `int64_t` (via `tur_ok(...)`), but the
ascribed type `(Result bool cstr)` lowers to the by-value struct
`Result__bool__cstr`. The C compiler is right; the ABI seam is wrong.

### Where the seam lives

| Layer | What it does | Status |
|---|---|---|
| Polymorphic stdlib constructors (`ok`, `err`, `some`, `none`) | `#{Construct}` + `make-struct` -- by-value codegen | **Done (M2).** See `stdlib/result.tur:36-66`. |
| Typeclass dispatch dict layout (`emit_typeclasses.c`) | Uniform `int64_t (*)(...)` slot per method | **Open (M4).** Tracked in `docs/upcoming/m4-typeclass-per-method-abi-plan.md`. |
| Carrier-bridge / by-value twin (`emit_carrier_bridge`, PRs #364/#369) | Reinterpret int64 carrier as by-value struct at known boundaries | **Live.** Already covers stdlib accessors. |
| **This plan** -- extend the bridge to typeclass instance method returns | Reinterpret the dispatch shim's `int64_t` return as the declared by-value struct | **Open. Subject of this doc.** |

The third row is what we reuse; the fourth row is what we add.

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

In `emit_typeclasses.c` (or wherever the dispatch-shim wrapper is
emitted), at the point a typeclass instance method is being lowered:

- Resolve the method's declared return type after instance-type
  substitution. (e.g. for `Decode bool`, the `decode` method's declared
  return `(Result A cstr)` substitutes to `(Result bool cstr)`.)
- Predicate: is this type a *by-value polymorphic struct*? Reuse the
  existing `type_uses_carrier_in_dispatch` check inverted, or whatever
  predicate `emit_carrier_bridge` already keys on. Do **not** introduce
  a new predicate -- this is interim work; rely on what already
  classifies these types.

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

1. **Spice-side decoder fixture.** Write
   `../turmeric-spices/spices/json/tests/decode-bool.tur` with a typed
   helper (`(defn show [b : bool] ...)`) consuming the ascribed
   `(Result bool cstr)`. Expected: PASS.
2. **`derive-decode-struct.tur`.** The same root cause blocks this
   fixture today. Expected: flips from FAIL to PASS as a side effect.
   If it doesn't, the bridge predicate is not catching the relevant
   seam; investigate before merging.
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
