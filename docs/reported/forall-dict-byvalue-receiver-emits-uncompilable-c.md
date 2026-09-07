---
title: A mode-B runtime dictionary call on a by-value struct receiver emits uncompilable C instead of TUR-E0311
category: Reported
description: The dict-clone body erases its argument to int64_t but casts the method slot to the instance's concrete signature. For carrier-shaped receivers (int, bool) the function-pointer pun works; for a by-value struct it is an "incompatible type for argument 1" cc error. forall-dict-pass guards its other unsupported shape with TUR-E0311 and a negative fixture; this one has no guard, so it escapes as a raw cc failure.
---

# Mode-B dict dispatch on a by-value struct receiver emits uncompilable C

**Severity: medium.** No miscompile -- the build fails -- but it fails as a
`cc` error against generated code, which is the failure mode
`forall-dict-pass` explicitly set out to avoid. Its sibling unsupported shape
(a method dispatched from inside a nested lambda) is rejected with TUR-E0311
and a negative fixture, and its archive note records the intent as
"never miscompiled"
([forall-dict-pass-nested-lambda-method.md](../archive/history/forall-dict-pass-nested-lambda-method.md)).
This shape has no such guard.

Filed while scoping runtime typeclass dispatch for
[docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md) (D8),
where it is the load-bearing limit on the existing machinery -- see
"Why this matters beyond the diagnostic" below.

## Repro (v0.44.2, `2da89e84`)

```turmeric
(defclass Shape [a] (area [x : a] : float))
(defstruct Circle [r : float])
(defstruct Square [s : float])
(definstance Shape [Circle] (area [x : Circle] : float (* 3.14159 (* (.r x) (.r x)))))
(definstance Shape [Square] (area [x : Square] : float (* (.s x) (.s x))))

(defn poly-area [a] [^Shape a x : a] : float (area x))

(defn use-both [f (forall [a] [(Shape a)] (-> a float))] : float
  (println (f (make-struct Circle 2.5)))
  (println (f (make-struct Square 7.1)))
  0.0)

(defn main [] : int
  (println (use-both poly-area))
  0)
```

```
$ tur run tc3.tur
/tmp/tur-build/..._tc3_tur.c:4682:93: error: incompatible type for argument 1 of
  '(double (*)(tur_adt_Square))*(void **)__dict_1449'
 4682 |  double __ps_40 = (((double (*)(tur_adt_Square))((void **)(intptr_t)__dict_1449)[0])(x));
      |                                                                                     ^
  note: expected 'tur_adt_Square' but argument is of type 'int64_t'
tur: cc invocation failed (status 256)
```

The identical program with `int`/`bool` receivers compiles and runs correctly
-- that is `tests/fixtures/forall-dict-show`.

## Root cause

The dict-clone body reaches the method by slot index through a `void **` cast,
and casts the slot to the method's signature:

```c
/* the working int/bool case, from tests/fixtures/forall-dict-show */
static int64_t poly_hyshow_un_undict_un1444(int64_t __dict_1445, int64_t x) {
    const char *__ps_40 =
        (((const char * (*)(int64_t))((void **)(intptr_t)__dict_1445)[0])(x));
    ...
}
```

Two things make that work, and both are properties of the *carrier*, not of
the design:

- The clone body's parameter `x` is erased to `int64_t`.
- The cast signature is written in terms of `int64_t` too, so calling
  `__inst_Show_show_bool` (really `(bool)`) through a `(int64_t)` pointer is a
  function-pointer type pun that the int64 carrier makes harmless.

For a by-value struct receiver the second property fails: the emitted cast
keeps the *concrete* parameter type (`tur_adt_Square`) while the argument is
still the erased `int64_t`, and C rejects the call.

Note also which instance's type got baked in. The clone body is shared across
instances, but its cast names `tur_adt_Square` -- one arbitrary instance's
signature. Even if the argument types were bridged, a single cast cannot be
right for both `Circle` and `Square`, because they are different by-value
layouts. The shape needs per-instance adaptation, not a different cast.

## Fix directions

1. **Guard it, matching the sibling shape.** Detect a constraint whose method
   takes (or returns) a by-value aggregate at the point mode B decides to build
   a dict-clone body, and emit TUR-E0311 naming the class, the method, and the
   receiver. Add a negative fixture beside
   `forall-dict-pass-nested-lambda-method`'s. This is the small, correct fix
   and should land regardless of whether (2) is ever wanted.

2. **Support it, with per-instance carrier wrappers.** For each
   `(class, method, instance)` whose signature is not carrier-shaped, emit

   ```c
   static double __dictwrap_Shape_area_Circle(int64_t c) {
       return __inst_Shape_area_Circle(*(tur_adt_Circle *)(intptr_t)c);
   }
   ```

   and store the wrapper in the dictionary slot instead of the raw instance
   function. Every slot then has one uniform carrier signature, the clone
   body's cast becomes `(double (*)(int64_t))` for all instances, and the pun
   is honest rather than accidental. The caller must pass a pointer to the
   value for a by-value receiver, which is the same box/no-box decision
   `emit_type_is_byvalue_adt` already makes for `any` widening -- so the
   predicate exists and the two paths should share it.

## Why this matters beyond the diagnostic

Direction (2) is not speculative work: it is precisely what runtime typeclass
dispatch over `any` would need. The Saffron plan (D8) records runtime dispatch
as tractable-but-unscheduled on the grounds that the dictionary plumbing
already exists, and the measurement behind that claim is this same emitted
code -- a dictionary is already an `int64_t` at runtime, and a method is
already reached as `((void **)dict)[slot]`. What is missing there is instance
*selection* (today the caller picks a singleton statically) and exactly this
carrier-wrapper layer.

So (1) closes a real diagnostic hole now, and (2) is a prerequisite that
D8 would have to build anyway. They are not competing fixes.
