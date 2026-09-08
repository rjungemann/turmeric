---
title: "A match arm binder in an `any`-substituted ADT monomorph is typed as the int64 carrier, not `tur_tagged_t`"
category: Reported
description: "`(match b (MkBox x) x)` on a `(Box any)` emits `int64_t x = (int64_t)__scrut.as.MkBox._0;` where the monomorph's field is a 16-byte tur_tagged_t. The struct layout substitutes A := any correctly; the arm binder's C type does not, so it takes the generic bare-tyvar carrier. Uncompilable C. No Saffron required."
---

# A match arm binder in an `any` monomorph keeps the carrier type

**RESOLVED 2026-09-08 -- and it was worse than filed.** Direction 1: the binder
now takes its C type from `adt_field_type_for_app` against the monomorph, the
same substitution the layout already did. Pinned by
`tests/fixtures/match-binder-monomorph-substituted-type`.

**The filed `any` case was the LOUD half.** Probing the fix with a fractional
float -- per the float rule in CLAUDE.md -- turned up a silent one from the
same mistake, and it is the more severe:

```turmeric
(defdata Box [a] (MkBox a))
(defn ub [A] [b : (Box A)] : A (match b (MkBox x) x))
(println (ub (:: (MkBox 3.25) (Box float))))   ;; printed 3
```

`(Box float)` lays the slot out as `double _0` and emitted the same
`int64_t x = (int64_t)__scrut.as.MkBox._0;`. Aggregate-to-integer is a C error,
which is why `any` failed loudly; double-to-integer is a legal LOSSY
conversion, so this compiled and returned the wrong number -- 3.25 -> 3,
7.9 -> 7, 0.5 -> 0, -2.75 -> -2. No diagnostic, no crash, and no Saffron
anywhere near it. An integer-valued float would have hidden it completely.

The fix is scoped to a field whose DECLARED type is a tyvar and whose
substitution lands on a scalar, cstr or `any` -- the cases where the monomorph
slot holds that type inline, so the honest binder type is its own and the cast
was the error. Aggregate and handle payloads are left to the deref and box
branches that already reason about how they are stored.

Suites: run.sh 2891 passed / 0 failed; run-turi.sh 1982 passed / 0 failed.

**Severity: medium** as filed; **high** in hindsight, for the float half. It is a *loud* failure -- the emitted C does not compile,
so nothing miscomputes. What it blocks is a user-defined parametric ADT holding
`any`, which is a shape the Saffron dialect reaches on any ordinary program and
plain Turmeric reaches whenever someone writes `(Box any)`.

Found while fixing
`docs/archive/saffron-unannotated-param-container-cast-panics.md`. That fix
turned this shape from a runtime panic into this compile error -- a better
failure mode, and still a failure. **It is not caused by that change:** the
repro below has no Saffron in it and fails the same way with the fix reverted.

## Repro

Plain Turmeric. No `#lang`, no dialect, no seam.

```turmeric
(defdata Box [a] (MkBox a))
(defn my-unbox [A] [b : (Box A)] : A (match b (MkBox x) x))
(defn main [] : int
  (let [b (:: (MkBox (:: 7.1 any)) (Box any))]
    (println (cast (my-unbox b) float)))
  0)
```

```
$ tur run box-tur.tur
/tmp/tur-build/box-tur_tur.c:7364:17: error: aggregate value used where an
integer was expected
/tmp/tur-build/box-tur_tur.c:7365:26: error: incompatible types when assigning
to type 'tur_tagged_t' from type 'int64_t' {aka 'long int'}
```

Substitute any concrete type for `any` -- `(Box int)`, `(Box float)` -- and it
compiles and runs. The element type being `any` is the whole trigger.

## Root cause

The monomorph's STRUCT LAYOUT substitutes `A := any` correctly. `any` is a
16-byte `tur_tagged_t`, and the field is emitted as one:

```c
typedef struct tur_adt_Box__any {
    union {
        struct { tur_tagged_t _0; } MkBox;
    } as;
} tur_adt_Box__any;
```

The MATCH ARM BINDER does not. It takes its C type from the constructor's
GENERIC field type -- a bare tyvar, which lowers to the int64 carrier -- and
emits:

```c
static tur_tagged_t my_unbox__spec__tur_tagged_t_tur_adt_Box__any(tur_adt_Box__any b) {
    tur_tagged_t __t215 = {0};
    {
        tur_adt_Box__any __scrut = (b);
        {
            int64_t x_1472 = (int64_t)__scrut.as.MkBox._0;   /* <- aggregate -> int64_t */
            __t215 = x_1472;                                  /* <- int64_t -> tur_tagged_t */
            goto ____t216;
        }
        ...
```

Both errors are the same mistake seen from its two ends: the read casts a
16-byte aggregate to an integer, and the write assigns an integer back into a
16-byte aggregate.

So the specialization is half-applied. `emit_abi_intern_spec` produced a
correct `Box__any` layout and the arm binder was left on the canonical path.
This is the same class as the repr-shadow issues -- a type whose
`type_has_concrete_codegen_layout` row is `false` (TY_ANY's row is `false`
deliberately, because a 16-byte by-value field is an ABI change) reaching a
site that assumes the carrier.

I have **not** confirmed which of the two candidate sites is at fault -- the
arm binder's ctype lookup, or the monomorph's substitution not reaching it --
which is why this is filed rather than fixed.

## Fix directions

1. **Type the arm binder from the MONOMORPH's field type, not the generic
   one.** -- **TAKEN.** The substituted type is available: `scrut_ty` is the
   monomorph app and `adt_field_type_for_app(&scrut_ty, &ctor->fields[bi])`
   resolves the declared field against it. The switch path already carried the
   complementary leg (`!scrut_is_app_monomorph`, resolving a tyvar through the
   active spec); this is the monomorph one, and the niche arm's own comment
   already described the problem ("this path spells the binder from the pattern
   binding's own type, which in a specialized `unwrap` body is still the erased
   `A`").
2. **Route the binder through the carrier bridge** (`emit_carrier_bridge`,
   `emit_localvar_record_ctype`) the way the `any` production seam does, so the
   binder records `tur_tagged_t` as its representation and every read of `x`
   goes through the side table rather than the declared kind.

Direction 1 first: it removes the mismatch rather than bridging around it, and
the layout side proves the substituted type is reachable at emit time.

## Not this bug

`(Vec any)` and `(Map int any)` are fine -- the stdlib collections box their
elements through `malloc`, so their element slot is a pointer and never a
by-value `tur_tagged_t` field. This is specifically a USER-DEFINED `defdata`
whose constructor field is the type parameter, instantiated at `any`.

The Saffron seam is also not this bug. It is fixed
(`tests/fixtures/saffron-unannotated-container-param`), and the failure here
reproduces with no dialect involved.
