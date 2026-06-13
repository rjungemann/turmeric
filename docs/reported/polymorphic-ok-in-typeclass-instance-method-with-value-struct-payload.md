---
title: polymorphic `(ok user)` inside a typeclass instance method body still fails for value-struct payloads (remaining gap after Prereq 6)
category: Codegen / ABI gap
severity: Medium. Blocks the macro-emitted Decode side of `derive-json` for `defstruct` types in `../turmeric-spices/spices/json`. Prereq 6 fixed the standalone call shape `(let [r : (Result T B) (ok (make-struct T ...))] ...)`; the typeclass-instance-method context is left over because the spec's result_type lowers to the int64 carrier (per the dispatch shim convention), which the direct-by-value Result construction synthesized by Prereq 6 can't match.
description: After Prereq 6 (2026-06-13, `docs/archive/history/polymorphic-ok-fails-for-value-struct-payload.md`), `(ok (make-struct User 7 "alice"))` at top level synthesizes a direct by-value Result__User__cstr struct construction. Inside a typeclass instance method body (e.g. `(definstance Decode [User] (decode [doc val] (ok (make-struct User ...))))`), the `ok` call's spec is interned with `result_type = int64_t` (the carrier ABI used by typeclass dispatch shims), so Prereq 6's synthesized direct-struct emission can't fire -- it would mismatch the int64 function signature. The original carrier-box helper sketch from the parent report can't fill the gap either: tur_ok's int64 ok_val slot can't hold a multi-byte value-struct, and dereferencing the carrier as Result__User__cstr reads off the end.
status: OPEN. Filed 2026-06-13 as the leftover from Prereq 6. Workaround in tree: `derive-json`'s Decode-side emission stays gated (only Encode is emitted by the macro). Hand-written `(definstance Decode [T] (decode [doc val] ```c ... return tur_ok((int64_t)(intptr_t)heap_t); ```))` using inline-C heap-spill works fine; the macro-driven path is what's blocked.
---

# polymorphic `ok` in typeclass instance method body, value-struct payload

## Summary

Prereq 6 added a synthesized-body path in `src/compiler/emit_fns.c`
for polymorphic `ok` / `err` monomorphizations when the spec resolves
to a value-struct A and the spec's C return type is the by-value
Result struct. The standalone shape works:

```turmeric
(defstruct User [id : int  name : cstr])
(let [r : (Result User cstr) (ok (make-struct User 7 "alice"))
      u (ok-val r)]
  (println (.name u)))   ; => alice
```

The typeclass-instance-method shape does NOT work yet:

```turmeric
(defclass Wrap [a] (wrap [v : int] : (Result a cstr)))
(definstance Wrap [User] (wrap [v] (ok (make-struct User v "x"))))

;; Inside the instance method body the spec for `ok` is interned with
;; result_type = int64_t (the carrier ABI used by typeclass dispatch
;; shims), so Prereq 6's synthesized direct-struct emission would mismatch
;; the function's int64 return signature. The Prereq 6 guard correctly
;; skips synthesis in this case, but the fallback path (inline-C body +
;; heap-spill) produces a tur_ok carrier handle whose layout doesn't match
;; what callers reading Result__User__cstr's ok_val expect.
```

The fallback path emits the carrier int64 from `tur_ok((int64_t)(intptr_t)heap_user)`,
which is a pointer to a `tur_result_box_t { bool is_ok; int64_t ok_val;
int64_t err_val; }`. Consumers that dereference this as
`Result__User__cstr` read off the end (User's ok_val field is wider
than int64).

## Where this surfaces

`derive-json`'s Decode-side emission in
`../turmeric-spices/spices/json/src/json/encode.tur`. The macro
expansion shape is exactly:

```turmeric
(definstance Decode [User]
  (decode [doc val]
    (ok (make-struct User <decoded-id> <decoded-name>))))
```

Macro plumbing (`__decode-field`, `__decode-fields`,
`__decode-make-struct`) is preserved in tree behind an Encode-only
`derive-json` emission so the Decode side can flip on once this gap
closes.

## Root-cause hypothesis

Two intertwined facts:

1. **Typeclass instance method dispatch returns int64 by convention.**
   The dispatch shim signature for a typeclass method is
   `int64_t __inst_Class_method_T(...)` regardless of the declared
   Turmeric return type. This is what lets dispatch dicts be uniform
   `(int64_t (*)(int64_t, int64_t))` function pointers without
   per-type variation.

2. **Polymorphic `ok` called from inside such a method has its spec's
   result_type lowered to int64**, matching the enclosing method's
   return ABI. Per Prereq 6's design, the synthesis path requires the
   spec's C return type to be the by-value Result struct, so it
   correctly bails in this case.

The fallback (the original inline-C body with heap-spill) ends up
producing a tur_ok carrier handle whose box layout doesn't match the
by-value Result__T__B struct layout that consumers downstream of the
dispatch shim use. The mismatch is real but doesn't surface as a
compile error -- the code links, runs, and reads garbage at the field
access.

## Proposed fix directions

Two plausible shapes, both more invasive than Prereq 6 was:

1. **Make `Result__T__B`'s ok_val a HEAP POINTER to T** (and similarly
   for err_val) when T is a value-struct. The struct layout becomes
   uniform 8-byte slots, the carrier box layout matches it byte-for-byte,
   and existing tur_ok / tur_err just work. Cost: a per-type-arg
   layout decision that ripples through every consumer of Result__T__B.

2. **Per-method ABI: typeclass instance methods returning a
   parameterized struct return the by-value struct, not int64.**
   The dispatch dict's function pointer becomes per-type, not
   uniform; the dispatch shim doesn't need to bridge. Cost: dict
   layout becomes per-method-signature, which complicates HKT
   typeclasses (`Functor [f]`, `Monad [m]`) where the dispatched
   tyvar appears in many positions.

(1) is the smaller blast radius for `Result` specifically; (2) is the
right "general" fix if more parameterized-struct returns from
instance methods come up.

## Validation when fixed

- Flip `derive-json` in
  `../turmeric-spices/spices/json/src/json/encode.tur` to emit both
  Encode and Decode instances using the existing
  `__decode-make-struct` macro plumbing.
- Add a fixture under `../turmeric-spices/spices/json/tests/` that
  parses JSON, decodes to a User via `(:: (decode doc root) (Result User cstr))`,
  then encodes back -- collapsing to a single
  `(derive-json User (id int) (name cstr))` declaration with no
  hand-written instance.
- Confirm the main-repo fixture
  `tests/fixtures/polymorphic-ok-err-value-struct-payload/` still
  passes (the standalone path should keep working under either fix
  direction).
