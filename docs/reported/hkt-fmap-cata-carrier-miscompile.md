---
title: Generic catamorphism via `Functor` `fmap` miscompiles at runtime -- per-carrier closure-thunk ABI mismatch + boxed cata result (int/bool wrong-equality, cstr segfault)
category: Carrier <-> Concrete ABI -- HKT `fmap` closure/thunk specialization + cata result carrier
severity: Medium. The textbook generic recursion-schemes `cata`
  (`alg . fmap (cata alg) . unroll`) type-checks but miscompiles: at an
  int/bool carrier the result comes back boxed/mis-typed (prints right, but
  `(= 4 (re-cata size-alg e))` is FALSE; a `(ReF bool)` algebra folds the wrong
  branch), and at a cstr/pointer carrier it SEGFAULTS with a `-Wint-conversion`
  warning showing the recursive `fmap` closure thunk stored into the thunk's
  `__fn` slot at the wrong function-pointer type. Direct structural recursion
  (no `fmap`) is correct, so folds must fall back to hand-written recursion.
status: OPEN -- found 2026-06-21 by the turmeric-spices Track C U5 regex
  prototype (spices/regex/src/regex/tree.tur). Verified on turmeric 0.22.0,
  main @ 99cc8b3, built from source (build-release). Tracked as gap G6 in
  docs/carrier-concrete-abi-crossing-audit-plan.md.
---

# Generic catamorphism via `Functor` `fmap` miscompiles per carrier

## One-line summary

A generic catamorphism `cata alg = alg . fmap (cata alg) . unroll` over a
`Fix`-style sum functor passes the type checker but miscompiles: the recursive
closure passed to `fmap` is specialized per carrier `B` with the wrong
thunk function-pointer ABI, and the `(:: (fmap ...) (ReF B))` result threads
through the int64 carrier (boxed) instead of by value.

## Symptoms by carrier

- **`int`/`bool` carrier:** result comes back **boxed / mis-typed**. It *prints*
  the right value, but `(= 4 (re-cata size-alg e))` is **false** (so
  `assert-eq 4 ...` fails with "expected 4, got 4"), and a `(ReF bool)` algebra
  returns the wrong branch for some shapes (e.g. `or false true` folds to
  `false`).
- **`cstr` (pointer) carrier:** **segfault**, with a codegen warning showing the
  `fmap` closure thunk cast to the wrong function-pointer type.

## Emitted C warning (the smoking gun)

```
re_cata__spec__const_char___int64_t_int64_t:
warning: assignment to 'int64_t' from 'const char * (*)(void *, int64_t)'
         makes integer from pointer without a cast [-Wint-conversion]
  __t80->__fn = (tur_thunk_const_char___int64_t_t)regex__tree____fn_1061;

re_cata__spec__bool_int64_t_int64_t:
warning: assignment to 'int64_t' from '_Bool (*)(void *, int64_t)'
         makes integer from pointer without a cast [-Wint-conversion]
  __t77->__fn = (tur_thunk_bool_int64_t_t)regex__tree____fn_1061;
```

`__fn_1061` is the inner `(fn [c : Re] : B (re-cata alg c))` handed to `fmap`.
Its thunk pointer is being stored into an `int64_t` slot at the wrong type per
carrier specialization.

## Repro (pure Turmeric, no inline C)

```turmeric
(load "stdlib/typeclass-functor.tur")

;; one layer of a tiny AST + by-value fixed point (needs #483)
(defdata ReF :copy [a] (EmptyF) (LitF :int) (AltF a a) (StarF a))
(defdata Re  :copy (Roll (ReF Re)))

(definstance Functor [ReF]
  (fmap [c g]
    (match c
      (EmptyF)   (EmptyF)
      (LitF n)   (LitF n)
      (AltF x y) (AltF (g x) (g y))
      (StarF x)  (StarF (g x)))))

(defn unroll-re [e : Re] : (ReF Re) (match e (Roll l) (:: l (ReF Re))))

;; generic catamorphism (type-checks fine; miscompiles)
(defn re-cata [B] [alg : (fn [(ReF B)] B) e : Re] : B
  (alg (:: (fmap (unroll-re e) (fn [c : Re] : B (re-cata alg c))) (ReF B))))

;; int algebra: node count
(defn size-alg [l : (ReF int)] : int
  (match l (EmptyF) 1 (LitF n) 1 (AltF x y) (+ 1 (+ x y)) (StarF x) (+ 1 x)))

;; bool algebra: nullable?
(defn null-alg [l : (ReF bool)] : bool
  (match l (EmptyF) true (LitF n) false (AltF x y) (or x y) (StarF x) true))

;; cstr algebra (no concat needed to trigger the crash)
(defn tag-alg [l : (ReF cstr)] : cstr
  (match l (EmptyF) "e" (LitF n) "L" (AltF x y) x (StarF x) x))

(defn lit  [n : int] : Re (Roll (LitF n)))
(defn alt  [x : Re y : Re] : Re (Roll (AltF x y)))
(defn star [x : Re] : Re (Roll (StarF x)))

(defn main [] : int
  (let [e   (star (alt (lit 1) (lit 2)))          ;; Star(Alt(L,L)) -> 4 nodes
        opt (alt (lit 1) (Roll (EmptyF)))]        ;; Alt(L, Empty)  -> nullable
    (do
      ;; SYMPTOM 1 (int): prints 4, but the equality is false (boxed result)
      (println (re-cata size-alg e))              ;; prints 4
      (println (= 4 (re-cata size-alg e)))        ;; EXPECT true; GETS false

      ;; SYMPTOM 2 (bool): Alt(L, Empty) nullable = (or false true) = true
      (println (re-cata null-alg opt))            ;; EXPECT true; GETS false

      ;; SYMPTOM 3 (cstr): segfaults (mis-cast fmap closure thunk)
      (println (re-cata tag-alg e))               ;; SEGFAULT
      0)))
```

Control (all correct): replace `re-cata` with direct structural recursion --
`(match e (Roll layer) (let [l (:: layer (ReF Re))] (match l ...)))` inlining
each algebra -- and every case is right (`= 4` true, nullable true, cstr fold
returns "L", no crash). So the defect is the `fmap`-driven cata path, not the
algebras.

## Expected vs actual

| Carrier | Expected | Actual |
|---------|----------|--------|
| `int`   | `(= 4 (re-cata size-alg e))` -> `true` | `false` (result boxed/mis-typed; prints 4) |
| `bool`  | `re-cata null-alg (Alt L Empty)` -> `true` | `false` |
| `cstr`  | `re-cata tag-alg e` -> `"L"` | **segfault** (`-Wint-conversion` on the thunk) |

## Root cause (direction)

Two crossings on the same expression, both per-carrier `B`:

1. **Closure-thunk ABI:** the recursive `(fn [c : Re] : B (re-cata alg c))`
   handed to `fmap` is monomorphized per carrier, but its thunk `__fn` slot is
   stored at the wrong function-pointer type (`tur_thunk_const_char___int64_t_t`
   / `tur_thunk_bool_int64_t_t` cast over an `int64_t` slot) -- the
   `-Wint-conversion` warning. This is the closure/fn-value specialization path
   (GHE2 / `emit_abi_scan_fn_values`, and the poly-closure-result float-spec
   machinery in `emit_abi_register_call`), where the thunk signature must follow
   `B` rather than the int64 carrier.
2. **Cata result boxing:** the `(:: (fmap (unroll-re e) ...) (ReF B))` result is
   not unboxed to the carrier's native representation -- the `int`/`bool` `=`
   failures. The `fmap` result threads through the int64 carrier (boxed) instead
   of by value, so the equality compares a boxed handle, not the scalar.

## Suggested fix direction

Audit the HKT `fmap` call-site lowering where the algebra closure is
monomorphized per carrier `B`: the thunk signature must match `B`, and the
`(:: (fmap ...) (ReF B))` result must thread by value, not via the int carrier.
A `cstr`/pointer carrier is the clearest crash case to pin first (the
`-Wint-conversion` warning points straight at the mis-typed thunk store).

## Related

This is the **HKT closure/thunk** corner of the carrier <-> concrete ABI family
tracked in `docs/carrier-concrete-abi-crossing-audit-plan.md` (gap G6). It is a
sibling of the audit's other gaps in the sense the plan's section 4 predicts --
"HKTs generate the most crossings, and composition multiplies them" -- but it
trips a *different* mechanism than the value-ABI gaps:

- **G2/G3** are dispatch re-resolution over a field-read aggregate receiver
  (which concrete `__inst_*` to call, carrier-vs-byvalue parameter). G6 is the
  **fn-value / closure-thunk** specialization path: the offending value is a
  *function pointer* whose per-carrier signature is wrong, plus a boxed result.
- The closure-thunk-per-carrier machinery already has float-specific handling
  (`emit_inner_closure_needs_float_spec`, `poly-closure-result-specialization`);
  G6 is the same class of problem generalized to a pointer (`cstr`) and a
  narrower-than-int64 (`bool`) carrier, reached through an HKT `fmap` rather than
  a direct closure return.

So: same family (a parametric payload riding the int64 carrier where a concrete
representation was required), distinct crossing site (closure-thunk fn-pointer
ABI + cata result boxing), worth its own row so it is not rediscovered as "yet
another fmap bug."
