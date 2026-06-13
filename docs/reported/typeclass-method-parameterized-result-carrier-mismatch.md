---
title: typeclass instance methods returning a parameterized struct (e.g. `(Result a cstr)`) cannot be made to compile -- three interacting issues
category: Codegen / dispatch / stdlib gap
severity: Medium. Blocks the typeclass surface for any `decode`-shaped class (Decode, Validate, Parse, …) -- anything whose method's natural return type is `Result a B` for some `a` chosen by the call-site instance. Surfaced while landing the P2a `derive-json` Decode side; the workaround (plain-defn decoders with sentinel error values) ships but loses type-level error context.
description: Implementing `(defclass Decode [a] (decode [doc val] : (Result a cstr)))` plus `(definstance Decode [int] ...)` plus `(definstance Decode [cstr] ...)` and consuming them at the call site as `(:: (decode doc off) (Result int cstr))` trips three layered issues that prevent any combination from compiling cleanly. None is the others' root cause; fixing any one in isolation does not unblock the surface.
status: OPEN (Issues 1 & 3 RESOLVED 2026-06-12 as Prereqs 1 & 3; Issue 2 still blocks the typed surface). Filed 2026-06-12 from the P2a Decode minimal-slice work in `../turmeric-spices/spices/json`. Workaround in tree: `json/decode.tur` ships plain-defn primitive decoders (`json-decode-int`, `json-decode-cstr`) returning the value directly with a sentinel error (-1 / NULL). Documented in the module header. When Issue 2 is addressed, the typeclass surface lands as a P2a follow-up.
---

# typeclass instance with parameterized-Result return cannot compile

## Summary

The plan doc's stated P2a target shape is:

```turmeric
(defclass Decode [a] (decode [doc val] : (Result a cstr)))

(definstance Decode [int] (decode [doc val] <inline-C builds Result__int__cstr>))
(definstance Decode [cstr] (decode [doc val] <inline-C builds Result__cstr__cstr>))

;; consumer:
(let [r (:: (decode d v) (Result int cstr))] ...)
```

This trips three interacting compiler / stdlib issues, surfaced in this
order while debugging:

1. **Return-type-dispatched typeclass call site loses monomorphization
   under `(unsafe ...)`.**  **RESOLVED 2026-06-12 as Prereq 1.**
   Consumer code shaped like
   `(unsafe (__int->cstr (ok-val (:: (decode doc off) (Result int cstr)))))`
   lowered to C that called `ok_hyval(...)` -- an undeclared C identifier --
   because the polymorphic `(defn ok-val [A B] [r : (Result A B)] : A ...)`
   in stdlib/result.tur was not monomorphized for the instance's
   concrete `A`. Root cause: `emit_abi_scan_expr` in `emit_module.c` had
   no case for `EX_HANDLE`, so the worklist seeding walker fell into
   `default: break;` and never traversed the `(unsafe ...)` body's call
   graph. Fix (mirror of the 2026-06-12 `EX_EXISTS_OPEN` fix from
   `docs/archive/history/open-monomorphizes-polymorphic-fn-only-partially.md`):
   add an explicit `EX_HANDLE` case that recurses into the handle body
   plus every `cases[i].body`. The `ok_val__spec__*` specialization is
   now emitted; the post-Prereq-1 failure mode is Issue 2's ABI
   mismatch (an `int64_t` carrier flowing into a function that expects
   the by-value struct), which is the next layer to peel.

2. **`Result A B` uses the carrier ABI (int64_t) at the value level,
   incompatible with by-value struct return from typeclass instance shims.**
   A parameterized struct (`type_uses_carrier_abi` returns true for
   `n_type_params > 0`) is lowered to `int64_t` at every value-level
   binding site. The typeclass instance method dispatch shim, however,
   emits its return type as the concrete `Result__int__cstr` struct.
   So a `defn` returning `(Result int cstr)` and an instance method
   returning the same type have *different* C ABIs: `int64_t` vs.
   `struct Result__int__cstr`. An inline-C body in the instance method
   cannot return a value that the caller will accept -- both readers of
   the result (the dispatch shim's caller and any monomorphized
   `ok_val_spec_*`) disagree about its in-memory shape.

3. **`(ok x)` is monomorphic on `:int`, so `(Result a B)` for non-int
   `a` has no usable constructor.**  **RESOLVED 2026-06-12 as Prereq 3.**
   `stdlib/result.tur:27` declared `(defn ok [x : int] : int ...)`.
   Attempting `(ok "hi")` for a `(Result cstr cstr)` rejected at
   TUR-E0001. Made polymorphic by mirroring `pair`'s pattern:
   `(defn ok [A B] [x : A] : (Result A B) ...)` plus a matching `err`
   `(defn err [A B] [e : B] : (Result A B) ...)`. The inline-C body
   still goes through the int64 carrier helpers `tur_ok` / `tur_err`
   with `(int64_t)(intptr_t)x` boxing -- the type-level polymorphism is
   what unlocks ascriptions like `(:: (ok "hi") (Result cstr cstr))`;
   the runtime ABI continues to use the uniform int64 carrier. B (in
   `ok`) and A (in `err`) are phantom tyvars inferred from context.
   Suite: 73 codegen-snapshot regenerations (the prelude no longer
   unconditionally emits `static int64_t ok(int64_t)` declarations;
   monomorphizations are emitted on demand per call site, matching
   how `pair` already works). Net diff: 0 regressions vs current
   baseline (hamt-delete pre-existing flake).

Any of the three issues could be considered the trigger; all three must
be addressed before a typed `Decode` surface (or `Validate`, or `Parse`,
or anything whose method returns a `Result` parameterized over the
typeclass tyvar) is shippable.

## Repro

The trail of attempts (in `../turmeric-spices/spices/json` history):

1. `(definstance Decode [int] (decode [doc val] : (Result int cstr) ...))`
   -- the return-type annotation on the method body is rejected as
   "type annotation ': type' is only valid after a parameter name or as
   a return type" even though the same shape works for `Eq [cstr]`
   instances. Dropping the annotation makes the class declaration
   accept; the dispatched method's return type is taken from the class.

2. Inline-C body allocating `struct __res *r = malloc(...); ... return r;`
   and returning `Result__int__cstr` -- "returning 'Result__int__cstr'
   from a function with incompatible result type 'int64_t'". The
   instance shim's declared return type is `int64_t` (carrier ABI), but
   the by-value cast was for the struct.

3. Inline-C body building the struct by value (`Result__int__cstr r;
   r.is_ok = true; ...; return r;`) -- the *body* compiles, but a
   downstream `(defn decode-id [doc] : (Result int cstr) (decode doc
   off))` emits a function that returns `int64_t` while its body
   evaluates to a `Result__int__cstr` -- same mismatch, different
   direction.

4. Letting the inline-C return `ptr<void>` (matching `json-parse`'s
   approach) -- defeats the typeclass dispatch's return-type
   unification: every instance returns the same C type and the dispatch
   shim has nothing to specialize.

5. Consumer code with `(:: (decode d v) (Result int cstr))` plus
   `(ok-val ...)` inside `(unsafe ...)` -- compiles but emits an
   undeclared `ok_hyval(...)` (issue 1).

## Workaround now in tree

`json/decode.tur` ships **plain `defn` decoders**:

```turmeric
(defn json-decode-int  [doc : int val : int] #{Unsafe} : int)
(defn json-decode-cstr [doc : int val : int] #{Unsafe} : cstr)
```

Returns the value directly; sentinel-encodes errors (`-1` for int,
`NULL` for cstr). Documented in the module header that the typeclass
surface is deferred until this report's issues are resolved.

## Proposed fix shape

Solving all three is a session of its own. Sketch:

1. **For (1)**: trace why `ok-val`'s monomorphization is suppressed
   under `EX_HANDLE` (the `unsafe` block's effect-handler frame); the
   `emit_module.c` abi-scan likely needs the same recursive descent that
   `emit_abi_scan_expr`'s pack/open fix added (see resolved-paper-trail
   2026-06-12 entry for `EX_EXISTS_PACK`/`EX_EXISTS_OPEN`). The
   `EX_HANDLE_EFFECT` body is structurally analogous: it has a body
   whose calls need to seed the worklist via the outer dispatcher.

2. **For (2)**: pick *one* ABI for `Result A B`. Either (a) lift the
   carrier-ABI rule for `Result` (so `Result int cstr` is by-value
   everywhere, paying the ABI cost), or (b) make typeclass instance
   shims with parameterized-struct return type box-and-cast through
   carrier ABI exactly like normal call sites do.  (b) is consistent
   with the existing ABI but requires the instance shim to allocate +
   return an `int64_t` carrier rather than the struct.

3. **For (3)**: add `defn ok [A] [x : A] : (Result A B)` -- a
   polymorphic constructor. Stdlib hand-rolls it as inline-C today; an
   `:A`-polymorphic version requires picking a representation that
   tolerates any element type (likely going through the int64 carrier
   for `(ok x)`).

## Validation when fixed

Reactivate the typeclass surface in `json/decode.tur` (revert the
"workaround in tree" stanza). Move `json-decode-int` / `json-decode-cstr`
to be `definstance Decode [int]` / `[cstr]`. Update the fixture
`spices/json/tests/decode-primitives.tur` to call `(decode doc off)`
with type ascription instead of the per-type defns. Extend
`derive-json` in `json/encode.tur` to emit `(definstance Decode [T] ...)`
alongside the existing Encode emission.
