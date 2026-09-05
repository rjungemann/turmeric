---
title: StThunk -- the opaque :ptr<void> carrier for a Stream thunk, removed
category: Archive
description: Row 1 of workarounds-to-remove. stdlib/logic.tur carried (defopaque StThunk :ptr<void>) plus a cast at every construction and force site because a :fn field once segfaulted. The blocker had moved twice by the time it was struck, and the thing actually keeping it alive was neither of the two recorded reasons.
---

# `StThunk` -- the opaque carrier for a `Stream` thunk

**REMOVED 2026-09-05.** `stdlib/logic.tur` now declares the field with its real
type:

```turmeric
(defdata Stream :copy (StNil) (StCons :Subst :Stream) (StInc (fn [] Stream)))
(defn st-force [th : (fn [] Stream)] : Stream (th))
```

and the `(:: ... :StThunk)` wrappers are gone from all four construction sites.

## The blocker moved twice, and neither recorded reason was the live one

The row in `workarounds-to-remove` recorded two successive reasons:

1. **Original:** a bare `:fn` field accepted a capturing closure, type-checked,
   and segfaulted when forced. Resolved and archived
   (`closure-in-defdata-field`) -- a bare `:fn` field now refuses a capturing
   closure at compile time, and a field declared with its full signature stores
   one correctly.
2. **Probed 2026-09-02:** "the honest declaration is `(StInc (fn [] Stream))`,
   and every construction site then fails `TUR-E0295: cannot reinterpret
   by-value aggregate 'Stream' as a one-word carrier`."

Reason 2 is close but wrong in the detail that matters. Probing it directly:

- A `:fn` field returning a by-value aggregate works --
  `(defdata Th (Th (fn [] Box)))` with a capturing lambda runs.
- So does the SELF-RECURSIVE version, `(defdata Stream ... (StInc (fn [] Stream)))`
  with `(StInc (fn [] : Stream (StCons n (StNil))))`.
- Making the change in `logic.tur` fails at **exactly one** site, not "every
  construction site": `st-bind`'s

  ```turmeric
  (st-append (:: (f v) :Stream) (st-bind rest f))
  ```

  where `f : fn` is an UNTYPED fn parameter, so `(f v)` yields the erased int64
  carrier and the ascription reinterprets it to a by-value aggregate. That line
  is unchanged by the field declaration; it starts failing because `Stream`
  stops being carrier-eligible once it holds a fn field.

So what kept the workaround alive was a **second type-eraser two functions
away** -- `f : fn` -- not the `:fn` field at all. Typing it
(`f : (fn [Subst] Stream)`, and `mbind` likewise) removes the ascription and the
error with it. That is the CLAUDE.md rule applied to the thing the rule is
about: the bare `:fn` was standing in for a real function type.

## And then one compiler fix

With both erasers gone the program type-checks and reaches codegen, where
`st-bind`'s `(let [lf f] ...)` alias tripped a `-Wint-conversion`:

```c
int64_t (*lf)(int64_t) = (int64_t (*)(int64_t))(intptr_t)(f);
__t258->lf = lf;                  /* int64_t = int64_t (*)(int64_t) */
```

A fn-typed PARAMETER arrives as the fat-closure handle (`int64_t f`, read as
`(*(thunk_t *)f)(f, ...)` everywhere else), so declaring the alias as a thin
function pointer and then storing it in an `int64_t` env field is a straddle --
a FAIL under the suite's cc ratchet and a hard error under GCC >= 14. Fixed in
`emit_expr.c` by carrying the alias as the handle, mirroring the
curried-closure arm beside it; the elaborator already did this for the ASCRIBED
spelling and a plain alias had no counterpart.

The ten `logic-*` fixtures are the regression coverage, verified by reverting
the emit fix and watching them trip the ratchet.

## What this cost, and the lesson

Three layers, each hidden behind the last: an opaque carrier, hiding an untyped
`:fn` parameter, hiding an emitter gap. Each layer's note described the layer
below it as the blocker and was wrong about it, because nobody had removed the
top layer to look. **A workaround's recorded blocker is a hypothesis until
someone takes the workaround out.** Taking it out cost one afternoon; the row
had carried a wrong blocker for three days and a superseded one for longer.

Two adjacent defects surfaced while writing a regression fixture for the emit
fix and are filed separately:
[let-alias-of-fn-param-captured-in-lambda](../reported/let-alias-of-fn-param-captured-in-lambda.md).
