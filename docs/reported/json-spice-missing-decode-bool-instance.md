---
title: json spice -- missing `Decode bool` instance breaks `derive-json` on any struct with a `bool` field
category: Reported -- spice (json) / typeclass instance gap
severity: ergonomics gap (compile-time error, no miscompile, no silent breakage)
---

# json spice -- missing `Decode bool` instance

## Summary

`../turmeric-spices/spices/json` ships `Encode` instances for `int`, `bool`,
and `cstr`, and `Decode` instances for `int` and `cstr` -- but **not**
`Decode bool`. The `derive-json` macro at
`spices/json/src/json/encode.tur:283-296` emits a `(decode doc ...)` call per
field of the user's struct, and each call requires a `Decode T` instance for
the field's declared type. So any user `defstruct` with a `bool` field --
including the canonical example in `tests/derive-encode-struct.tur` -- fails
to elaborate with:

```
src/json/encode.tur:285:10: error: no instance 'Decode bool'
```

even though the user only intended to *encode* (not decode) the struct: the
macro generates both sides unconditionally, so the missing `Decode bool`
hits the *encode* test path too.

## Severity

**Ergonomics gap.** It's a hard compile-time error, not a miscompile, and
the message points roughly to the right place (`derive-json` expansion).
But it is a "the example in your own README would fail" gap: any realistic
domain struct has at least one boolean (`active`, `enabled`, `is-admin`,
`deleted`, ...) and `derive-json` is *the* user-facing surface of the
spice.

This is independent of the carrier/by-value ABI rework in
[`docs/upcoming/end-to-end-monomorphization-plan.md`](../upcoming/end-to-end-monomorphization-plan.md);
filling the instance is a small, local fix.

## Repro

`../turmeric-spices/spices/json/tests/derive-encode-struct.tur` already
reproduces it. Minimal:

```turmeric
(defmodule repro (export)
(import json/encode :refer [derive-json])
(defstruct User [id : int  name : cstr  active : bool])
(derive-json User (id int) (name cstr) (active bool))
(defn main [] : int
  (println (encode (make-struct User 7 "alice" true)))
  0))
```

Run:

```
./build/tur run ../turmeric-spices/spices/json/tests/derive-encode-struct.tur
```

Observed:

```
src/json/encode.tur:285:10: error: no instance 'Decode bool'
283 | (defmacro __decode-field [fname ftype]
284 |   `(ok-val
285 |      (:: (decode doc (unsafe (json-obj-get doc val ~(symbol-name fname))))
    |          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
286 |          (Result ~ftype cstr))))
```

Expected: program compiles and emits
`{"id":7,"name":"alice","active":true}` to stdout.

## Root cause

`spices/json/src/json/encode.tur` declares:

- `Encode [int]`   (line 36)
- `Encode [bool]`  (line 46)
- `Encode [cstr]`  (line 59)
- `Decode [int]`   (line 194)
- `Decode [cstr]`  (line 224)

There is no `Decode [bool]`. The `derive-json` macro (line 363+) splices the
field's declared type into `__decode-field`, which expands at every field to
`(decode doc ...)` with that type as the result ascription
(`(Result ~ftype cstr)`). For a `bool` field, elaboration searches for an
instance with head `Decode bool` and finds none.

Note the asymmetry: `Decode [int]` at line 202-203 already parses
`"true"`/`"false"` source bytes and returns `tur_ok((int64_t)1)` /
`tur_ok((int64_t)0)`, so the actual JSON-to-bool parsing logic already
exists -- it just lives inside `Decode int` instead of being exposed as
`Decode bool`. The fix is mechanical, not semantic.

## Proposed fix

Add a `Decode [bool]` instance next to the other Decode instances in
`spices/json/src/json/encode.tur`. The body can either:

1. **Pure inline-C**, mirroring the `"true"`/`"false"` probe already in
   `Decode int` (lines 202-203). Reject anything else as
   `tur_err("decode bool: not a boolean")`.

2. **Delegate to `decode :: int`** (cheaper to write, slightly looser):
   call the existing int instance and treat non-zero as `true`. Loose
   because it would also accept `42` as `true`; reject if the spice wants
   strict JSON-`true`/`false`-only semantics.

(1) is the right call -- it matches what a JSON decoder should do.

Sketch:

```turmeric
;;; Decode [bool] -- accept literal `true` / `false`; reject anything else.
(definstance Decode [bool] (decode [doc val]
  ```c
  typedef struct { char *src; size_t len; } __jdoc;
  __jdoc *d = (__jdoc *)(intptr_t)doc;
  if (!d || val < 0 || (size_t)val >= d->len) {
    return tur_err((int64_t)(intptr_t)"decode bool: out-of-range val handle");
  }
  size_t i = (size_t)val;
  if (i + 4 <= d->len && memcmp(d->src + i, "true",  4) == 0) return tur_ok((int64_t)1);
  if (i + 5 <= d->len && memcmp(d->src + i, "false", 5) == 0) return tur_ok((int64_t)0);
  return tur_err((int64_t)(intptr_t)"decode bool: not a boolean");
  ```))
```

## Validation

1. `./build/tur run ../turmeric-spices/spices/json/tests/derive-encode-struct.tur`
   should print `{"id":7,"name":"alice","active":true}` and exit 0.
2. Add a `tests/decode-bool.tur` fixture that round-trips `true`/`false`
   through `derive-json` on a struct with a `bool` field. Should be a
   trivial sibling of `round-trip.tur`.
3. Reject case: feed `"42"` to `decode :: bool` and confirm it returns
   `err`, not silently coerces.
4. The other four json fixtures (`encode-primitives`, `decode-primitives`,
   `round-trip`, `derive-encode-struct`) must continue to pass. (Note:
   `derive-decode-struct` will still fail until M2 of the monomorphization
   plan lands -- that's a separate, known issue.)

## Cross-references

- Surfaced while running the spice-side validation baseline for
  [`docs/upcoming/end-to-end-monomorphization-plan.md`](../upcoming/end-to-end-monomorphization-plan.md)
  on 2026-06-14.
- Related but distinct from the open carrier/by-value report
  `polymorphic-ok-in-typeclass-instance-method-with-value-struct-payload.md`,
  which is what makes `tests/derive-decode-struct.tur` fail. This report is
  about a missing instance; that report is about an ABI seam.
- P2a minimal slice memory note
  (`project_p2a_derive_json_minimal.md`) records that the initial slice
  shipped `Encode int/bool/cstr + Decode int/cstr`. This report is the
  follow-up that closes the `Decode bool` gap left by that slice.
