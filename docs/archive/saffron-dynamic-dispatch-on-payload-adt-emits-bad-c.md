# Dynamic typeclass dispatch on a payload-carrying ADT emits uncompilable C

**RESOLVED 2026-09-14.** The non-witness shim in `emit_instance_dyn_table`
(`src/compiler/emit_stmt.c`) was missing one arm. `dict_slot_param_is_carrier`
DECLINES a parameter the C convention passes as `const T *` -- that is its
pass-by-ptr arm, and it is correct -- after which the conversion chain fell
straight through to the scalar `(T)__r` at the bottom, which is what GCC
rejected. The arm now sits between them and spells the receiver
`(const T *)(intptr_t)__r`, matching the three-way choice the WITNESS path a
few lines above already makes; the widen boxed the value, so `__r` is already
the ADT's address.

The fix directions filed below named the right predicate
(`emit_type_is_byvalue_adt`) and the right precedent (the dict wrapper), so
nothing here was a wrong turn -- but note the defect was not "the by-value path
is missing", it was "the by-value path is there and one SIZE of by-value ADT
routes around it". That is why an all-nullary `defdata` worked and a
single-int-payload one worked too: both ride the int64 carrier. Only a
receiver large enough to be passed by pointer took the broken arm, which is a
narrower trigger than the report's "carries a payload" and worth knowing if a
neighbouring shim turns up with the same shape.

A `defstruct` receiver was measured alongside and was never affected.

Pinned by `tests/fixtures/saffron-dyn-dispatch-payload-adt`, which carries all
three shapes (pass-by-ptr ADT, carrier-shaped ADT, all-nullary ADT) plus a
primitive instance through one class and one dispatch site. The Saffron guide's
"what dynamic dispatch does not cover yet" list, which did not mention this
case, now says explicitly that it is not among them.

---

**Severity: high -- build breaker.** A `.method` call on an un-narrowed `any`
holding an ADT works when the ADT's constructors are nullary, and fails to
compile when any constructor carries a payload: the emitted dispatch shim
`__dynshim_<Class>_<method>_<Type>` does a scalar cast to a by-value struct.
Compiled back end only -- the interpreter answers correctly.

The Saffron guide's "What dynamic dispatch does not cover yet" list does not
include this case, and its own `.name-of` examples are on primitives, so
nothing caught it.

Found writing `docs/guides/introducing-saffron.md`; it is why that guide
demonstrates dynamic dispatch over an ADT whose constructors are nullary.

## Repro

```turmeric
#lang saffron
(defdata Shape (Circle :float) (Rect :float :float))
(defclass Describe [a] (label [x] : cstr))
(definstance Describe [Shape]
  (label [x] (match x (Circle r) "circle" (Rect w h) "rect")))
(defn tell [x] (println (.label x)))         ;; x is `any`
(defn main [] (tell (Circle 2.5)) 0)
```

```
In function '__dynshim_Describe_label_Shape':
error: conversion to non-scalar type requested
warning: control reaches end of non-void function
tur: cc invocation failed (status 256)
```

`tur --interpret` on the same file prints `circle`.

## Contrast -- nullary constructors are fine

```turmeric
(defdata Animal (Dog) (Cat))
(definstance Speaks [Animal] (speak [x] (match x (Dog) "woof" (Cat) "meow")))
(tell (Dog))    ;; => woof, compiled and interpreted
```

which is the difference: an all-nullary `defdata` rides the int64 carrier, so
the shim's scalar cast is correct for it and wrong for everything else.

## Fix directions

The shim is generated where the dynamic method table is emitted
(`__dynshim_` in `src/compiler/emit_module.c`). It needs the by-value ADT
path the ordinary call site already uses -- `emit_type_is_byvalue_adt` is the
predicate, and `saffron-lang-plan` section 5's dict discussion works through
the same box/no-box decision for the typeclass dictionaries. Unboxing through
a `tur_adt_<T> *` (as the dict wrapper at plan line 588 does) rather than a
scalar cast is the shape.
