---
title: A `(Vec any)` builds and runs, but its element type does not flow back out and its `vec-new` spec dedups against a sibling
category: Reported
description: With the repr-decision ICE fixed, a (Vec any) can be constructed, pushed into and freed correctly -- but `vec-get` reports its result as `int` rather than `any`, so an element cannot be read back with its tag; and a program containing both a (Vec any) and another vec-of emits one -Wincompatible-pointer-types warning from a deduped vec-new spec.
---

# `(Vec any)` is buildable now, and half-plumbed

**Severity: medium.** Neither half is a miscompile -- the values stored are
correct, the element boxes are freed, and the warning is over two structurally
identical types -- but the first half makes a `(Vec any)` write-only, which is
most of the point of having one.

Residue of
[vec-of-any-repr-decision-ice](../archive/vec-of-any-repr-decision-ice.md),
which is fixed: `(Vec any)` no longer aborts the compiler. These are the two
things that fix did not reach, both found by measuring what the newly-buildable
type actually does.

## Half 1 -- `vec-get` on a `(Vec any)` returns `int`

```turmeric
(defn dyn [x : any] : any x)
(defn main [] : int
  (let [v (:: (vec-new) (Vec any))]
    (vec-push! v (dyn 7.1))
    (println (type-of (vec-get v 0))))
  0)
```

```
error: 'type-of' expects an 'any'-typed argument, got 'int'
```

Identical on both back ends, so this is elaboration, not codegen. The element
type does not flow out of the container: `vec-get [A] [v : (Vec A) i : int] : A`
should ground `A` to `any` from the receiver and does not.

Ascribing past it does NOT recover the element -- it makes it silently wrong,
which is the part worth knowing:

```turmeric
(println (type-of (:: (vec-get v 0) any)))   ; compiled: int   interpreted: float
```

That ascription is a WIDEN of an `int`-typed expression, so it re-tags the
carrier word with a statically-chosen tag rather than reading the element's own.
The two back ends pick different tags, which is how the shape announces itself.
The ascription is behaving correctly in isolation; what is wrong is the type it
is handed.

This is S6's subject (containers of `any` are that stage's whole point) and it
should be fixed there rather than as a local patch, since the same question --
"how does an element type reach a read through a container" -- governs
`#map{...}`, `#set{...}` and cons lists too.

## Half 2 -- a deduped `vec-new` spec, and one cosmetic warning

A program containing a `(Vec any)` AND any other `vec-of` emits:

```
warning: returning 'tur_adt_Vec__int *' from a function with incompatible
         return type 'tur_adt_Vec__any *' [-Wincompatible-pointer-types]
```

Either `vec-of` alone is warning-free; so are two `vec-of`s at different
CONCRETE element types (`(vec-of "a")` + `(vec-of 7.1)`). It takes an `any`
beside another element type.

The cause is inside `vec-empty-like__`, whose body is `(:: (vec-new) (Vec A))`.
Its own clones stay distinct (their argument types differ, `tur_tagged_t` vs
`int64_t`), but the nested `vec-new` -- zero arguments, result `(Vec A)` --
resolves to the `int` instantiation for both. The dedup family is the one
`emit_abi_intern_spec`'s `match_bindings` flag was added for; whether `vec-new`
should be asking for it, or whether the `heap_inline_c_producer` block above it
is flattening the result type before the comparison, is not established here.

**Measured cosmetic, not asserted.** The two monomorphs have identical layout:

```c
typedef struct tur_adt_Vec__any { void *data; int64_t len; int64_t cap; } ...;
typedef struct tur_adt_Vec__int { void *data; int64_t len; int64_t cap; } ...;
```

so the pointer is the same pointer under two names. The fixture that provokes
it (`tests/fixtures/vec-of-any-builds`) prints correct results and is
leak-clean. It is still a warning in a user's build, which this project treats
as something to close rather than tolerate.

## Fix directions

1. **Half 1, in S6**: ground a container read's element tyvar from the
   receiver's monomorph. Worth doing once for every container rather than for
   `Vec` alone.
2. **Half 2**: determine which of the two candidates above collapses the
   `vec-new` spec, then either request binding-matching for it or stop the
   result-type flattening from erasing `(Vec any)` to `(Vec int)`. A probe on
   `emit_abi_intern_spec`'s dedup loop naming both candidate specs is the next
   step; reading the two call sites did not settle it.
