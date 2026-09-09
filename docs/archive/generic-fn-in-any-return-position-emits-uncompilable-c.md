---
title: Calling a generic stdlib constructor in an `: any` return position emits uncompilable C -- the monomorph is never instantiated
category: Archive
description: RESOLVED 2026-09-07. Corrected diagnosis: the widen site did NOT fail to request the monomorph -- elaboration was already correct, carrying (Option float) and the A := float binding. emit_abi_scan_expr, the walk that seeds the monomorphization worklist, simply had no case for EX_UNION_INJECT, so a call under a widen was never scanned. Original text: (defn f [] : any (some 7.1)) emits a call to a bare `some` that was never declared or instantiated, so cc reports an implicit declaration returning int and then an incompatible assignment into the box. Writing the ADT constructor directly ((Some 7.1)) works, so this is specific to a generic defn whose monomorph the `any` widen site does not request.
---

# A generic function in an `: any` return position is never monomorphised

**RESOLVED 2026-09-07 -- and, as with P3, the filed diagnosis was wrong in a
useful way.**

The report said "the `any` widen site does not request its `A = float`
monomorph" and proposed making the widen site request it. Elaboration was
already correct. Instrumenting both positions showed the call nodes are
**identical**: same result type `(type-app Option float)`, same single
`A := float` abi_binding, same `wrap_generic_result`. Only emit diverged.

The cause is that `emit_abi_scan_expr` -- the walk that seeds the
monomorphization worklist -- had **no case for `EX_UNION_INJECT`**. A call
sitting under a widen was never scanned, so its specialization was never
minted, and the emitter fell back to the unmangled generic name. Hence
`(defn f [] : (Option float) (mk 7.1))` emitting
`mk__spec__tur_adt_Option__float_double(7.1)` while `: any` emitted
`mk(((union { double s; int64_t d; }){.s = 7.1}).d)` -- a bare call to a symbol
nothing declared, with the argument bit-cast to the carrier.

The fix is four cases in that walk. `EX_UNION_INJECT` is the widen itself and
so covers every position a value is coerced to `any`; the other three
(`EX_ANY_CAST`, `EX_ANY_IS`, `EX_ANY_TYPE_OF`) read a box and can carry a
generic call underneath in exactly the same way.

**Scope was wider than filed**, which the report flagged as unchecked. It was
not specific to the return position, nor to stdlib: argument position
(`(take (some 7.1))` into an `any` parameter) and a user-defined generic
(`mk`) were equally affected. A generic whose result is a *non-parametric*
instantiation (`(defn ident [A] [v : A] : A v)`) always worked -- nothing needs
minting there.

**Fixture:** `any-widen-generic-call-monomorph` -- return position, argument
position, the stdlib repro, and the round trip back out through `is?`/`cast`.
Suites: `run.sh` 2838/0, `run-turi.sh` 1932/0, `run-leak-check.sh` 83/0. No
snapshot churn.

**Two separate defects surfaced while fixing this**, both filed rather than
absorbed:

- [any-coercion-not-driven-by-expected-type](../reported/any-coercion-not-driven-by-expected-type.md)
  -- `(let [x : any 42] ...)` and an `if` under an `: any` return do not widen.
  Reproduces with a plain literal; no generic involved.
- [interp-native-ctor-loses-adt-name](../reported/interp-native-ctor-loses-adt-name.md)
  -- making the compiled side work exposed that `--interpret` answers `"Some"`
  where compiled answers `"Option"`, because the `some` native builds its value
  without the ctor->ADT link. The fixture asserts that value through `cast`
  rather than `type-of` so it stays about monomorphization.

---

## The original report

**Severity: medium.** A build failure, but a `cc` error against generated code
rather than a diagnostic, and the workaround (call the ADT constructor
directly) is not discoverable from the message.

Found alongside
[any-narrowing-broken-for-parametric-receivers](any-narrowing-broken-for-parametric-receivers.md)
while probing how parametric values interact with `any` for
[docs/upcoming/saffron-lang-plan.md](../upcoming/saffron-lang-plan.md).
Different root cause, so filed separately.

## Repro (v0.44.2, `2da89e84`)

```turmeric
(defn boxed-opt [] : any (some 7.1))
(defn main [] : int (println (type-of (boxed-opt))) 0)
```

```
$ tur run h2.tur
warning: implicit declaration of function 'some' [-Wimplicit-function-declaration]
 7301 |  __auto_type __ps_180 = (some(((union { double s; int64_t d; }){.s = 7.1}).d));
      |                          ^~~~
error: incompatible types when assigning to type 'tur_adt_Option__float' from type 'int'
 7303 |  return ({ tur_adt_Option__float *__tur_box = ... *__tur_box = (__ps_180); ... });
tur: cc invocation failed (status 256)
```

The widen site knows the type well enough to name the box
(`tur_adt_Option__float`), but the call it wraps went out as a bare `some`
that nothing declared -- so C defaulted it to `int`-returning and the
assignment into the box failed.

## What distinguishes it

Writing the constructor directly works, and so do user-defined parametric
ADTs -- so neither "parametric" nor "`any`" is sufficient on its own:

```turmeric
(defn boxed [] : any (Some 7.1))            ;; => type-of "Option"    OK
(defdata Box [a] (MkBox a))
(defn boxed [] : any (MkBox 7.1))           ;; => type-of "Box"       OK
```

`some` is `(defn some [A] [x : A] #fx{Construct} : (Option A) (Some x))`
(`stdlib/option.tur:33`) -- a *generic defn*. The `any` widen site does not
request its `A = float` monomorph, so no `some__spec__...` is emitted and the
emitter falls back to the unmangled name.

## Fix directions

1. The widen site should request the callee's monomorph the same way an
   ordinary typed call position does. It already computes the concrete result
   type -- it names the box `tur_adt_Option__float` -- so the instantiation it
   needs is in hand at that point.

2. Failing that, a missing monomorph should be a diagnostic naming the callee
   and the instantiation, not an emitted call to an undeclared symbol. Emitting
   a name the emitter knows it has not defined is the part that turns a
   fixable gap into a `cc` error.

Worth checking whether this is specific to the return position or applies to
any `any` widen of a generic call (a `let` binding annotated `: any`, an
argument to an `any` parameter). The probe above only covers the return
position.
