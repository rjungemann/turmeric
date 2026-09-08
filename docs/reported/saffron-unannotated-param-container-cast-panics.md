---
title: A Saffron function taking a container in an unannotated parameter panics on the compiled path
category: Reported
description: "`(defn pick [v] (vec-get v 0))` in a Saffron file panics `cast: any holds a different instantiation of Vec` compiled, and works under --interpret. The D5 boundary cast exempts a bare TY_TYVAR expected type as having 'nothing to check against', but `(Vec A)` is a TY_APP whose ARGUMENT is a tyvar, so it slips through and checks against an instantiation nobody chose."
---

# An unannotated Saffron parameter holding a container panics compiled

**Severity: high.** Passing a vector to a function is the most ordinary thing a
program does, and in Saffron the parameter is unannotated by design -- that is
the whole point of the dialect. The compiled path panics; the interpreter is
correct. So this is a back-end divergence AND a crash on the path Saffron ships
on second.

Found while writing S7's fixtures: the reverse-direction probe panicked, and
narrowing it removed the module boundary entirely.

## Repro

A single Saffron file. No modules, no imports.

```turmeric
#lang saffron
(defn pick [v] (vec-get v 0))
(defn main []
  (let [v [7 8]]
    (println (pick v)))
  0)
```

```
$ tur run p.tur
panic at ...: cast: any holds a different instantiation of Vec

$ tur --interpret p.tur
7
```

## Root cause

D5's boundary inserts a checked `cast` at each argument whose static type is
`any` and whose parameter type is CONCRETE (`elab_call.c`). Its guard reads:

```c
if (!arg_ok && args[i] && args[i]->type.kind == TY_ANY &&
    lang_span_is_saffron(args[i]->span) &&
    expected_arg_kind != TY_ANY && expected_arg_kind != TY_TYVAR &&
    expected_arg_kind != TY_UNKNOWN) {
```

with the comment: *"a callee expecting `any` needs no check, and one expecting a
type variable has nothing to check against."*

That reasoning is right and its implementation is one level too shallow.
`vec-get` is `[A] [v : (Vec A) i : int]`, so the expected type is `(Vec A)` --
kind `TY_APP`, not `TY_TYVAR`. The guard passes, and the seam checks the `any`
against a `Vec` instantiation that the tyvar never determined. The value is a
`(Vec any)` (from the S6 literal widen); the target is some other instantiation;
the check fails.

The interpreter does not diverge here by accident: it HEAD-matches parametric
targets on purpose (`turi_any_target_name`, which records the trade -- compiled,
`(is? x (Option int))` on an `(Option float)` is false; interpreted it is true).
So the interpreter never compares the instantiation and never sees a mismatch.

Nothing caught it because every Saffron fixture that touches a container either
annotates the parameter (`stdlib/saffron/prelude.tur` declares
`v : (Vec any)`) or calls the container accessor inline in `main`, where the
receiver's type is known statically.

## Fix directions

1. **Extend the exemption to an application whose type ARGUMENTS are not
   determined.** The same sentence the guard already carries -- "nothing to
   check against" -- applies to `(Vec A)` exactly as it does to `A`. Smallest,
   and it makes the guard say what it means. What it leaves open is what
   happens next: with no cast inserted, the `any` argument still has to satisfy
   a `(Vec A)` parameter, so this needs pairing with whatever makes that
   check succeed.
2. **Ground the undetermined arguments to `any`.** In a Saffron file an
   undetermined container element type IS `any` -- that is D3's default and what
   the S6 literal widen produces -- so `(Vec A)` becomes `(Vec any)`, the check
   succeeds, and the seam keeps its contract.

   **ATTEMPTED 2026-09-08, and REVERTED: it is necessary but not sufficient,
   and alone it turns the panic into a WRONG ANSWER.** Grounding works -- a
   probe confirms the seam then inserts exactly `(Vec any)`, byte-identical to
   what a hand-written `(cast v (Vec any))` produces -- and the panic goes away.
   The program then prints the element's BOX ADDRESS instead of its value:

   ```
   (println (type-of (pick v)))   ; int   <- tag is right
   (println (cast (pick v) int))  ; 94242104267456   <- payload is the box
   ```

   A wrong answer is worse than a panic, so this must not ship on its own.

   See "Why grounding is not enough" below for the exact cause; the short
   version is that the seam's substitution is not visible to the result-tyvar
   grounding, so the call's result collapses to the int64 carrier and the
   return position then widens the BOX POINTER into an `any` instead of reading
   through it.
3. **Head-match parametric targets on the compiled path**, as the interpreter
   does. Closes the divergence from the other end and is the larger change: it
   weakens `cast` for every program, not just Saffron ones, and the archived
   note on `turi_any_target_name` argues the compiled strictness is the
   better half of that trade.

## Why grounding is not enough -- measured

Both forms in ONE file, so the comparison is exact:

```turmeric
#lang saffron
(defn pick-seam     [v] (vec-get v 0))                    ; wrong answer
(defn pick-explicit [v] (vec-get (cast v (Vec any)) 0))   ; correct
```

The emitted bodies are IDENTICAL except the return:

```c
static tur_tagged_t pick_hyseam(tur_tagged_t v) {
    int64_t __ps_208 = (vec_hyget(... __tur_any_cast_check(...) ...));
    return TUR_TAG(3, (int64_t)(intptr_t)(__ps_208));      /* widen the BOX PTR */
}
static tur_tagged_t pick_hyexplicit(tur_tagged_t v) {
    int64_t __ps_209 = (vec_hyget(... __tur_any_cast_check(...) ...));
    return (*(tur_tagged_t *)(intptr_t)(__ps_209));        /* deref the box */
}
```

Same cast, same `vec_get` call, same argument. The seam form emits a
RETURN-POSITION WIDEN (`elab_coerce_to_any`, tag 3 = int); the explicit form
does not, because its body type is already `any` and the boxed `(Vec any)`
element is read through instead.

So at return-inference time the seam form's body type is NOT `any`. A probe on
`elab_call` shows `vec-get` calls resolving both ways in the same compile --
`result=tyvar arg0=(type-app Vec tyvar)` and `result=any arg0=(type-app Vec
any)`. The reading that fits: the seam replaces `args[i]`, but the RESULT
tyvar `A` is grounded from bindings established before that substitution, so
`A` stays open, the result collapses to the int64 carrier
(`call_result_type`'s bare-tyvar collapse, which exempts `any`/`union` but not
an ungrounded tyvar), and the return widen then tags the box POINTER.

**So direction 2 needs the result-tyvar grounding to see the substituted
argument** -- either by re-grounding after the seam fires, or by moving the
seam ahead of whatever builds those bindings. That is a change in the tyvar
grounding machinery, not in the seam, which is why it was reverted rather than
pushed through at the end of a session.

## Not this bug

An ANNOTATED parameter is fine: `stdlib/saffron/prelude.tur`'s
`vec-map [v : (Vec any) ...]` works on both back ends, which is why the prelude
fixtures pass. So does an explicit hand-written `(cast v (Vec any))` --
measured, in plain Turmeric with no Saffron involved. The defect is specifically
the cast the SEAM chooses when the parameter's type arguments are open.
