---
title: Eq Per-Call-Site Synthesis Passes a Bare Comparator Pointer to a ^fat Sink
category: Reported Bug
description: The constrained-Eq per-call-site synthesis dispatcher (F3-5/F3-6) lowers a recursive `.eq?` on a TY_APP element type by emitting a bare captureless comparator function pointer `(void*)(intptr_t)(__fn_NNN)` directly as the comparator argument, bypassing the ^fat auto-box machinery. If the receiving comparator parameter is migrated to ^fat (fat-dispatched), that bare pointer is read as a fat box and the program segfaults. This is why option-eq? / pair-eq-carrier? / mutmap-eq? cannot be moved onto ^fat in Phase 1 of the closure-representation-unification plan, and is the concrete motivation for that plan's Phase 3 (box captureless fns at fat-dispatched sinks).
---

# Eq Per-Call-Site Synthesis Passes a Bare Comparator Pointer to a ^fat Sink -- Reported Bug

> **Status:** RESOLVED (2026-06-03) in closure-representation-unification
>   Phase 3 / Option B, sub-phase B-3. The synthesis dispatcher now boxes every
>   synthesized value/element comparator (`box_synth_comparator` ->
>   `EX_FN_TO_FAT`) before building its direct `EX_CALL`, and the `*-eq?`
>   carrier helpers (`option-eq?`, `vec-eq?`, `list-eq?`, `result-eq?`,
>   `pair-eq-carrier?`, `set-eq-cmp?`, `mutmap-eq?`, `map-eq?` / `map-eq-k?` /
>   `map-eq-dynamic`) take `^fat` value comparators and fat-dispatch through
>   slot 0 -- including `tur_hamt_eq_dynamic` in the C runtime. Both the
>   instance-body path and the synthesis path now agree on the fat box
>   representation. Regression fixture:
>   `tests/fixtures/eq-carrier-capturing-comparator` (a *capturing* comparator
>   passed directly to `option-eq?` / `vec-eq?` / `mutmap-eq?`). The MapKey
>   `keyeq` carrier stays thin (it is a constant carrier-ABI fn pointer, not a
>   user closure). `bash tests/run.sh`: 0 FAIL.
> **Found:** 2026-06-03, while executing Phase 1 of
>   [closure-representation-unification-plan.md](../upcoming/closure-representation-unification-plan.md)
> **Severity:** Medium -- not a defect in the current tree (the synthesis
>   path and the helpers agree on the thin bare-pointer convention today), but
>   a hard **segfault** the moment a fat-dispatched comparator sink is
>   introduced. It is the precise blocker that scopes the `*-eq?` helpers out
>   of Phase 1.
> **Related:**
> - [closure-representation-unification-plan.md](../upcoming/closure-representation-unification-plan.md) (Phase 3 / Option A)
> - [arrow-thin-call-segfaults-capturing-closures.md](arrow-thin-call-segfaults-capturing-closures.md)

---

## Summary

The constrained-`Eq` dispatcher (cross-plan-followups F3-5/F3-6) lowers a
recursive `.eq?` on a parameterized element type by **synthesizing** a
comparator function and passing it to the container's `*-eq?` carrier helper.
The synthesized comparator is a top-level captureless static function, and the
dispatcher emits it **as a bare pointer**:

```c
/* Option[Vec[int]] .eq? -- main()'s call site */
puts((option_eq_(o1, o2, (void *)(intptr_t)(__fn_862))) ? "true" : "false");
```

where `__fn_862` is `static bool __fn_862(int64_t, int64_t)`.

This is the **thin** representation. It is correct only while `option_eq_`
invokes its comparator through a matching bare cast
`((bool(*)(int64_t,int64_t))(intptr_t)cmp_fn)(...)`.

By contrast, the `Eq[Option]` *instance body* -- `(option-eq? x y (fn [a b]
(= a b)))` -- routes its `(fn ...)` literal through the normal elaborator,
which (when the parameter is `^fat`) boxes it into a fat shim:

```c
__t2[0] = (int64_t)(intptr_t)__tur_fatshim_bool_int64_t_int64_t;
__t2[1] = (int64_t)(intptr_t)__fn_622;
return option_eq_(x, y, (void *)(intptr_t)(__t2));   /* fat box */
```

So the two paths into `option_eq_` disagree about representation the instant
the comparator parameter is fat-dispatched: the instance-body path boxes, the
synthesis path does not.

## Repro

`tests/fixtures/option-of-tvec-eq` (run with `-Xdata-literals`):

```turmeric
(let [o1 (:: (some v1) (Option (Vec int)))
      o2 (:: (some v2) (Option (Vec int)))]
  (println (.eq? o1 o2)))   ; recursive Eq on Option[Vec[int]]
```

With `option-eq?` migrated to a `^fat` comparator (so its body does
`TUR_APPLY2_T(bool, int64_t, int64_t, cmp_fn, ...)`), this **segfaults**:
`TUR_APPLY2_T` reads slot 0 of `__fn_862` (a code address, not a box) as the
thunk and jumps to garbage. With the helper left on the thin bare-pointer
convention, it prints `true / false / false` as expected.

## Root cause

The synthesis dispatcher emits the comparator argument as a raw
`(void*)(intptr_t)(__fn_NNN)` cast, independent of whether the callee's
parameter is `^fat`. The `^fat` auto-box (`EX_FN_TO_FAT` / per-signature
`__tur_fatshim_*`) only fires for arguments the elaborator sees as fn-typed at
a `^fat` call site; the dispatcher-synthesized argument is lowered directly and
never reaches that boxing branch.

This is the same captureless-fn-reaches-a-fat-sink gap described as report #5
in the unification plan -- here the captureless fn is *compiler-synthesized*
rather than user-written, but the failure mode is identical.

## Impact on the unification plan

Phase 1 migrates thin-call closure consumers onto `^fat` + fat dispatch.
`stdlib/arrow.tur` and `stdlib/option.tur`'s `option-map` migrate cleanly --
their closures are user-supplied and reach the helper either as a literal
(auto-boxed) or as an already-fat capturing value.

`option-eq?`, `pair-eq-carrier?`, and `mutmap-eq?` cannot: they are consumed by
this synthesis dispatcher, which hands them bare captureless pointers. Moving
them to `^fat` without first teaching the dispatcher to box is a hard segfault
regression in existing typeclass dispatch. They therefore stay on the thin
convention until Phase 3.

## Proposed fix directions

1. **Phase 3 / Option A (preferred):** when the synthesis dispatcher targets a
   `^fat` comparator parameter, emit the per-signature fat-shim box around the
   synthesized function instead of a bare `(void*)` cast -- reusing the
   existing `__tur_fatshim_<R>_<A...>` machinery. Then `option-eq?` /
   `pair-eq-carrier?` / `mutmap-eq?` can move to `^fat` and uniformly
   fat-dispatch, completing their Phase 1 migration.
2. **Phase 3 / Option B:** a first-class closure type so the synthesized
   comparator is uniformly fat at construction, removing the bare/fat split at
   its root.

## Validation when fixed

- `tests/fixtures/option-of-tvec-eq` (and the other recursive-`Eq`-on-`TY_APP`
  fixtures) print their expected output with `option-eq?` /
  `pair-eq-carrier?` / `mutmap-eq?` on `^fat` comparators.
- New fixtures pass a **capturing** comparator directly to each `*-eq?`
  helper and round-trip (the latent crash these helpers have today).
- `bash tests/run.sh` zero `FAIL`.
