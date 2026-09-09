---
title: "A fn-typed LOCAL passed to a rank-2 (poly-fn) slot of a by-value spec is wrapped by a file-scope thunk that calls it by name: `'f' undeclared`"
category: Reported
description: "`(defn ap [o : (Option any) f : (fn [any] any)] : (Option any) (fmap o f))` fails to compile: `'f' undeclared (first use in this function)`. The call to the by-value `fmap` spec wraps `f` through make_poly_wrapper_ex, which synthesises a top-level `__poly_N(void *, tur_tagged_t)` whose body calls the inner binding BY NAME -- correct for a global defn, meaningless for a parameter. The same function over `(Option float)` works because it reaches the carrier base and passes `f` straight through as the tur_poly_fn_t it already is."
---

# A local fn value into a rank-2 slot gets a by-name wrapper

**Severity: medium.** Loud -- a C compile error, never a wrong answer. It
blocks passing a function *parameter* (or any local fn value) into a
higher-kinded method whose call resolves to a by-value specialisation, which
is every `fmap`/`bind` over a container of `any` or of a by-value struct.

Found building saffron-lang-plan D8 question 3: the first witness defn passed
its `any` parameter straight into `fmap` and hit this. The witness now casts
explicitly (`(cast __a1 (fn [any] any))`), which produces an expression rather
than a binding and takes the fat-closure pass-through instead -- so Saffron's
dispatch is not blocked by this. Plain Turmeric still is.

## Repro -- plain Turmeric

```turmeric
(defn ap [o : (Option any) f : (fn [any] any)] : (Option any) (fmap o f))
(defn main [] : int
  (println (cast (unwrap-or (ap (:: (some (:: 41 any)) (Option any))
                                (fn [x : any] : any x))
                            (:: 0 any)) int))
  0)
```

```
error: 'f' undeclared (first use in this function)
```

Two controls that pass:

| | result |
|---|---|
| same `ap`, but `(Option float)` / `(fn [float] float)` | 14.5 |
| same call with the fn passed as a CAST expression: `(fmap o (cast g (fn [any] any)))` with `g : any` | 41 |

## Root cause -- measured from the emitted C

The float version's `ap` takes `f` as a `tur_poly_fn_t` already and calls the
carrier base with it unchanged:

```c
static tur_adt_Option__float ap(tur_adt_Option__float o, tur_poly_fn_t f) {
    ... __inst_Functor_fmap_Option((int64_t)(intptr_t)(&__t186), f) ...
```

The `any` version resolves to the by-value spec (an `(Option any)` is a
by-value product), and the argument goes through the rank-2 path in
`elab_call.c` -- the `else` branch beside the dict-clone case:

```c
Binding *wrapper_b = make_poly_wrapper_ex(e, inner_fn_b, inner_arity, nc, ...);
```

`make_poly_wrapper_ex` builds a FILE-SCOPE `__poly_N` whose body is
`(inner_b x0 ...)` -- a call to the binding by name. That is right when
`inner_fn_b` is a global defn, which is the case it was written for (its
comment: "create a poly wrapper thunk for passing a function to a rank-2
param"). For a parameter it emits:

```c
static tur_tagged_t __poly_1439(void *env, tur_tagged_t x0) {
    return _un_unf_1441(x0);        /* the PARAMETER, from file scope */
}
```

Nothing checks that `inner_fn_b` is reachable from file scope.

## Fix directions

1. **Pass a local through as a fat closure.** When `inner_fn_b` is not a
   global defn (it is a parameter, a `let` binding, or otherwise has no
   `source_fn_def` at file scope), do not synthesise a wrapper; set the
   `EX_POLY_WRAP`'s `is_closure` so emission takes the existing pass-through
   -- `(tur_poly_fn_t){ box, *(thunk *)box }` -- which is what a lambda
   argument already gets. The value in the local IS a fat closure box, so
   this is the representation the carrier expects.
2. **Reject with a diagnostic** naming the parameter, if 1 turns out to have a
   representation the pass-through cannot handle. Strictly better than the C
   error, strictly worse than 1.

A fixture wants the repro plus both controls, so a fix cannot make the local
case work by breaking the global-defn case the wrapper exists for.

## Not this bug

`poly-fn-with-any-parameter-is-called-with-the-int64-carrier` (archived) was
the wrapper's PARAMETER TYPE and is fixed; this is the wrapper's CALLEE, and
it does not depend on `any` -- a by-value struct element type reaches the
same spec path.
