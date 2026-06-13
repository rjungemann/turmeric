---
title: return-dispatch typeclass call site doesn't honor ascription when the return type is `(Result a B)` (works for bare `a`)
category: Elaborator gap / instance resolution
severity: Medium. Blocks the typed `Decode` typeclass surface for the json spice's P2a follow-up (`derive-json` Decode side). A bare return-dispatch class like `(defclass Show [a] (show [x] : a))` resolves instances by ascription correctly; wrapping the return in `(Result a B)` causes the elaborator to fall back to argument-type dispatch and pick the wrong instance.
description: With Prereqs 1-4 in tree, a return-dispatch class whose method returns `a` resolves instances by ascription: `(:: (show 42) :int)` picks `Show [int]`; `(:: (show 42) :cstr)` picks `Show [cstr]`. But wrapping the return in `(Result a cstr)` breaks resolution -- both `(:: (dec 42) (Result int cstr))` and `(:: (dec 42) (Result cstr cstr))` emit calls to `__inst_Dec_dec_int(...)`, ignoring the ascription's `a`-position. The dispatcher appears to bypass the ascription entirely when the return type is wrapped in a parameterized struct.
status: OPEN. Filed 2026-06-12 from the typed `Decode` follow-up work in `../turmeric-spices/spices/json`. Workaround in tree: `json/decode.tur` keeps the plain-defn primitive decoders (`json-decode-int`, `json-decode-cstr`) shipped in the original minimal slice; the typed `Decode` typeclass surface lands as a follow-up once this report is addressed.
---

# return-dispatch ascription not honored when return is `(Result a B)`

## Summary

Return-type-directed instance dispatch works when the class method's
return type is bare `a`:

```turmeric
(defclass Show [a] (show [v : int] : a))

(definstance Show [int]  (show [v] v))
(definstance Show [cstr] (show [v]
  ```c char *b = (char*)malloc(24); snprintf(b, 24, "STR%lld", (long long)v); return b; ```))

(println (:: (show 42) :int))   ; => 42         (picks Show [int])
(println (:: (show 42) :cstr))  ; => STR42      (picks Show [cstr])
```

But wrapping the return type in a parameterized struct `(Result a B)`
breaks dispatch: both ascriptions resolve to the same (first?)
instance:

```turmeric
(defclass Dec [a] (dec [v : int] : (Result a cstr)))

(definstance Dec [int]  (dec [v] (ok v)))
(definstance Dec [cstr] (dec [v] (ok "hello")))

;; emitted C calls __inst_Dec_dec_int for BOTH ascriptions:
(println (ok-val (:: (dec 42) (Result int  cstr))))   ; => 42       OK
(println (ok-val (:: (dec 99) (Result cstr cstr))))   ; => segfault  WRONG
```

The emit shows both ascriptions calling `__inst_Dec_dec_int(...)` then
attempting to reinterpret its int64_t return through the bridge as
`Result__cstr__cstr` -- which (after Prereq 2's bridge) dereferences a
small integer as a heap pointer and segfaults.

## Where this surfaces

The canonical end-to-end shape the typed `Decode` typeclass surface
wants:

```turmeric
(defclass Decode [a] (decode [doc val] : (Result a cstr)))

(definstance Decode [int]  (decode [doc val] ...))
(definstance Decode [cstr] (decode [doc val] ...))

;; The user pins the instance via the ascription's `a`:
(:: (decode doc off) (Result int  cstr))   ; want Decode [int]
(:: (decode doc off) (Result cstr cstr))   ; want Decode [cstr]
```

Both ascriptions in the json spice's `tests/decode-primitives.tur`
resolved to `__inst_Decode_decode_int`, so the cstr field decode
returned an int (the byte offset) and printed `(null)` after the bridge
tried to interpret it as `Result__cstr__cstr`.

## Hypothesis

Likely site: the elaborator's instance-resolution path (probably
`elab_typeclasses.c`'s call-site dispatch resolution, separate from
the substitution site fixed in Prereq 4). When the method's return
type is bare `a`, the resolver unifies `a` against the ascription's
type kind directly. When the return is wrapped in
`(type-app Result a cstr)`, the resolver may:

  - skip the unification step and fall back to argument-type dispatch
    (picking the int instance because the args are int), OR
  - unify `(Result a cstr)` against `(Result int cstr)` correctly for
    one ascription but use a cached/shared decision for the other.

Need to instrument the dispatch resolution to confirm which.

## Workaround now in tree

`json/decode.tur` keeps the plain-defn primitive decoders
(`json-decode-int`, `json-decode-cstr`) from the original minimal
slice -- they return the value directly with a sentinel error
(`-1`, `NULL`). The typed `Decode` typeclass surface is deferred until
this report is addressed.

## Proposed fix direction

Inspect the call-site instance resolution in `elab_typeclasses.c`
(separate from the parse / definstance substitution sites). For a
return-dispatch method whose return type mentions the class tyvar
inside a `TY_APP` head (e.g. `(Result a cstr)`), unify the ascribed
type against the return-type form to extract the `a`-position binding,
then dispatch off that.

The bare-`a` case already works, so the resolver knows how to
do this for the simplest shape. The fix is likely making the
unification recurse into the type-app's args.

## Validation when fixed

- The shape above (`(defclass Dec [a] (dec [v : int] : (Result a cstr)))`
  with two instances) should dispatch correctly to each instance based
  on the ascription's `(Result A cstr)` `A` position.
- Reactivate the typed `Decode` surface in `json/decode.tur`. Move
  `json-decode-int` / `json-decode-cstr` to be `definstance Decode
  [int]` / `[cstr]` (similar to the attempt in
  `docs/archive/history/typeclass-method-parameterized-result-carrier-mismatch.md`'s
  "validation when fixed" section).
- Extend `derive-json` in `json/encode.tur` to emit
  `(definstance Decode [T] ...)` alongside the Encode emission.
