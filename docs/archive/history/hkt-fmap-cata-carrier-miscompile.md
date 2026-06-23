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
status: RESOLVED -- found 2026-06-21 by the turmeric-spices Track C U5 regex
  prototype (spices/regex/src/regex/tree.tur). Tracked as gap G6 in
  docs/carrier-concrete-abi-crossing-audit-plan.md. Closed on branch
  claude/g6-carrier-concrete-abi-audit-pb3gqo by monomorphizing the HKT instance
  over parametric sum types (direct fmap) AND per-spec cloning the recursive
  closure passed to fmap (generic catamorphism).  bool/int/float/cstr round-trip
  over deep trees; suite green (1748/0).  Fixtures hkt-fmap-byvalue-sum-element
  (direct) and hkt-cata-fmap-byvalue-carrier (recursive).  See the dated status
  updates below for the spec-selection (A0), direct-fmap-layout, and recursive
  (A/B/C) sub-fixes.
---

## Status update (2026-06-21) -- spec-selection half fixed; closure-thunk half open

Root cause split in two:

1. **Spec selection (FIXED).** `re-cata` is a constrained generic differentiated
   ONLY by its return type `B`; for B=int/bool/cstr it interns sibling specs with
   identical argument types (`int64_t, int64_t`), differing only in result. The
   by-args spec lookup (`find_matched_abi_spec` / `emit_call_name`) couldn't tell
   them apart, so the int `(re-cata size-alg e)` call grabbed the `bool` spec
   (`re_cata__spec__bool_...`) -- making `(= 4 <bool>)` an always-false
   `-Wbool-compare`. Fixed by `emit_spec_result_mismatch` (`emit_expr.c`): a
   by-args match is now rejected when the call's and the spec's result types are
   distinct primitive kinds. int now prints `4` / `true`; cstr no longer
   segfaults (prints `L`). Suite green (1743/0).

2. **Closure-thunk per carrier (OPEN).** The recursive closure handed to `fmap`
   (`(fn [c : Re] : B (re-cata alg c))`) is lifted to a SINGLE C function
   `__fn_1045` returning `int64_t` and calling the carrier base `re_hycata`,
   shared across all three specs (the bool/cstr specs merely cast its thunk to a
   different return type -- the residual `-Wint-conversion`). For a sub-word
   `bool` carrier this is wrong: `null-alg` over `Alt(Lit, Empty)` should fold to
   `(or false true) = true` but yields `false`. The fix is to clone the lifted
   closure per carrier `B` (a `__fn_1045__spec__bool` whose body calls
   `re_cata__spec__bool` and returns `bool`), generalizing the existing
   inner-closure float-spec machinery (`emit_inner_closure_needs_float_spec` /
   `poly-closure-result-specialization`) to sub-word and pointer carriers. cstr
   "works" today only because a pointer round-trips losslessly through the int64
   carrier; `bool` does not.

---

## Status update (2026-06-21, branch claude/g6-carrier-concrete-abi-audit-pb3gqo) -- the remaining half is NOT cata-specific: the carrier `Functor` instance miscompiles a *direct* `fmap` over any sub-int64 element

The remaining "closure-thunk" half was mischaracterized as a closure/cata
problem. It is actually a **Functor-instance layout** problem, and it reproduces
with **no cata, no recursion, and no closure capture** -- a single direct `fmap`
whose result element is narrower than the int64 carrier is already wrong:

```turmeric
(load "stdlib/typeclass-functor.tur")
(defdata ReF :copy [a] (EmptyF) (LitF :int) (AltF a a) (StarF a))
(definstance Functor [ReF]
  (fmap [c g]
    (match c
      (EmptyF)   (EmptyF)
      (LitF n)   (LitF n)
      (AltF x y) (AltF (g x) (g y))
      (StarF x)  (StarF (g x)))))
(defn to-bool [x : int] : bool (> x 0))
(defn main [] : int
  (let [r (:: (fmap (:: (AltF 0 5) (ReF int)) to-bool) (ReF bool))]
    ;; x=(0>0)=false, y=(5>0)=true -- print y
    (println (match r (EmptyF) 0 (LitF n) 1 (AltF x y) (if y 100 200) (StarF x) 3))))
;; EXPECT 100 (y = true).  ACTUAL: 200 (y read as false).
```

A `float` element is wrong the same way (and additionally trips the
`-Wint-conversion` register-class issue on the closure thunk when reached through
the cata closure). `int` and `cstr` happen to be correct only because they are
exactly 8 bytes wide, matching the carrier slot.

### Root cause (concrete C, from the repro above)

The `Functor [ReF]` instance is emitted **once**, as the int64 *carrier*
representative `__inst_Functor_fmap_T`, whose body builds the **carrier** ADT
`tur_adt_ReF` via `ctor_AltF(int64_t, int64_t)`:

```c
typedef struct tur_adt_ReF {            // carrier (what fmap BUILDS)
    int tag;
    union { ...; struct { int64_t _0; int64_t _1; } AltF; ...; } as;
} tur_adt_ReF;                          //  -> AltF._1 at offset 8
```

But the consumer reads the value at the **by-value monomorphized** layout
`tur_adt_ReF__bool` (because `(ReF bool)` lowers the element field to `bool`):

```c
typedef struct tur_adt_ReF__bool {      // by-value (what the reader EXPECTS)
    int tag;
    union { ...; struct { bool _0; bool _1; } AltF; ...; } as;
} tur_adt_ReF__bool;                     //  -> AltF._1 at offset 1
```

`fmap` writes `_1 = true` at offset 8; the `(ReF bool)` reader loads `_1` from
offset 1 (byte 1 of the int64 `_0 = 0`) and sees `false`. Pure layout mismatch,
no closure involved. The cata example is just the first place a spice exercised a
sub-int64 functor element.

### Refined fix direction (subsumes the closure-thunk half)

The real fix is to **monomorphize the HKT instance method per concrete result
element**, so producer and consumer agree on the ADT layout:

- The emit side **already** has the by-value HKT instance-method spec machinery
  (M7 "layer-4", `g_m7_hkt_enabled`): `emit_fns.c:566` returns the by-value ADT
  C name (`tur_adt_ReF__bool`) for the instance-method return *once an ABI spec
  is active*, and `emit_module.c:2636` notes the carrier base so the dict slot
  still resolves. The body-construction sites (`ctor_*`) already have by-value
  twins (`ctor_AltF__bool`).
- The GAP is that **no by-value `fmap` spec is interned / selected** at the
  `(:: (fmap ...) (ReF bool))` call site. `emit_abi_register_call`
  (`emit_module.c:1812`) needs `call->as.call_.abi_bindings` to mint a spec, and
  elab does not attach the result-element binding (`b -> bool`) to an HKT method
  call. So the call falls through to the carrier `__inst_Functor_fmap_T`.
- Closing it: (1) elab attaches the HKT method call's result-element tyvar
  binding (the `b` in fmap's `(f a) -> (f b)`) when the ascription/context pins
  it to a concrete type; (2) `emit_abi_register_call` mints the by-value
  instance-method spec for a concrete sub-int64 `TY_APP` result (the layer-4
  emit path then produces `__inst_Functor_fmap__spec__...` building
  `tur_adt_ReF__bool` and calling `g` at its true result register class);
  (3) the recursive-closure register-class clone (the originally-described
  closure-thunk half) then falls out for `float` because `g` is finally called
  at `double` rather than via the int64 carrier.

This is bounded but non-trivial: step (1) touches elaboration of *every* HKT
method call, so it is snapshot- and regression-sensitive and must be gated to
fire only when the result element is a concrete type that does not round-trip
through the int64 carrier (i.e. sub-int64 integer-class or float). `int`/`cstr`
elements must remain on the carrier path byte-for-byte (zero churn).

**Status:** still OPEN. The diagnosis is sharpened (the bug is the carrier
Functor-instance layout, reproducible without cata) and the entry points are
identified; the by-value-HKT-spec minting is the remaining implementation.

---

## Status update (2026-06-21, branch claude/g6-carrier-concrete-abi-audit-pb3gqo) -- DIRECT-fmap half FIXED (by-value HKT instance monomorphization over sum types); recursive cata half remains

The direct-`fmap` layout half is now **FIXED**. The M7 by-value HKT
instance-method spec machinery handled parametric STRUCTS (Option/Result/Pair)
but not parametric ADTs (`defdata` sum types), so a `Functor [ReF]` over a sum
fell through to the int64-carrier representative. Closed by:

1. **elab** (`elab_typeclasses.c`): `m7_body_constructs_byvalue` /
   `m7_body_returns_byvalue_element` now recurse through an `EX_MATCH` body and
   accept an ADT constructor call (`e->as.call_.ctor`) -- the shape of a
   match-bodied Functor over a sum.
2. **types** (`types.c`/`.h`): `type_app_is_concrete_adt` + `type_adt_app_def`,
   distinct from the struct-app predicate the existing machinery used.
3. **emit register** (`emit_module.c`): an HKT instance method whose result
   grounds to a concrete parametric ADT app is an ABI change -> a per-(f, A)
   by-value spec is interned.
4. **emit ctor** (`emit_expr.c`): a bare-TY_ADT construct inside a by-value HKT
   spec has its element erased; `emit_hkt_spec_ctor_suffix` recovers the
   monomorphized ctor suffix from the active spec's result family, so the body
   emits `ctor_AltF__bool` (matching the consumer's by-value layout) instead of
   the carrier `ctor_AltF`. `g` is already called at its true result register
   class, so float/bool/cstr/int all round-trip.

Fixture `tests/fixtures/hkt-fmap-byvalue-sum-element` (bool/float/int/cstr round
trip through a direct `fmap`). Suite green (1746/0), zero snapshot churn.

**Recursive cata half -- ALSO FIXED (this branch).** `cata = alg . fmap (cata
alg) . unroll` now round-trips `bool`/`int`/`float`/`cstr` over DEEP trees. Three
coupled changes closed it (suite green 1748/0):

- **A (elab):** inside the generic `re-cata [B]` body the `fmap` result element
  `B` is ungrounded and `m7_collect_tyvar_bindings` skipped it (its tyvar-actual
  skip guards Applicative `ap`); a symbolic `b -> B` binding is now attached for
  the fmap/Monad shape (element = a closure arg's result).
- **B (emit):** the method's declared `(f b)` is instantiated through the
  composed spec bindings (`b -> B -> bool`) to recover the concrete `(ReF bool)`,
  so the by-value `fmap` spec is minted inside `re_cata__spec__bool`.
- **C (emit):** `emit_find_passed_spec_closure` finds the CAPTURED closure PASSED
  to the `fmap` call; the existing inner-closure-spec machinery clones it per
  spec (the EX_CLOSURE emit already redirects via `inner_closure_spec_idx`); the
  clone body is scanned under its own spec so its recursive `(re-cata alg c)`
  registers the active return-spec, and a return-only-poly recursive result is
  recovered from the active spec's bindings (scoped to `is_passed_closure_clone`)
  so it resolves to `re_cata__spec__double` not a spurious int64 sibling.

A DEEP tree is the discriminating probe: a shallow `Alt(Lit, Empty)` passes by
leaf-child luck, but `Alt(Lit, Alt(Lit, Empty))` mis-reads the nested `_1` unless
the whole recursion stays in the by-value `B` world. Fixtures
`hkt-fmap-byvalue-sum-element` (direct) and `hkt-cata-fmap-byvalue-carrier`
(recursive, deep + discriminating + float). **G6 fully closed.**

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
