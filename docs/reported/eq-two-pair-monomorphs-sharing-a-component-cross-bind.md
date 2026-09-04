---
title: Two Pair monomorphs sharing a component cross-bind their per-component Eq dispatch
category: Reported
description: A (Pair (Option String) int) and a (Pair (Option String) (Option String)) each work alone, but in ONE program the Eq[Pair] spec bodies cross-contaminate -- the .snd dispatch of one binds the other's instance and receives the first component's unboxed payloads. cc warns, the program crashes.
---

# Two Pair monomorphs sharing a component cross-bind their per-component Eq dispatch

**Severity: medium** -- a crash on the DEFAULT path for a shape `tur check`
accepts.  Found 2026-09-04 while pinning
[eq-on-pair-of-niche-option-segfaults](../archive/eq-on-pair-of-niche-option-segfaults.md);
that report's fix is **not** this, and this is what its own "not yet probed"
section anticipated.

## Repro

Each `let` block below compiles and answers correctly **on its own**.  Together
in one program they do not.

```turmeric
(load "stdlib/string.tur")
(load "stdlib/pair.tur")
(defn main [] : int
  (let [a (:: (some (string/from-cstr "aa")) (Option String))
        b (:: (some (string/from-cstr "aa")) (Option String))]
    (let [p1 (:: (pair a 1) (Pair (Option String) int))
          p2 (:: (pair b 1) (Pair (Option String) int))]
      (println (if (eq? p1 p2) "g1-eq" "g1-ne")))
    (let [q1 (:: (pair a a) (Pair (Option String) (Option String)))
          q2 (:: (pair b b) (Pair (Option String) (Option String)))]
      (println (if (eq? q1 q2) "g2-eq" "g2-ne")))
    0))
```

```
warning: passing argument 1 of '__inst_Eq_eq_qu_int' makes integer from
         pointer without a cast [-Wint-conversion]
  bool __ps_336 = (__inst_Eq_eq_qu_int(
      (__t334 ? (void *)(intptr_t)tur_opt_value_checked(__t334) : (void *)0), ...
```

Read that call: inside the `(Pair (Option String) int)` spec, the **`.snd`**
comparison -- which should be `Eq[int]` over two plain integers -- is being
handed the **first** component's unboxed Option payloads.  The two per-component
dispatches of one `Eq[Pair]` body have been crossed.

The two monomorphs share their FIRST component (`(Option String)`) and differ
in the second.  Neither `(Pair (Option String) int)` nor
`(Pair (Option String) (Option String))` alone reproduces; a `(Pair (Option
String) int)` beside a `(Pair (Vec int) int)` -- which shares nothing -- does
not either.

## Relationship to the fix that found it

`eq-on-pair-of-niche-option-segfaults` was a different defect at a different
site: the nested-dispatch spec path forced only parameter 0 to the resolved
receiver, so a binary method's second parameter kept the erased carrier and one
`Eq[Option]` spec came out `(void *, int64_t)`.  That is fixed, and this repro
is measurably better for it -- 5 `-Wint-conversion` warnings before, 2 after --
but "better" is not "fixed", and the program still dies.

Stating that explicitly because the two are easy to conflate: they are both
"an `Eq[Pair]` spec is minted wrong", they surface within minutes of each
other, and the first fix moves this repro's numbers without closing it.

## Root cause

Not established.  What is known:

- It needs TWO `Pair` monomorphs in one program that SHARE a component type.
- The damage is in the spec BODY, not just the key: the emitted
  `(Pair (Vec int) int)` body in the larger probe contained
  `tur_opt_value_checked` calls, which nothing in a Vec/int pair should
  produce.  So a body is being reused or re-instantiated against the wrong
  element bindings, rather than merely named wrong.
- `Eq [Pair]`'s body is `(and (eq? (.fst x) (.fst y)) (eq? (.snd x) (.snd y)))`
  -- two dispatches on the same receiver at different field positions, which is
  the structure that distinguishes it from every container whose instance
  dispatches once.

The element-binding vector (`eb`/`enb`) threaded into the nested-dispatch spec
minting is the thing to look at: if it is keyed by the receiver monomorph
rather than by the field position, two Pair monomorphs sharing a first
component would resolve the second component's dispatch through each other's
bindings, which is exactly the observed shape.

## Detection

`tests/run.sh` already FAILs any fixture whose build stderr carries
`-Wint-conversion`, so a fixture for this shape is all the guard needs.  One is
deliberately NOT added yet: it would be a red fixture pinning broken behaviour.
`tests/fixtures/eq-nested-binary-class-method-spec` carries a comment pointing
here and explains why the `(Pair (Option String) (Option String))` group is
absent from it.

## Guides to update when fixed

- None known; a codegen defect with no documented behaviour attached.
