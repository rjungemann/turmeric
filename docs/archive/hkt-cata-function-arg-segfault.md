# `cata` with a function-typed carrier whose own argument is a function mis-lowers the argument (thin int64_t) and segfaults

**Found by:** turmeric-spices Track C (U5 regex matcher), re-checking after #489
**Verified on:** turmeric main @ d9fb741 (#497), built from source
**Severity:** Low-Medium. Blocked the "NFA / matcher is one cata" CPS form
(carrier = a continuation-taking closure). Direct structural recursion was a
working alternative.

## Status: FIXED

A generic catamorphism over a by-value `Fix` whose carrier `B` is a function
type already worked when `B`'s arguments were scalars (`B = (fn [int] int)`,
#489; `B = (fn [int int] int)`). But when an argument of the carrier was
*itself a function* -- `B = (fn [(fn [int] int) int] int)` -- the program
type-checked, compiled (with an int-from-pointer warning) and **segfaulted at
runtime**: at the carrier-result application site the function-typed argument
was passed *thin* (cast to `int64_t`) while a sibling capturing closure flowing
into the same slot was a fat box, so the callee dispatched a fat box as a raw
function pointer and jumped into the env block.

This was the next variant past
[hkt-cata-function-carrier-recursive-segfault](../reported/hkt-cata-function-carrier-recursive-segfault.md):
#489 gave the carrier *result* the fat-closure ABI; the carrier's
function-typed *arguments* at the application site needed the same treatment.

## Repro (no inline C) -- now prints 7 / 12

```turmeric
(load "stdlib/typeclass-functor.tur")

(defdata ExprF :copy [a] (LitF :int) (AddF a a))
(defdata Expr  :copy (Roll (ExprF Expr)))

(definstance Functor [ExprF]
  (fmap [c g]
    (match c
      (LitF n)   (LitF n)
      (AddF x y) (AddF (g x) (g y)))))

(defn unroll-e [e : Expr] : (ExprF Expr) (match e (Roll l) l))

(defn cata [B] [alg : (fn [(ExprF B)] B) e : Expr] : B
  (alg (:: (fmap (unroll-e e) (fn [c : Expr] : B (cata alg c))) (ExprF B))))

(defn lit [n : int] : Expr (Roll (LitF n)))
(defn add [x : Expr y : Expr] : Expr (Roll (AddF x y)))

;; carrier B = (fn [(fn [int] int) int] int): first arg is itself a function
(defn alg [l : (ExprF (fn [(fn [int] int) int] int))]
         : (fn [(fn [int] int) int] int)
  (match l
    (LitF n)   (fn [k : (fn [int] int) s : int] : int (k (+ s n)))
    (AddF x y) (fn [k : (fn [int] int) s : int] : int
                 (x (fn [s2 : int] : int (y k s2)) s))))

(defn main [] : int
  (println ((cata alg (add (lit 3) (lit 4))) (fn [r : int] : int r) 0))) ;; 7
```

Regression fixture: `tests/fixtures/hkt-cata-fn-arg-carrier/`.

## Root cause

Every function value that crosses the generic int64 carrier boundary is (or
must be) a uniform fat closure box `{ thunk, env... }`. For a fat-dispatched
carrier `B = (fn [(fn [int] int) int] int)` the dispatcher passes each argument
slot as an opaque `int64_t`, so a function-typed argument must arrive as a fat
box and be fat-dispatched by the callee. Two coordinated sites disagreed:

1. **Producer (the algebra arm closures).** The arm lambdas
   `(fn [k : (fn [int] int) s : int] : int ...)` -- the carrier values -- are
   only ever fat-dispatched (through the generic `cata` / match-arm carrier
   binding), and within `alg` a *capturing* closure `(fn [s2] (y k s2))` flows
   into the `x` carrier's `k` slot. But the lambda's function-typed parameter
   `k` was dispatched **thin** (`((int64_t(*)(int64_t))k)(...)`), so a fat box
   arriving in `k` was called as code -> SIGSEGV.

2. **Consumer (the carrier-result application site).** The top-level
   `((cata alg e) (fn [r] r) 0)` head is the result of a generic call whose
   declared result is a bare tyvar (`cata ... : B`). It was marked `boxed` by
   #489, but its function-typed argument slots were not marked `arg_fat`, so a
   bare/thin continuation was passed straight into the `int64_t` slot.

## Fix (hkt-cata-function-arg)

- `src/compiler/elab_call.c`, `elab_call_head_expr`: when the chained-call head
  is a generic call whose declared result is a bare `TY_TYVAR` (marked `boxed`),
  also mark each of the recovered carrier fn type's function-typed arguments
  `arg_fat` (skipping `c-fn` pointers). The existing call-site auto-shim then
  boxes a bare/thin fn argument via `EX_FN_TO_FAT` instead of passing a raw thin
  pointer.
- `src/compiler/elab_fns.c`, `elab_fn` typed-param parsing: when a lambda is
  elaborated against an expected function type whose argument at this position
  is itself a (non-`c-fn`) function type -- i.e. the lambda is a function-typed
  carrier value -- mark the function-typed parameter `is_fat`, so `(k ...)`
  reads slot 0 instead of calling the fat box's address as a thin pointer. This
  is symmetric with the call-site `arg_fat` marking and, because both ends flip
  together, also keeps an ordinary direct-application closure-returning HOF
  (`((make n) inc s)`) consistent.
- `src/compiler/emit_expr.c`, fat-dispatch arg emission: when the carrier's
  function-typed argument slot lowers to the `int64_t` carrier but the argument
  value is a fat box (`EX_FN_TO_FAT` / `EX_CLOSURE` / `:ptr<void>` / boxed fn),
  coerce it through `(int64_t)(intptr_t)` so the box pointer lands in the slot
  without `-Wint-conversion` (mirrors the existing `(void *)(intptr_t)` bridge
  for the opposite direction).

Full `bash tests/run.sh`: 1759 passed, 0 failed.
