---
title: Under `--interpret`, `type-of` on an `any` holding an inline-C-produced opaque value dereferences the value as a struct pointer and segfaults
category: Reported
description: A `defopaque` value returned from an inline-C body arrives as a TURI_STRUCT-tagged TuriValue whose as_struct is the raw integer the body returned (e.g. 0x7). turi_any_named_type checks only the tag and non-NULL, so it dereferences 7 as a TuriStruct* -- UBSan reports a misaligned member access and ASan a SEGV on the zero page.
---

# An inline-C opaque value crashes the interpreter's `any` reflection

**Severity: high (crash), narrow (one path).** A hard segfault, not a wrong
answer -- but it needs only `type-of` on a value a stdlib-style `defopaque`
constructor produced.

Found while fixing
[interp-collection-handles-report-as-int](../archive/interp-collection-handles-report-as-int.md):
the first probe for that report used an inline-C `defopaque` constructor, which
is how it surfaced. It is **pre-existing and unrelated** -- the guard it trips
predates this session's `any` work, and the crash reproduces with no boxing
involved.

## Repro (2026-09-07)

```turmeric
(defopaque Route :int)
(defn route-of [n : int] : Route
  ```c
  return n;
  ```)
(defn boxed [] : any (route-of 7))
(defn main [] : int (println (type-of (boxed))) 0)
```

```
$ tur run p.tur
Route

$ tur --interpret p.tur
src/turi/eval.c:1169:20: runtime error: member access within misaligned address
  0x000000000007 for type 'struct TuriStruct', which requires 8 byte alignment
AddressSanitizer: SEGV on unknown address 0x000000000001f
```

Replacing the inline-C constructor with an ascription -- `(:: 7 Route)` -- does
not crash, which localises it to the inline-C return path rather than to
`defopaque` itself.

## Root cause

`turi_any_named_type` validates only the tag and non-NULLness before
dereferencing:

```c
static const char *turi_any_named_type(TuriValue v) {
    if (v.tag != TURI_STRUCT || !v.as_struct) return NULL;
    if (!turi_struct_is_struct_like(v) && v.as_struct->ctor && ...
```

A pointer of `7` passes both checks. The value comes from the interpreter's
inline-C path: the body returns the plain integer `n`, and because the declared
return type is an opaque (a lowered ADT), the result is tagged `TURI_STRUCT`
while the payload is still the immediate. Nothing reconciles the two.

## Why nothing caught it

`tests/run-turi.sh` PASS-skips every fixture containing a user inline-C block
(the TI7 carve-out, ~767 fixtures), so no fixture in the tree exercises an
inline-C `defopaque` under `--interpret` at all.

## Fix directions

1. **Do not tag an inline-C result as a struct when the body returns an
   immediate.** The declared type says "opaque", and an opaque over `:int` is an
   immediate at runtime -- the interpreter should carry it as `TURI_INT`, which
   is what the ascription path already produces (and what makes that path work).
   This is the real fix, and it makes the value indistinguishable from the one
   the non-inline-C constructor builds.
2. **Harden the reader regardless.** `turi_any_named_type` and
   `turi_struct_is_struct_like` dereference a pointer they have not validated;
   any other producer of a mis-tagged value crashes the same way. A cheap
   plausibility check (or, better, never constructing the value in the first
   place) turns a segfault into a wrong-but-safe answer.

Direction 1 is the fix; 2 is defence in depth. A fixture needs a dedicated
runner or a `requires.*` marker, since the inline-C carve-out would skip it.
