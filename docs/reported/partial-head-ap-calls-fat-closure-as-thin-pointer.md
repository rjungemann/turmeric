# Compiled `ap` over a partial-head instance segfaults

**Severity:** high -- a well-typed program crashes at runtime on the compiled
path; `tur --interpret` prints the right answer. Blocks
typeclass-superclasses-plan SC8b's `Monad` over `Applicative` retrofit, which
needs an `Applicative [(Result _ B)]` instance in the stdlib.

## Summary

An `Applicative` instance over a *partially applied* head such as
`(Result _ B)` or a user `(Either _ E)` gets no by-value `ap` specialization.
The call site passes the address of a by-value monomorph struct
(`tur_adt_Either__fn1_int__int__int`) into the instance's generic carrier body,
and that body reads the `Right` payload as a thin C function pointer and calls
it. The payload is a two-word fat closure (shim + fn), so the call jumps into
data.

`Option`'s `ap` does not crash because it has a full head, and
[ap-fn-in-container-monomorphization-plan](../archive/ap-fn-in-container-monomorphization-plan.md)
gave full heads a by-value `__spec` clone that invokes the wrapped function
through its typed signature. That plan named `Result<E, fn a b>` as needing the
same representation but only shipped `Option`.

Before this gap is closed, adding `Applicative [(Result _ B)]` to the stdlib
would turn today's clean "no instance" type error into a runtime segfault for
anyone who calls `ap` on a `Result`.

## Repro

A user type is needed because the orphan rule refuses an `Applicative
[(Result _ B)]` instance outside `stdlib/result.tur`.

```turmeric
(defdata Either :copy [A E] (Right A) (Left E))

(definstance Functor [(Either _ E)]
  (fmap [c g] (match c (Right v) (Right (g v)) (Left e) (Left e))))

(definstance Applicative [(Either _ E)]
  (pure [x] (Right x))
  (ap [ff fa]
    (match ff
      (Right f)
      (match fa (Right a) (Right ((:: f (fn [a] b)) a)) (Left e) (Left e))
      (Left e)
      (Left e))))

(defn main [] : int
  (let [dbl (:: (Right (fn [x : int] : int (* x 2))) (Either (fn [int] int) int))
        a21 (:: (Right 21) (Either int int))]
    (println (match (ap dbl a21) (Right v) v (Left e) (- 0 e))))
  0)
```

```
$ tur run repro.tur          # Segmentation fault, exit 139
$ tur --interpret repro.tur  # 42
```

The `(:: f (fn [a] b))` ascription is needed because without it the body does
not type-check ("'f' is not a function or continuation"): the partial head
does not carry the function type into the `Right` pattern the way `Option`'s
full head does. Rewriting the body as `(fmap fa f)` fails differently
("match: arm types are incompatible -- expected app, got app").

Two related symptoms on the same shape, from the `Result` variant of this
probe:

- Passing `ap`'s result to a `(Result int int)` parameter fails the type
  checker with `expected (type-app (type-app Result int) int), got (type-app ?
  ?)`, even under a declared `let` type. `fmap` and `bind` on `Result` ground
  their result types fine; only `ap` does not.
- With an explicit `(:: (ap ...) (Result int int))` ascription the checker is
  satisfied, but the emitted C passes an `int64_t` carrier where the callee
  takes the by-value `tur_adt_Result__int__int`, and the C compiler rejects it.

## Root cause (from the emitted C)

The instance body is emitted once, unspecialized, over carriers:

```c
static int64_t __inst_Applicative_ap_Result_tyvar(int64_t ff, int64_t fa) {
    tur_adt_Result *__scrut = (tur_adt_Result *)(intptr_t)(ff);
    ...
    int64_t f_1162 = (int64_t)__scrut->as.Ok._0;
    ...
    int64_t (*__call_head_1164)(int64_t) = (int64_t (*)(int64_t))(intptr_t)(f_1162);
    int64_t __ps_48 = (((int64_t (*)(int64_t))(intptr_t)__call_head_1164)(a_1163));
```

and the call site hands it the address of a by-value monomorph of a different
layout:

```c
tur_adt_Result__fn1_int__int__int __t183 = dbl_1621;
tur_adt_Result__int__int __t184 = a21_1622;
int64_t __ps_185 = (__inst_Applicative_ap_Result_tyvar(
    (int64_t)(intptr_t)(&__t183), (int64_t)(intptr_t)(&__t184)));
```

So there are two mismatches: the by-value struct is read through the carrier
`tur_adt_Result *` layout, and the fat closure stored in `Ok` is called as a
thin function pointer. The by-value re-narrowing in `emit_expr.c`'s match
lowering (the SR2a block that cites `hkt-ap-fn-in-container`) only applies
inside a specialized body, and no specialization is minted for the partial
head.

## Fix directions

1. Extend the `ap` by-value specialization to partial-head instances, so
   `(Result _ B)` and user `(Either _ E)` get the same `__spec` clone `Option`
   does. This is the remaining item of the archived monomorphization plan.
2. Ground `ap`'s `(f b)` result from the function element's result type for
   partial heads, which fixes the `(type-app ? ?)` result.
3. Until then, the call site should refuse rather than crash: an `ap` whose
   instance has no by-value spec and whose `ff` is a by-value monomorph could
   be a compile-time error.

Once fixed, add `Applicative [(Result _ B)]` to `stdlib/result.tur` (the body
in the repro above, with `ok`/`err`) with a fixture exercising `ap` on
`Result` under both harnesses, and SC8b's `Monad` over `Applicative` step is
unblocked.
