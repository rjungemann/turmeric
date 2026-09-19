---
title: A plain (non-^fat) fn-typed parameter accepts any function at all
category: Reported
description: A parameter declared `(fn [int] int)` accepts `(fn [cstr] cstr)` with exit 0. Call arguments compare TY_FN to TY_FN by kind; the structural check added for ^fat parameters (stdlib-int-stand-in-audit S1) reaches only them, because only ^fat records arg_full_types.
---

# A plain (non-`^fat`) fn-typed parameter accepts any function at all

**Severity: medium.** Silent: the wrong-shaped function is accepted, and what
happens next depends on the ABI rather than on the declared type.

**Status:** OPEN. Filed 2026-09-18 while executing
[stdlib-int-stand-in-audit](stdlib-int-stand-in-audit.md) S1, which closed the
same hole for `^fat` parameters only.

## Repro

```turmeric
(defn takes-plain [f : (fn [int] int)] : int 0)
(defn main [] : int (takes-plain (fn [a : cstr] : cstr a)) 0)
```

`tur check` exits 0. Arity mismatches are accepted too.

The `^fat` sibling is now rejected, which is the whole of the difference:

```turmeric
(defn takes-fat [^fat f : (fn [int] int)] : int 0)
;; error [TUR-E0001]: function 'takes-fat' arg 1: expected a function of type
;;   (fn [int] : int), got (fn [cstr] : cstr)
```

## Root cause

`elab_call.c` computes `arg_ok` as a KIND compare, so `TY_FN` satisfies `TY_FN`
whatever the shape. S1 added `fn_type_structurally_compatible` (`types.c`) at
the call site, but it is reached through the LT2 block, which reads the
expected type out of `fn_type.as.fn.arg_full_types[i]` -- and
[types.h:681](../../src/compiler/types.h) says that array is
"NULL for monomorphic args, non-NULL for poly". A `^fat` parameter records one;
an ordinary `(fn [int] int)` parameter does not, so there is nothing to compare
against and the check returns early.

Note `fn_type_subtype`, the pre-existing LT2 helper, asserts in its own comment
that "arity mismatch caught elsewhere". It is not caught anywhere.

## Fix direction

Record the declared full type for a monomorphic fn-typed parameter as well, so
the existing structural check sees it. The predicate itself needs no change --
it already compares carrier class rather than exact kind, and skips
tyvar/unknown/any slots, which is what kept its blast radius at zero once those
two refinements were in (59 -> 3 -> 0 regressions across the S1 iterations).

The risk is entirely in widening `arg_full_types`: that array is load-bearing
for the rank-2 / poly paths, so the change is "also populate it for the
monomorphic case", not "repurpose it".
