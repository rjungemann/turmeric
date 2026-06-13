---
title: Vec Eq rewrite to pure-Turmeric blocked on (1) affine multi-use AND (2) polymorphic-defn recursive-call return-type inference
severity: ergonomics gap / latent inference defect
date: 2026-06-13
---

## Update

Affine multi-use (issue 1) is fully unblocked by let-bound int coercion
(`(let [xi (:: x :int)] ...)`) — the rebound int IS Copy.  Probe:

```turmeric
(let [v (:: (vec-of) (Vec int))]
  (vec-push! v 1) (vec-push! v 2)
  (let [vi (:: v :int)]
    (println (vec-len vi))   ; 2
    (println (vec-len vi))))  ; 2, no use-after-move
```

The real remaining blocker is **issue 2**: a poly-defn recursive
self-call's return type is inferred as `int` instead of the declared
`:bool`.  Attempted:

```turmeric
(defn vec-eq-loop [A]
  [xi : int yi : int i : int len : int ^fat cmp : (fn [A A] bool)] : bool
  (if (= i len) true
      (if (cmp (:: (vec-get xi i) A) (:: (vec-get yi i) A))
          (vec-eq-loop xi yi (+ i 1) len cmp)   ;; <-- typed as int, not bool
          false)))
```

Diagnostic: `if branches have mismatched types: then=int else=bool` on
the inner if.  The recursive call's return doesn't unify against the
declared `:bool`; it falls back to the carrier `int`.  Cons's working
rewrite recurses via `(eq? (:: t1 (Cons A)) (:: t2 (Cons A)))` — that's
typeclass dispatch, NOT a direct poly-defn self-call — which is why Cons
shipped and Vec hasn't.

This is a real type-inference gap.  Likely lives in
`src/compiler/elab_fn.c` (or wherever the recursive defn binding gets
seeded with a placeholder return type) — the placeholder is currently
the carrier-int instead of the declared full type.

## Suite state at time of report

`bash tests/run.sh 2>&1 | grep -c '^FAIL '` = 172 (baseline, unchanged).
The Vec definstance is back to the carrier-helper form; no fixture
churn.

---

## Summary

After the three TCO-in-ABI-specs lifts landed (commits …5..53c23aed),
the planned Vec rewrite in
`docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md` was attempted
and immediately blocked by `Vec`'s affine semantics. The planned shape

```turmeric
(definstance Eq [Vec] [(Eq A)]
  (eq? [x y]
    (and (= (vec-len x) (vec-len y))
         (vec-eq-loop x y 0 (vec-len x) (fn [a b] (eq? a b))))))
```

triggers `TUR-E0005 use-after-move: binding 'x' was moved` because
`vec-len` consumes its `(Vec A)` receiver (passed via int-carrier
ascription) and the recursive loop also needs `x`/`y` again.

`vec-eq-loop` itself wants `x : (Vec A)` referenced four times across the
function body (two `vec-get`s per iteration plus the recursive backedge).
Even within the loop body alone, every iteration would move `x` and `y`
into `vec-get`, then need them again for the backedge.

## Why the carrier helper version works

`(vec-eq? x y (fn ...))` performs **exactly one** ascription per
receiver — the `:int` coercion happens at the call boundary, the
helper takes opaque ints, and Turmeric's affine checker sees a single
move per binding.

## Options to unblock

1. **Borrow refs / &Vec** — adopt a borrow type for the Eq receiver so
   `(vec-len &x)` doesn't consume. Requires a language extension; out
   of scope here.
2. **One-shot carrier conversion + int-typed loop** — let-bind
   `(:: x :int)` once into a Copy int, then use the int everywhere.
   Requires the int rebinding to be Copy after the ascription consumes
   the Vec; this may already work in some cases but needs a probe.
3. **Inline the whole loop in `eq?` body** — single defn, no helper,
   single move per receiver. Doable but loses TCO (the body recurses
   into Eq dispatch on element type, not a self-call).
4. **Accept the carrier helper indefinitely** — the M4c bridge audit
   counts will not reduce on Vec, Map, MutableMap, Set until a borrow
   or one-shot-carrier story lands. This is the status quo.

## Recommendation

Probe option (2) — a let-bound `(:: x :int)` may be the cheapest
ship-path. If the resulting int is Copy and accepted by `vec-len` and
`vec-get`, the rewrite simplifies to:

```turmeric
(eq? [x y]
  (let [xi (:: x :int)
        yi (:: y :int)
        lx (vec-len xi)
        ly (vec-len yi)]
    (and (= lx ly) (vec-eq-loop-i xi yi 0 lx (fn [a b] (eq? a b))))))
```

If that probe fails (the ascription itself errors or the int isn't
treated as Copy), the right next step is a `borrow` / `&` design pass
rather than further iteration on the eq? rewrite shape.

## What landed

- TCO lifts #1, #2, #3 (`53c23aed` and predecessors). The eligibility
  gate, `tco_param_type` spec-awareness, and dict-arg reject all work
  correctly. The infrastructure for pure-Turmeric iteration inside
  ABI specs is in place; only the affine pattern blocks Vec specifically.
- Cons (`stdlib/list.tur`) IS using the pattern successfully because
  field projection (`.tail`) returns a fresh `(Cons A)` ascribed from
  the int carrier — each recursive level gets a fresh binding, so
  there's no multi-use of the same receiver.

## Suite state at time of report

`bash tests/run.sh 2>&1 | grep -c '^FAIL '` = 172 (baseline, unchanged).
