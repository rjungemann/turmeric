---
title: A closure that captures cannot be recovered from an `any` on the compiled back end, though the interpreter can
category: Reported
description: A fat `{thunk, env}` closure -- a lambda over an outer binding, or a partial application -- interns a different `any` box id from a bare fn of the same signature, so `is?` is false and `cast` panics. That is deliberate (calling one as the other segfaulted), but it means no capturing closure round-trips through `any` compiled, while the interpreter has no fat/bare split and does it fine. A capability divergence, not a wrong answer.
---

# A capturing closure cannot come back out of an `any` (compiled)

**Severity: medium.** Nothing miscompiles and nothing crashes -- the compiled
side says so with a panic. But it is a genuine capability gap, and it is a
portability trap in the worse direction: the program works under `--interpret`
and panics compiled.

Filed as the residue of
[partial-application-widened-to-any-is-a-ptr](../archive/partial-application-widened-to-any-is-a-ptr.md),
which fixed the reflection half (a partial application reports as a function on
both back ends now, instead of "ptr" compiled and "fn" interpreted) and closed a
segfault, but deliberately stopped short of this.

## Repro (2026-09-07)

```turmeric
(defn add [x : int y : int] : int (+ x y))
(defn capturing [n : int] : any (fn [x : int] : int (+ x n)))
(defn curried   []        : any (add 1))

(defn main [] : int
  (println (if (is? (capturing 1) (-> int int)) 1 0))
  (println ((cast (capturing 1) (-> int int)) 41))
  0)
```

```
$ tur run p.tur
0
panic: cast: any holds a function this cast cannot accept -- a different
       signature, or a closure that captures where a plain function is required

$ tur --interpret p.tur
1
42
```

A NON-capturing lambda round-trips fine on both paths -- that is the case
`any-fn-tag-does-not-discriminate-signatures` kept working, and it is the whole
of what works today.

## Why it is currently correct to refuse

A bare fn payload is a code pointer; a capturing one is a fat `{ thunk, env }`
handle invoked through slot 0. `cast` emits its call from the **target** type,
and a target written `(-> int int)` is bare -- so before the two got distinct
box ids, a fat payload passed the tag check and the call ran a closure box as
though it were code. That segfaulted. Refusing is strictly better than that, and
the interpreter is unaffected because it has no representation split to get
wrong.

## Fix direction: fatten every fn payload at the widen

Make the representation uniform instead of discriminating between two of them.

1. In `elab_coerce_to_any`, wrap a bare `TY_FN` payload in `EX_FN_TO_FAT` (the
   existing auto-shim, already used for fat struct fields and rank-2 slots) and
   mark the widened type `boxed`. Every fn inside an `any` is then fat.
2. In `any_narrow_target`, mark a fn target `boxed` too, so the ids line up and
   the cast's call site emits the fat-protocol call.

Then a capturing closure, a partial application and a plain lambda all behave
the same through `any`, and the fat/bare id split added for safety becomes
unnecessary -- there is only one representation to name.

**The open question is ownership**, and it is why this was not folded into the
report above: step 1 mallocs a fat box per widen of a bare fn, and that box needs
an owner or it is a leak per widen. The `any` drop machinery
(`any-struct-box-leak-per-widen`) already models exactly this question for a
by-value struct payload -- frame-box in argument position, scope-exit drop for a
local, and the escape analysis that decides between them -- so the shape of the
answer exists; it has to be extended to a fn payload and re-verified under
LeakSanitizer. That is a change to the representation of every `any`-boxed
function and deserves its own leak analysis rather than riding along.

Assert **both back ends** in the fixture when it lands: the interpreter already
gets this right, so the compiled path catching up is the whole point, and
`tests/fixtures/any-closure-capture-not-a-bare-fn` carries `requires.compiled`
today precisely because they differ. That marker should come off.
