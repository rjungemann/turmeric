---
title: `is?` / `cast` on an `any` holding a function match ANY function type, so a wrong-signature cast miscalls silently
category: Archive
description: A function value widened to `any` carries the bare TY_FN TypeKind as its tag, so `is?` compiles to `TUR_GETTAG(v) == 7` and answers true for every function type -- wrong parameter types, wrong arity, wrong result. `cast` then hands back a callable typed however the caller asked, and calling it is undefined behaviour. Compiled and interpreted also disagree on the `is?` answer.
---

# An `any`-boxed function matches every function type

**RESOLVED 2026-09-07.** Fix direction 2 landed, with direction 3 alongside it,
and direction 1 was not needed: the capability the report worried about losing
is preserved intact.

A function payload now interns a **per-signature** `any` box id, exactly the way
a struct/ADT monomorph does. `emit_any_type_id` treats `TY_FN` as a named type
and keys it on the fn type's rendered spelling -- `"(fn [int] : int)"` -- which
carries arity, each parameter's TypeKind, the result kind and the C-ABI bit. P1
had already made those ids a hash of the key rather than a per-TU index, so
nothing else was needed to make them stable across translation units; the
extended `run-any-type-id-multi-module.sh` pins that a fn row minted in one TU
carries the identical id in another.

The `shown` name stays `"fn"` deliberately. The id discriminates signatures; the
*name* is what `type-of` prints, and both back ends already agree on "fn" there
(`type-of-on-boxed-closure-diverges`). Because both sides of a wrong-signature
mismatch then display the same name, `__tur_any_cast_check` grew a third arm:
`"cast: any holds a function of a different signature"`, rather than the
"a different instantiation of fn" the parametric wording would have produced.

**The interpreter was aligned rather than merely documented.** `type_name`'s
`TY_FN` case moved into a shared `tur_fn_type_key(arg_kinds, arity, result_kind,
cfnptr)`, so `eval.c` reconstructs the *same string* from a `TuriClosure`'s
`FnDef` -- full parameter Types and declared return type are both there -- and
compares it to the target's. Two hand-written renderers would have drifted, and
drift here is silently a new compiled/interpreted disagreement.

Both back ends now answer identically on every probe in this report:

| program | before (compiled / interp) | after (both) |
| --- | --- | --- |
| `(is? int->int-fn (-> cstr cstr))` | 1 / 0 | **0** |
| `(is? int->int-fn (-> int int int))` | 1 / 0 | **0** |
| `(is? int->int-fn (-> int int))` | 1 / 0 | **1** |
| `(cast int->int-fn (-> cstr cstr))` | miscall / miscall | **panic** |
| `(cast int->int-fn (-> int int))` then call | 42 / 42 | **42** |
| `(cast 7 (-> int int))` then call | panic / **miscall** | **panic** |

The last row is a second defect the fix swept up: the interpreter's cast switch
had no `TY_FN` arm at all, so a fn target fell into `default: ok = true` and
passed *anything* -- an int typed as a function, then called.

## What is still coarse

Two residuals, both stated in the fixture comments rather than left to be
rediscovered:

- **Parameter types are TypeKinds.** `Type.as.fn.arg_kinds` is a `uint8_t`
  vector of `TypeKind`, so `(-> Pt int)` and `(-> Qt int)` both render
  `"(fn [<adt>] : int)"` and share an id, and `& rest` does not appear in the
  spelling at all. Separating those needs a fn type that carries full parameter
  Types, which is a representation change, not a fix to this seam. Both back
  ends are coarse the same way, so they still agree.
- **A closure that cannot render its signature head-matches.** A native (no
  `FnDef`) and a variadic return NULL from `turi_closure_fn_key`, and the
  interpreter then answers "is this a function at all" rather than guessing. A
  wrong key would be a false negative that silently breaks a type-case;
  head-matching is merely coarse.

One neighbouring defect fell out of the probes and is filed separately:
[partial-application-widened-to-any-is-a-ptr](../reported/partial-application-widened-to-any-is-a-ptr.md)
-- an under-saturated call's static type is `ptr<void>`, so a curried closure
widens as a pointer, `type-of` says "ptr" compiled and "fn" interpreted, and no
`is?` target matches it.

## Fixtures

- `tests/fixtures/any-fn-signature-discriminates` -- wrong types, wrong arity,
  right-arity-wrong-types, the correct signature, and a `cast` round-trip that
  calls the recovered function. No `requires.compiled`: it runs on **both** back
  ends, which is the point.
- `tests/fixtures/any-fn-wrong-signature-cast-panics` -- the undefined-behaviour
  case, now a panic on both paths.
- `tests/run-any-type-id-multi-module.sh` -- a fn payload minted in `producer`
  and narrowed in `main`, with a structural pin that both TUs emit the same id
  for it and that a closure row is `boxed=0`.

---

**Severity: high.** `cast` is documented and implemented as a *checked*
downcast that panics on mismatch. For a function payload it silently does not
check, and the value it returns is then called through a signature it does not
have -- undefined behaviour, reached without a single warning.

Found while fixing
[type-of-on-boxed-closure-diverges](type-of-on-boxed-closure-diverges.md).
Independent of that defect, but **that fix makes this one easier to reach**:
`type-of` on a boxed function now answers `"fn"` instead of `"unknown"`, so a
reader is far more likely to go on and try `is?` / `cast` on it. Filed rather
than fixed because the remedy is a design decision (below), not a correction.

## Repro (2026-09-07, after the type-of fix)

```turmeric
(defn mk [] : any (fn [x : int] : int (+ x 1)))

(defn main [] : int
  (println (if (is? (mk) (-> cstr cstr)) 1 0))
  (let [f (cast (mk) (-> cstr cstr))] (println (f "hi")))
  0)
```

```
$ tur run f3.tur
1                    <- WRONG: an int->int fn "is" a cstr->cstr fn
i                    <- called an int->int function with a const char *

$ tur --interpret f3.tur
0                    <- and the two back ends disagree on the test
91328184806105       <- also miscalls, differently
```

Arity is not checked either:

```turmeric
(is? (mk) (-> int int int))    ;; => 1, on a one-argument function
```

## Root cause

`emit_any_type_id` interns a per-monomorph id only for a **named** type (an ADT
with a def, or a `TY_APP` whose head resolves to one). A function type is
neither, so the box carries the bare `TY_FN` TypeKind, and `is?` lowers to a
comparison against that one number:

```c
TUR_GETTAG(__ps_181) == 7      /* 7 == TY_FN */
```

Every function value in the program has tag 7. So the test is really "is this
*a* function", spelled as though it were "is this *this kind of* function", and
`cast` inherits the same non-check: `__tur_any_cast_check(7, 7)` passes and the
payload is handed back typed as whatever was requested.

The interpreter takes a different route (`turi_any_target_name` /
`turi_any_named_type`) and answers `false` for the same test, so the two back
ends disagree as well -- and its `cast` miscalls too, just with different
garbage.

## Fix directions

1. **Reject a function-typed `is?` / `cast` target.** The box tag cannot
   distinguish signatures, so the honest answer is that the question is not
   decidable -- the same shape as the guards added for a bare type constructor
   (`any-narrowing-broken-for-parametric-receivers`) and a by-value rank-2
   receiver (`forall-dict-byvalue-receiver-emits-uncompilable-c`). Cheap, and
   it converts undefined behaviour into a diagnostic.

   The cost is real and should be stated: `(cast x (-> int int))` with the
   *correct* signature works today and is the only way to get a callable back
   out of an `any`. Rejecting it removes that, so it should land together with
   (2) or with a documented replacement.

2. **Give function boxes a real id.** Intern the function *type* the way
   struct/ADT monomorphs are interned, so `(-> int int)` and `(-> cstr cstr)`
   get distinct ids and both `is?` and `cast` become genuinely checked. This is
   the fix that keeps the capability. It is more work -- `type_name` on a fn
   type has to be a stable identity key, and every widen site has to intern it
   -- but it is the same machinery `emit_any_type_id` already runs for named
   types, and P1 made those ids deterministic and cross-TU stable, so the hard
   part is done.

3. Whichever lands, **align the interpreter**, and add a fixture that asserts
   the same answer on both back ends. The divergence here went unnoticed for
   the same reason the `type-of` one did: nothing compares them on a function
   payload.

Direction 2 is the better outcome and is also a prerequisite for calling an
`any`-held function at all (the dynamic-call work in the Saffron plan's D4), so
it is not throwaway effort.
