---
title: A typeclass method dispatched on an `any` receiver emits uncompilable C -- including via the `@TypeName` witness the diagnostic recommends
category: Reported
description: When instance resolution pins an instance, nothing checks or bridges the receiver's representation. With exactly one instance the ambiguity guard never fires and the tagged box is passed straight to a concrete-typed instance function; with an explicit @TypeName witness the same thing happens even though two instances exist. Two symptoms, one root cause. The two-instance-no-witness case is diagnosed correctly, so the guard exists -- it is just not reached.
---

# Typeclass dispatch on an `any` receiver emits uncompilable C

**Severity: medium.** No miscompile -- the build fails -- but it fails as a
`cc` error against generated code, and in the `@TypeName` case it fails while
the programmer is doing exactly what the compiler's own diagnostic told them
to do.

Found while scoping how far the "typeclass methods need a static receiver"
limitation actually reaches for
[docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md) (D8).
It matters beyond Saffron: `any` is a shipping type, and dispatching a method
on one is a reasonable thing to try.

## Root cause

When instance resolution settles on an instance, the receiver is passed to the
concrete instance function with no check that its *representation* matches. An
`any` receiver is a `tur_tagged_t`; the instance function expects the payload
type. Nothing bridges the two, and nothing rejects the call.

The guard for this exists and works -- it is just not reached in either shape
below. With two instances and no witness, the receiver's erasure IS detected:

```
error []: ambiguous method dispatch: '.tag-of' matches 2 instances
  (Tag[?], Tag[?]) -- receiver type is erased (int64_t).
  Hint: annotate the receiver's type or use @TypeName syntax (see D1).
```

## Symptom 1 -- exactly one instance bypasses the guard

```turmeric
(defclass Shape [a] (area [x : a] : float))
(defstruct Circle [r : float])
(definstance Shape [Circle] (area [x : Circle] : float (* 3.14159 (* (.r x) (.r x)))))

(defn dyn-area [x : any] : float
  (area x))                       ;; no narrowing; receiver is `any`

(defn main [] : int (println (dyn-area (make-struct Circle 2.5))) 0)
```

```
$ tur run n2.tur
error: incompatible type for argument 1 of '__inst_Shape_area_Circle'
 7298 |  double __ps_180 = (__inst_Shape_area_Circle(x));
      |                                              ^  tur_tagged_t
 note: expected 'tur_adt_Circle' but argument is of type 'tur_tagged_t'
tur: cc invocation failed (status 256)
```

With one candidate there is no ambiguity to report, so resolution takes it
without ever asking whether the receiver is a `Circle` -- and it is not, it is
a box that might hold one.

## Symptom 2 -- the `@TypeName` witness has the same hole

The witness (`elab_typeclasses.c:5563`, Phase D1) pins dispatch to a named
instance. It is what the ambiguity diagnostic above tells the programmer to
reach for. Following that advice:

```turmeric
(defclass Tag [a] (tag-of [x : a] : cstr))
(definstance Tag [int]  (tag-of [x : int]  : cstr "I-AM-INT"))
(definstance Tag [bool] (tag-of [x : bool] : cstr "I-AM-BOOL"))

(defn dyn-tag [x : any] : cstr
  (tag-of @bool x))               ;; the witness the hint recommends
```

```
$ tur run n4.tur
error: incompatible type for argument 1 of '__inst_Tag_tag_hyof_bool'
 note: expected '_Bool' but argument is of type 'tur_tagged_t'
tur: cc invocation failed (status 256)
```

The witness correctly selects `Tag[bool]` and then hands it the box. So the
one escape hatch built for "the receiver's type is erased" does not work on
the erasure that `any` produces.

## What already works (and why the fix is small)

Two other routes through the same wall compile and run correctly today, which
is what makes this look like a gap rather than a design limit:

```turmeric
;; is?-guard narrowing -- the guard refines x to Circle in the then-branch
(defn dyn-area [x : any] : float
  (if (is? x Circle) (area x)
    (if (is? x Square) (area x) 0.0)))     ;; => 19.6349 / 50.41

;; explicit checked cast
(defn dyn-area [x : any] : float (area (cast x Circle)))   ;; => 19.6349
```

Both work because they produce a value whose static type is the payload type,
not the box. So the machinery to bridge an `any` into an instance call exists
and is exercised -- it is simply not applied at the two sites above.

## Fix directions

1. **Symptom 2 is the valuable one: make `@TypeName` on an `any` receiver
   imply the unbox.** The witness already names the target instance, which is
   exactly the information `cast` needs, so `(tag-of @bool x)` can lower to the
   dispatch plus the same checked unbox `(cast x bool)` emits -- including the
   runtime tag check, so a wrong witness panics with the existing
   `cast: any holds ...` message rather than reinterpreting the payload. This
   makes the compiler's own hint true, and turns a one-token annotation into a
   complete answer for the erased-receiver case.

2. **Symptom 1: check the receiver, do not just count candidates.** Resolution
   should reject (or, per (1), bridge) a receiver whose representation is not
   the instance's type, independently of how many instances matched. One
   candidate is not evidence that the candidate is right.

3. Whichever lands, the diagnostic for the un-narrowed, un-witnessed case
   should name the two routes that do work -- `is?` narrowing and `cast` --
   rather than only `@TypeName`.

Related but a different site: the mode-B dict-clone body has its own version
of "erased argument, concrete-typed callee", filed as
[forall-dict-byvalue-receiver-emits-uncompilable-c](forall-dict-byvalue-receiver-emits-uncompilable-c.md).
Fixing either does not fix the other; both are instances of the same missing
question, "does the representation match?"
