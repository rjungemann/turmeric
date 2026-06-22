# Generic catamorphism does not thread a function-typed carrier

**Found by:** turmeric-spices Track C U5 regex prototype
(`spices/regex/src/regex/tree.tur`)
**Verified on:** turmeric 0.22.0, main @ 97dcd86 (post #487 / gap G6)
**Severity:** Low-Medium. Value-carrier catamorphisms are correct (G6, #487).
A catamorphism whose carrier `B` is itself a function type (`(fn [int] int)`,
the "NFA is one cata" matcher `(fn [k s] bool)`, an environment-passing
interpreter, a `Doc = (fn ...)` pretty-printer) used to fail to type-check; the
type error is now fixed, and the non-generic / direct-`fmap` algebra runs, but
the *generic* `cata` driver still mis-dispatches the function carrier at
runtime. A clean direct-recursion workaround exists, so this is an ergonomics
gap, not a blocker.

## Status -- ALL THREE LAYERS FIXED (see below for the residual)

This finding had **three layers**. All three are now fixed.

| Layer | What | Status |
| --- | --- | --- |
| Bug 0 | `(cata fn-alg e)` typed `int`, "not callable" | **FIXED** (#489) |
| Bug A | fn-typed `match`-arm payload not captured by an inner closure | **FIXED** (#489) |
| Bug C | a fn value crossing the generic int64 carrier boundary dispatches *thin*, not *fat* -> SIGSEGV | **FIXED** (the recursive fn-carrier fix) |

The Bug C runtime fix (chained slot-0 fat-dispatch of a carrier-recovered
result, uniform fat-boxing of fn values in parametric ADT fields, and fat
dispatch of fn-typed match-arm payloads) is documented, with its repro and the
two **remaining open residuals**, in
[hkt-cata-function-carrier-recursive-segfault.md](hkt-cata-function-carrier-recursive-segfault.md):

- a captureless algebra arm (e.g. a variable-lookup `(fn [env] env)`) still
  crosses the carrier *thin* (the report's "Bug B"); and
- a program mixing a fn-carrier and a value-carrier `cata` over the same `Fix`
  hits a pre-existing per-spec env-struct name collision.

A sibling closure-ABI gap (Bug B, thin captureless lambda -> fat
`tur_poly_fn_t` coercion) is described under "Related" below.

---

## Bug 0 -- result-type collapse (FIXED)

### Symptom

    error: expression in call head has type `int`, which is not callable
      (do (println ((cata fn-alg (add (lit 3) (lit 4))) 0)) 0))
                    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The error is on the *application* of the cata result: `(cata fn-alg ...)` was
typed `int` rather than as the function carrier `(fn [int] int)`.

### Root cause

`elab_call.c`, the call-result computation for a callee whose declared result
is a bare type variable (`cata [B] ... : B`, `result_kind == TY_TYVAR`). The
result was instantiated correctly to `B := (fn [int] int)` (a `TY_FN`), but the
"is this a concrete carrier-ABI composite worth preserving?" allowlist only
covered `TY_APP` / struct / ADT / `TY_EXISTS` / `TY_FORALL`. A `TY_FN`
instantiation fell through to `call_result_type = TYPE_INT`, collapsing the
function carrier to the bare int64 register class -- exactly the loss the same
code already documents for `(Tuple2 cstr int)` and existentials.

### Fix

`src/compiler/elab_call.c` (`result_is_concrete_composite`, ~line 4337): add
`(result_type.kind == TY_FN)` to the allowlist. A function value is
pointer-width and already inhabits the int64 carrier register class (rax, not
xmm0), so the full `TY_FN` type must be preserved for the call site to apply
the result. This is precisely the per-carrier handling the report's original
"likely cause" predicted.

---

## Bug A -- fn-typed match-arm payload not captured (FIXED)

### Symptom

After Bug 0, the algebra `fn-alg` failed to compile its `AddF` arm:

    error: 'f' undeclared (first use in this function)
    error: 'g' undeclared (first use in this function)
    // static int64_t __fn_1057(int64_t env) {            <-- no env struct!
    //   return ((..)f)(env) + ((..)g)(env); }

The lambda `(fn [env] (+ (f env) (g env)))` in `(AddF f g) -> ...` captures the
match-arm payload bindings `f` and `g`, but was lifted as a *thin* (captureless)
function -- `f`/`g` were never added to its env. The identical shape with an
`int` payload (`(AddF x y) -> (fn [env] (+ x y))`) captured correctly: the
discriminator was the *type* of the captured binding.

### Root cause

`src/compiler/elab_core.c`, `collect_free_vars`, the `EX_CALL` free-variable
gate. A function-typed binding invoked as a callee `(f env)` inside an inner
closure is reached through `call_.fn_binding` (not an `EX_VAR` child), so the
gate has a dedicated clause for it -- but that clause required
`fn_binding->is_param`. A `match`-arm payload binding is `TY_FN` but **not**
`is_param`, so `f`/`g` were dropped, `n_captures` came back 0, and the lambda
was lifted thin.

The `is_param` restriction existed to avoid wrongly capturing a letrec/named-let
self-recursive `TY_FN` binding. Broadening the gate to all non-global `TY_FN`
call heads *does* regress those (56 fixtures: letrec/named-let/reactor), because
the self-name is neither a param nor global.

### Fix

A precise discriminator instead of `!is_global`:

- `src/compiler/expr.h`: new `Binding.is_match_binding` flag.
- `src/compiler/elab_structs.c` (lines 2733, 3182): set it on ADT
  constructor-pattern field bindings.
- `src/compiler/elab_core.c` (~line 634): capture a `TY_FN` call-head binding
  that is `is_param` **or** `is_match_binding`. A letrec/named-let self-name is
  neither, so it is left to the recursion machinery.

Full `bash tests/run.sh` is unchanged by Bugs 0+A: `1703 passed, 45 failed`,
identical to the clean baseline (the 45 are pre-existing `-lturi` env failures
in httpd/reactor fixtures, unrelated to this work).

---

## Bug C -- function carrier dispatches thin across the generic boundary (OPEN)

### Symptom

With Bugs 0+A fixed, the full generic-`cata` repro type-checks and emits C, but
SIGSEGVs at runtime:

    int64_t (*__call_head_1065)(int64_t) =
        cata__spec__..(.., add(lit(3), lit(4)));   // returns an int64 carrier
                                                    // holding a FAT closure box
    .. __call_head_1065(0) ..                       // called THIN -> jump into
                                                    // the env block -> SIGSEGV

The cata result is a *fat* closure (the algebra returns capturing lambdas), but
because it crossed the generic int64 carrier boundary the call site dispatches
it as a *thin* function pointer.

### Root cause

`src/compiler/elab_call.c`, `elab_call_head_expr` (~line 728): when the call
head is the *result of a call* (`EX_CALL`) whose static type is `TY_FN`, the
chained-application dispatch reads `expr_closure_fn_binding(source_expr)` to
decide thin-vs-fat. For a generic callee whose result is a bare tyvar carrier
(`cata`'s `: B`), there is no single `returns_closure_fn_binding` thunk (the
body returns `(alg ...)`), so it resolves NULL and the application falls back to
a thin pointer call. The fat-dispatch path in `emit_expr.c` (~line 2452) also
needs a *single known thunk binding*, which a value reconstructed from the
int64 carrier (or returned via differing `match` arms) does not have.

The same shape reproduces *without* generics, via direct structural recursion
whose body is a `match` returning closure arms: `expr_closure_fn_binding`
(`src/compiler/elab_core.c`, ~line 1926) has no `EX_MATCH` case (falls to
`default: NULL`), so a function whose body is `(match e (..) (fn ..) (..) (fn
..))` never records `returns_closure_fn_binding`, and chained application of its
result dispatches thin -> SIGSEGV.

### Fix direction

The fat-vs-thin distinction is erased at the int64 carrier boundary (and at a
multi-arm `match`). A function value crossing that boundary must dispatch via
the runtime slot-0 read `((sig)box[0])(box, args)` rather than a statically
named thunk. Two sub-pieces:

1. **`match`-returning-closure** (direct recursion): teach
   `expr_closure_fn_binding` an `EX_MATCH` case. Because the arms are distinct
   lambdas, it cannot return one canonical thunk binding; the dispatch needs to
   switch to the slot-0 form when the arms disagree but all carry the same
   `(fn ...)` signature.
2. **generic carrier**: when an `EX_CALL` head has a `TY_FN` type recovered from
   a bare-tyvar carrier result, mark it for slot-0 fat dispatch unconditionally
   (any closure that crossed the carrier is boxed fat).

Both are delicate closure-ABI changes (the thin path is the common, snapshot-
heavy case), so they are recorded here rather than landed alongside the
type-level fixes.

### Minimal repro (segfaults; Bugs 0+A fixed, Bug C open)

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

    (defn cata [B] [alg : (fn [(ExprF B)] B) e : Expr] : B
      (alg (:: (fmap (unroll-e e) (fn [c : Expr] : B (cata alg c))) (ExprF B))))

    (defn fn-alg [l : (ExprF (fn [int] int))] : (fn [int] int)
      (match l
        (LitF n)   (fn [env : int] : int n)
        (AddF f g) (fn [env : int] : int (+ (f env) (g env)))))

    (defn lit [n : int] : Expr (Roll (LitF n)))
    (defn add [x : Expr y : Expr] : Expr (Roll (AddF x y)))

    (defn main [] : int
      (do (println ((cata fn-alg (add (lit 3) (lit 4))) 0)) 0))   ;; want 7

### Controls that DO work now

- Value carrier through the same generic `cata` (`size-alg : (fn [(ExprF int)]
  int)`): correct.
- The function-producing algebra applied **directly** (no generic cata):
  `((fn-alg (:: (AddF a b) (ExprF (fn [int] int)))) 0)` returns `7` -- this is
  the Bug A fix; the capturing algebra lambda is now lifted fat.

## Impact / workaround

Affects only folds whose carrier is a function: CPS matchers ("NFA is one
cata"), environment-passing interpreters, pretty-printers returning
`Doc = (fn ...)`. Workaround in spice code: write these as direct structural
recursion over the Fix tree **and** avoid chaining the application of a
recursive call's closure result through a value the carrier/`match` boundary has
erased (e.g. apply it in a `let`-bound, concretely-typed step). regex/tree's
`re-matches?` is green; its comment points here.

## Related

**Bug B -- thin captureless lambda coerced to a fat `tur_poly_fn_t`.** Passing a
*captureless* lambda where a `tur_poly_fn_t` is expected
(`(combine (fn [e] 3) (fn [e] 4))` for `combine [f g] : (fn ...)`) builds
`(tur_poly_fn_t){ p, ((int64_t*)p)[0] }` (`src/compiler/emit_expr.c`,
`EX_POLY_WRAP` is_closure path, ~line 5213) which dereferences the *code*
address of the thin function as if it were a closure env block -> SIGSEGV. The
correct thin->fat bridge is `EX_FN_TO_FAT` (a `{shim, orig_fn_ptr}` box); the
gap is that a thin lambda in this position is routed through the is_closure
deref instead. Capturing lambdas in the same position work. Same closure-ABI
cluster as Bug C.
