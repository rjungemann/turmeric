# A declared or ascribed size index is never checked against the value it describes

**Severity: medium** -- not a miscompile, but it defeats the static discipline
`docs/guides/sized-types-guide.md` advertises, and it does so through the
idiom that guide and `stdlib/sized-buf.tur` tell you to use. Filed 2026-09-05
while verifying whether sized types are enforced at all. They are: the
cross-parameter unifier (`sz_cross_param_unify`,
[src/compiler/elab_call.c:1869](../../src/compiler/elab_call.c)) rejects every
statically-known argument-position mismatch the guide claims it does. What it
never checks is the *claim itself*.

Two halves, one root cause -- a written size claim is never reconciled with the
value it describes:

- **(A) trusted but unchecked.** A `defn`'s declared return-type size index IS
  recovered and used downstream, and is never validated against the body. So a
  wrong annotation does not merely go unnoticed; it launders a mismatch past a
  check that fires correctly otherwise.
- **(B) written but inert.** An ascribed size (`(:: e (SizedBuf (Static k)))`)
  is neither validated against `e` nor recovered at call sites, so it
  contributes no static size at all -- including in the direct-argument
  position where the same comparison from a function return WOULD fire.

(A) is the soundness half. (B) is why (A) is reachable through the stdlib's
documented surface rather than only through a hand-written mistake.

## Repro

All four probes exit 0 with no diagnostic under `./build/tur check`.

### (A) return-position annotation is taken on faith

```turmeric
(defgadt Size []
  (Static (int) : (Size))
  (Add (Size) (Size) : (Size))
  (Mul (Size) (Size) : (Size)))
(defgadt SizedVec [n]
  (SVNil  : (SizedVec (Static 0)))
  (SVCons int (SizedVec n) : (SizedVec (Add (Static 1) n))))

; declares length 3, returns length 1 -- ACCEPTED
(defn three [] : (SizedVec (Static 3)) (SVCons 1 (SVNil)))

(defn pairwise [xs : (SizedVec n) ys : (SizedVec n)] : int 0)

(defn main [] : int
  ; `n` unifies 3 against 3 on the DECLARATIONS; the real lengths are 1 and 3.
  (println (pairwise (three) (SVCons 1 (SVCons 2 (SVCons 3 (SVNil))))))
  0)
```

Same on a phantom `defopaque` carrier:

```turmeric
(defopaque LaMatN [m n] :int)
(defn mk23 [] : (LaMatN (Static 2) (Static 3)) (:: 0 :LaMatN))
(defn bogus [] : (LaMatN (Static 5) (Static 5)) (mk23))     ; ACCEPTED
(defn takes55 [a : (LaMatN (Static 5) (Static 5))] : int 7)
(defn main [] : int (println (takes55 (bogus))) 0)          ; ACCEPTED
```

The control proves the check is real and that the annotation is what defeats
it: passing `(mk23)` to `takes55` directly IS rejected --
`TUR-E0260: argument 1 of 'takes55' has size 2 but parameter declares size 5`.
Routing the same value through `bogus` launders it.

### (B) ascribed sizes are inert

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(defn main [] : int
  ; 4 elements allocated, 99 claimed -- ACCEPTED
  (let [b (:: (sized-buf-new-zeroed 4) (SizedBuf (Static 99)))]
    (println (sized-buf-len b)))
  0)
```

and the ascribed size never reaches the unifier, even in argument position
against a shared `n` (`sized-buf-copy! [n] [dst : (SizedBuf n) src : (SizedBuf n)]`,
[stdlib/sized-buf.tur:343](../../stdlib/sized-buf.tur)):

```turmeric
(defn main [] : int
  ; (Static 4) vs (Static 99) on a shared `n` -- ACCEPTED
  (sized-buf-copy! (:: (sized-buf-new-zeroed 4) (SizedBuf (Static 4)))
                   (:: (sized-buf-new-zeroed 4) (SizedBuf (Static 99))))
  0)
```

## Why this reaches the stdlib's documented surface

`sized-buf-new [n] [k : int] : (SizedBuf n)`
([stdlib/sized-buf.tur:217](../../stdlib/sized-buf.tur), and
`sized-buf-new-zeroed` at :232) has **no connection between the runtime length
`k` and the type-level `n`**. Its own docstring says so and prescribes the fix:

> A fresh (SizedBuf n) where `n` is polymorphic; pin it via ascription, e.g.
> `(:: (sized-buf-new 4) (SizedBuf (Static 4)))`.

By (B) that ascription is inert. So every static guarantee on the `SizedBuf`
surface rests on a programmer claim the compiler neither validates nor, on the
ascription path, even reads.

## Root cause

Size literals in a type-application slot lower to a `TY_INT` placeholder
([src/compiler/elab_fns.c:4448](../../src/compiler/elab_fns.c)) -- the real
size rides only on the retained Form. Two consequences:

1. `fn_type.as.fn.result_type_form = return_type_form_kept`
   ([src/compiler/elab_fns.c:8644](../../src/compiler/elab_fns.c)) retains the
   declared return annotation, and `sz_recover_type_form`'s `EX_CALL` arm
   ([src/compiler/elab_call.c:1787](../../src/compiler/elab_call.c)) hands it
   straight to the unifier. Nothing anywhere compares a function body's
   inferred `size_index` against that Form, because the types are both
   `TY_INT` and unify vacuously. Hence (A).
2. `sz_recover_type_form` handles `EX_VAR`, `EX_CALL` and `EX_GET_FIELD` and
   falls through `default: return NULL`
   ([src/compiler/elab_call.c:1836](../../src/compiler/elab_call.c)). There is
   no `EX_ASCRIBE` arm, and an ascribed `let` init does not populate
   `decl_type_form` the way a call init does
   ([src/compiler/elab_forms.c:970](../../src/compiler/elab_forms.c)). Hence
   (B).

This is the residue of the placeholder representation that
[sz8-opaque-phantom-size-not-load-bearing](../archive/history/sz8-opaque-phantom-size-not-load-bearing.md)
introduced to make `(Static N)` legal in a type-app slot, and that
[sz8-projection-size-recovery-gap](../archive/history/sz8-projection-size-recovery-gap.md)
then extended recovery over. Both closed the *recovery* direction; neither
closed the *validation* direction, and ascription was never added to either.

## Fix directions

Roughly in increasing order of cost:

1. **Check the body against the declared return Form.** Where a `defn`'s
   return annotation carries a foldable size index and the body's inferred
   `size_index` also folds, compare them and emit `TUR-E0260` on disagreement.
   This is the same fold-and-compare `sz_cross_param_unify` already does in its
   Case 2 arm ([elab_call.c:1958](../../src/compiler/elab_call.c)), applied at
   the return seam. Closes (A), which is the soundness half, and is the
   cheapest of these.
2. **Give `sz_recover_type_form` an `EX_ASCRIBE` arm** returning the ascribed
   type Form. This makes ascribed sizes load-bearing, so the second (B) repro
   starts rejecting. Note this makes (A) *more* consequential until (1) lands,
   since it adds a second unvalidated claim the unifier trusts -- do (1) first
   or together.
3. **Tie `sized-buf-new`'s `k` to `n`.** The honest signature is a refined or
   size-witnessed constructor rather than a polymorphic `n` pinned afterward by
   ascription; at minimum the ascription should be checked against `k` when `k`
   is a literal. Without this, (1) and (2) make the annotations consistent with
   each other but still disconnected from the allocation.

Deliberately not proposed: rejecting an ascription whose size is not
statically known. Ascription against a runtime-computed length is the
legitimate polymorphic case and must keep working -- the fall-through to the
runtime predicates is by design, per the guide.

## Guides to update when fixed

- `docs/guides/sized-types-guide.md` -- the "Static checking" callout lists its
  deferrals (open templates, size arithmetic, run-time-known sizes) and reads
  as though the discipline is otherwise closed. Neither half of this report is
  among them.
- `stdlib/sized-buf.tur` -- the module docstring (:5-:10) and
  `sized-buf-new` / `sized-buf-new-zeroed` docstrings, which prescribe the
  inert ascription idiom as the way to pin `n`.
- `README.md:271` -- "Flat buffer -- phantom size index, bounds-checked access"
  is accurate about the bounds check being a runtime one, but sits next to the
  ascription idiom.

## Related

- [gadt-length-index-not-enforced](gadt-length-index-not-enforced.md) -- the
  separate, already-open dependent-types gap. That report is about indices that
  are phantom *by design pending a future phase*; this one is about indices
  that are load-bearing today and simply unvalidated.
- Element indexing (`sized-buf-get [n] [b : (SizedBuf n) i : int]`,
  [stdlib/sized-buf.tur:284](../../stdlib/sized-buf.tur)) is a runtime bounds
  check by design, and is **not** part of this report. Making it static is
  planned as RE2 in
  [docs/upcoming/v1/ecs-refinement-typed-apis-plan.md](../upcoming/v1/ecs-refinement-typed-apis-plan.md).
