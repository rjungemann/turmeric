---
title: Two Pair monomorphs sharing a component cross-bind their per-component Eq dispatch
category: Archive
description: The spec lookup's cross-spec fallback keys on the source Expr* alone, but a dict-dispatched call re-resolves its callee per monomorph -- so one sibling spec's recorded clone was adopted for a call to an entirely different function. RESOLVED 2026-09-04: the fallback now requires the clone to belong to this callee.
---

# Two Pair monomorphs sharing a component cross-bind their per-component Eq dispatch

**RESOLVED 2026-09-04.**  Root cause and resolution at the bottom.

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


## Root cause (established 2026-09-04)

Not instance selection, and not the spec KEY: the emitted callee was right
(`__inst_Eq_eq_qu_int`) and only the ARGUMENT was bridged wrong.  A gdb
breakpoint on the niche unbox put the sink type at
`emit_expr.c`'s dispatch-argument bridge, `matched_spec->arg_types[i]` -- so
`find_matched_abi_spec` had returned the wrong spec.

That lookup keys on the source `Expr*`, with a cross-spec fallback:

```c
if (active_outer != NULL && !saw_call && !construct) {
    fallback_clone = ctx->specialized_call_names[i];   /* first entry, any outer */
}
```

Its own comment names the case it is for -- "no outer-matched entry exists
(top-level / single-spec case)".  With two sibling specs over one shared
instance body that case is no longer unambiguous, and worse, the entry it
grabs need not even belong to the same FUNCTION: a dict-dispatched call
re-resolves to a different concrete instance method per monomorph.  So
`Eq [Pair]`'s `(eq? (.snd x) (.snd y))`, emitting under the
`(Pair (Option String) int)` spec with `Eq[int]` as its callee, adopted the
`Eq[Option]` clone recorded under the sibling `(Pair ... (Option String))`
spec -- and bridged two plain ints as niche options.

The trace that settled it:

```
[fms] fn=__inst_Eq_eq_qu_int active_outer=...Pair__Option__String__int...
      saw_call=0 n_other=1 other=__inst_Eq_eq_qu_Option__spec__bool_void___void__
```

A clone named for `Eq[Option]` handed to a call whose callee is `Eq[int]`.

**The Expr\* is shared; the callee is not.**  The fallback now requires both:
`emit_spec_clone_belongs_to` resolves the candidate clone back to its spec and
checks `spec->binding == fn_binding`.  A genuine cross-spec fallback -- the
same callee under a different outer -- still passes; a cross-CALLEE one no
longer does.

A first attempt guarded on "only one candidate entry" instead, on the theory
that ambiguity was the problem.  It did not fix the repro (the trace above
shows `n_other=1`), and it was the wrong question: one candidate is not safer
than two if it names the wrong function.  Worth recording, because the count
heuristic looks plausible right up until the trace is read.

## Validation

- `bash tests/run.sh` **2787 passed / 0 failed, zero snapshot drift** -- for a
  change to a lookup this widely called (a dozen call sites in emit_expr.c
  alone), the snapshot suite is the gate, and it moved nothing.
- `tests/fixtures/eq-nested-binary-class-method-spec` group 2 now carries the
  sibling-spec case that the fix for the previous report had to omit.  Its last
  line varies ONLY the int component, so a re-crossing of the two dispatches
  fails loudly rather than silently agreeing.
- `run.sh` already fails any fixture whose build stderr carries
  `-Wint-conversion`, which is the primary guard here; the printed answers are
  secondary.
- option-niche seam 10/0, sr2 55/0, sr4 24/0, leak-check 79/0.
