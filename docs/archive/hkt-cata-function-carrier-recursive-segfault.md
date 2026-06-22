# Recursive function-typed-carrier catamorphism segfaults (follow-up to #489)

**Found by:** turmeric-spices Track C (U5 regex matcher)
**Verified on:** turmeric 0.22.0, main @ 1941256
**Severity:** Low-Medium. The "interpreter / NFA as one cata" form (carrier = a
closure). Direct structural recursion is a working alternative (shipping in
regex/tree).

**Status: RESOLVED 2026-06-22 -- archived.** Canonical repro FIXED (PR #489
era); residual (a) captureless-arm carved into its own report and fixed by
PR #501 (`docs/archive/captureless-algebra-arm-thin-through-carrier.md`);
residual (b) mixed fn/value carrier env-struct collision fixed by PR #500.
Nothing remains open.

## Status: the canonical repro is FIXED (this branch)

#489 fixed the fn-typed match-arm payload *capture* (its fixture
`fn-typed-match-arm-capture` -- the algebra applied to a single layer -- prints
7) and the result-type collapse (Bug 0). The full *recursive* catamorphism
`cata alg = alg . fmap (cata alg) . unroll` over a by-value `Fix`
(`Expr = Roll (ExprF Expr)`) with a FUNCTION-typed carrier `B = (fn [int] int)`
still SEGFAULTED at runtime ("Bug C"). That recursive segfault -- the repro
below, with capturing algebra arms -- is now fixed.

## Repro (no inline C) -- now prints 7

```turmeric
(load "stdlib/typeclass-functor.tur")

(defdata ExprF :copy [a] (LitF :int) (AddF a a))
(defdata Expr  :copy (Roll (ExprF Expr)))

(definstance Functor [ExprF]
  (fmap [c g]
    (match c
      (LitF n)   (LitF n)
      (AddF x y) (AddF (g x) (g y)))))

(defn unroll-e [e : Expr] : (ExprF Expr)
  (match e (Roll l) (:: l (ExprF Expr))))

;; generic catamorphism, carrier B
(defn cata [B] [alg : (fn [(ExprF B)] B)  e : Expr] : B
  (alg (:: (fmap (unroll-e e) (fn [c : Expr] : B (cata alg c))) (ExprF B))))

;; carrier B = (fn [int] int): each node folds to an env->int function
(defn fn-alg [l : (ExprF (fn [int] int))] : (fn [int] int)
  (match l
    (LitF n)   (fn [env : int] : int n)
    (AddF f g) (fn [env : int] : int (+ (f env) (g env)))))

(defn lit [n : int] : Expr (Roll (LitF n)))
(defn add [x : Expr y : Expr] : Expr (Roll (AddF x y)))

(defn main [] : int
  (do (println ((cata fn-alg (add (lit 3) (lit 4))) 0))   ;; 7
      0))
```

## Root cause

Three coordinated thin-vs-fat dispatch defects at the generic int64 carrier
boundary. Every function value that crosses the carrier is (or must be) a
uniform fat closure box `{ thunk, env... }`; the dispatch sites assumed a thin
function pointer.

1. **Chained application of the cata result** (`((cata fn-alg e) 0)`). The head
   is the result of a generic call whose declared result is a bare type variable
   (`cata ... : B`, recovered to `(fn [int] int)` by #489's Bug 0 fix). There is
   no single named closure thunk to route through (the body returns `(alg ...)`,
   a value reconstructed from the carrier), so the application fell back to a
   *thin* pointer call -- a jump into the closure's env block -> SIGSEGV.

2. **Constructed ADT field holding a thin fn.** `(AddF lit3 lit4)` storing
   captureless lambdas left thin function pointers in the field; the
   monomorphized constructor lowers a parametric (`a`) field to the int64
   carrier, so the existing tyvar->fat auto-shim (`EX_FN_TO_FAT`) never fired at
   construction. The match-arm extraction (fix 3) then fat-dispatched a thin
   pointer.

3. **Function-typed match-arm payload.** `(AddF f g)` over a `(fn ...)` carrier
   binds `f`/`g` to fat boxes (the recursive `cata` results, threaded through
   `fmap`), but the inner lambda `(fn [env] (+ (f env) (g env)))` dispatched
   them thin.

## Fix

- `src/compiler/elab_call.c`, `elab_call_head_expr`: when the chained-call head
  is the result of a generic call whose callee's declared result is a bare
  `TY_TYVAR`, mark the head temp's fn type `boxed` so emit takes the runtime
  slot-0 fat-dispatch path (ER2).
- `src/compiler/elab_call.c`, constructor application: box a concrete (`TY_FN`)
  function value stored into a parametric (tyvar) ADT field via `EX_FN_TO_FAT`,
  so the field is a uniform fat box. A *carrier-erased* recursion result inside a
  generic `fmap`/`cata` body is elaborated as the int64 carrier (`TY_INT`, the
  carrier type `B` being an unbound tyvar) and is therefore NOT statically
  `TY_FN`, so the gate skips it -- it is already fat and must not be
  double-boxed.
- `src/compiler/elab_structs.c`, ADT match-arm binding: a `TY_FN` field declared
  as a bare type variable is marked `is_fat`, so `(f env)` fat-dispatches.
- `src/compiler/emit_expr.c`, constructor emit: cast an `EX_FN_TO_FAT` box (a
  `void *`) to the int64 carrier when storing into the constructor's int64 field
  (clean codegen, no `-Wint-conversion`).

Regression fixture: `tests/fixtures/hkt-cata-fn-carrier-recursive/` (the repro
plus a deeper `((3+4)+5)` nesting). Full `bash tests/run.sh`: 1752 passed, 0
failed; the value-carrier cata (`B = int`) and #489's
`fn-typed-match-arm-capture` remain green.

## Remaining OPEN residuals

Two narrower gaps in the same area. Residual (b) is now FIXED (2026-06-22);
residual (a) is carved into its own report and remains open.

### (a) Captureless algebra arm stays thin through the carrier (Bug B) -- OPEN

> Now tracked as its own report:
> `docs/reported/captureless-algebra-arm-thin-through-carrier.md`. Summary kept
> here for context.

An algebra arm that returns a *captureless* closure -- e.g. a variable-lookup
node `(VarF i) (fn [env : int] : int env)` in an environment-passing
interpreter -- is codegen'd as a bare thin function pointer, not a fat box. When
that arm's value is the cata result (or a sibling of fat arms in the same
closure-returning function), it crosses the carrier thin while fix 1/3
fat-dispatch it -> SIGSEGV.

Minimal repro (segfaults):

```turmeric
;; ... ExprF with (VarF :int), cata as above ...
(defn eval-alg [l : (ExprF (fn [int] int))] : (fn [int] int)
  (match l
    (LitF n)   (fn [env : int] : int n)
    (VarF i)   (fn [env : int] : int env)     ;; captureless -> thin
    (AddF f g) (fn [env : int] : int (+ (f env) (g env)))))
;; ((cata eval-alg (var 0)) 100)   ;; SIGSEGV
```

Root cause: `eval-alg`'s `VarF` arm returns `__fn_NNNN` (a raw thin fn pointer)
where the function's other arms return heap fat boxes; the function's `(fn ...)`
result is not uniformly fat. Fix direction: when a closure-returning function /
match yields a fat box in any arm (or its result feeds a carrier-typed
algebra), auto-shim the captureless arms to fat via `EX_FN_TO_FAT` at the
tail/return leaves (the `result_fat` machinery, applied at the carrier crossing
rather than only on an explicit `^fat` annotation). This is the report's
original "Bug B" (thin captureless lambda -> fat box at the carrier boundary)
and is a broader closure-ABI uniformity change. Affects environment-passing
interpreters / variable-lookup nodes specifically.

### (b) Mixed fn-carrier + value-carrier cata: spec-name collision -- FIXED 2026-06-22

A program that folds the *same* `Expr`/`Fix` with both a function carrier
(`fn-alg : (fn [(ExprF (fn [int] int))] (fn [int] int))`) and a value carrier
(`size-alg : (fn [(ExprF int)] int)`) failed to *compile*: both `cata`
specializations mangled their carrier `B` to the int64 carrier and emitted a
colliding env struct (`struct __env_NNNN__spec__int64_t` defined twice with
different `__fn` field types).

**Fixed:** `emit_abi_clone_name` already disambiguates colliding clone names
with a `__h<n>` suffix (Gap H); the env-struct override site
(`src/compiler/emit_module.c`) now mirrors that -- when the base env name
collides with another spec's `env_name_override`, it appends the same
deterministic `__h<n>` discriminator. Non-colliding names are untouched, so no
snapshot churn. Pinned by `tests/fixtures/hkt-cata-mixed-fn-value-carrier`
(prints `7` then `2`). `bash tests/run.sh` => 1761 passed, 0 failed.

## Impact / workaround

The capturing-arm "interpreter as one cata" form now works through the generic
`cata`. Environment-passing interpreters with a bare variable-lookup arm still
need the direct-structural-recursion workaround (regex/tree's `re-matches?`)
until residual (a) lands.
