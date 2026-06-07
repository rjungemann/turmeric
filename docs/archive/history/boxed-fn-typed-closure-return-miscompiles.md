---
title: A plain `defn` returning a non-boxed `(fn [..] R)` whose body is a capturing closure miscompiles its C return/consumer types
category: Bug Report
description: When a plain (non-`__inst_`) defn declares its return type as a concrete, non-boxed function type (e.g. `(fn [float] float)`) but its body yields a *capturing* closure (a fat box), the emitted C signature uses the function's *result* type (`double`) instead of the fat-closure carrier. The producer then `return`s a `void *` where `double` is expected (hard `cc` error for float/cstr), and the consumer initializes a `R (*)(A)` fn pointer from the carrier (a `-Wint-conversion` "works by luck" for int). Surfaced while executing docs/reported/arrow-compose-float-closure-int64-thunk-mismatch.md (Direction B): fn-typed float closures are a prerequisite for a register-class-correct generic `>>>`.
---

# Boxed fn-typed closure return miscompiles (capturing body)

> **Status:** RESOLVED (2026-06-05) via Direction B, as Stage 0 of
> `docs/upcoming/poly-closure-result-specialization-plan.md`. Found while
> implementing Stage A of that plan (Direction B of the arrow-compose float
> report). This was a genuine pre-existing defect, independent of that work: it
> reproduced with a fully monomorphic `defn` and no generics. Recorded here per
> the CLAUDE.md "Reporting Bugs" rule.
>
> **Fix:** `src/compiler/elab_fns.c` -- when a plain (non-`__inst_`) defn's body
> yields a capturing closure (`returns_boxed_closure`) and its declared return is
> a non-boxed, non-`^fat` `TY_FN`, the declared result type is now marked `boxed`
> during elaboration. `type_c_name(TY_FN boxed)` lowers to `void *`, so the
> signature, forward declaration, and the consumer let-binding all carry the
> void* fat-closure carrier in lockstep; the producer's `return <box>` then
> matches. Fixture: `tests/fixtures/boxed-fn-typed-closure-return/` (float prints
> `1.55`, int prints `8`, both build clean with no `-Wint-conversion`).
>
> **Severity:** latent miscompile / hard error. For a `:float`- or
> `:cstr`-result closure the emitted C is a hard `cc` error (program does not
> build). For an `:int`-result closure it compiles only with a
> `-Wint-conversion` warning and "works by luck" because a heap pointer round
> trips through `int64_t` -- the same register-class-luck class of defect the
> arrow-compose report flags.

## Summary

A plain `defn` whose **declared return type is a concrete, non-boxed function
type** and whose **body evaluates to a capturing closure** (an `EX_CLOSURE` fat
box, not a thin/non-capturing fn pointer) is lowered with the wrong C return
type. The return-type lowering picks `type_c_name(TY_FN non-boxed)` -- which is
the function's *result* type's C name (`double` for `(fn [float] float)`) --
instead of the fat-closure carrier (`void *` / `int64_t`). The body actually
returns the heap fat box (`void *`), so producer, signature, and consumer
disagree.

## Minimal repro

### Float result -- hard `cc` error

```turmeric
;; boxret.tur
(defn make-scale [k : float] : (fn [float] #{} float)
  (fn [x : float] : float (* x k)))         ; captures k -> fat box

(defn main [] : int
  (let [f (make-scale 0.5)]
    (println (f 3.1)))
  0)
```

Emitted C (abridged):

```c
static double make_hyscale(double k) {                 /* WRONG: should be void* */
    struct __env_893 *__t25 = (struct __env_893 *)malloc(sizeof(struct __env_893));
    __t25->__fn = (tur_thunk_double_double_t)__fn_891;
    __t25->k = k;
    void *__t26 = __t25;
    return __t26;                                       /* void* returned as double */
}
...
double (*f)(double) = make_hyscale(0.5);                /* WRONG carrier at consumer */
```

- **Observed:** `cc` error:
  `incompatible types when returning type 'void *' but 'double' was expected`
  (and, at the consumer, a `double (*)(double)` initialized from a closure box).
- **Expected:** `make_hyscale` returns the fat-closure carrier (`void *`), the
  consumer binds it as the carrier, and `(f 3.1)` fat-dispatches through slot 0
  -- result `1.55`.

### Int result -- compiles only with a warning ("works by luck")

```turmeric
(defn make-adder [k : int] : (fn [int] #{} int)
  (fn [x : int] : int (+ x k)))
(defn main [] : int
  (let [f (make-adder 5)] (println (f 3))) 0)   ; prints 8, but see warning
```

```c
static int64_t make_hyadder(int64_t k) {               /* int64_t happens to fit the box ptr */
    ...
    return (int64_t)(intptr_t)__t26;                    /* carrier cast inserted for int only */
}
...
int64_t (*f)(int64_t) = make_hyadder(5);                /* -Wint-conversion */
```

- **Observed:** prints `8`, with
  `warning: initialization of 'int64_t (*)(int64_t)' from 'int64_t' makes
  pointer from integer without a cast [-Wint-conversion]`.
- The int path round-trips because a heap pointer fits in `int64_t`; the float
  path has no such luck because the carrier and the `double` register class
  differ.

## Root cause (file:line)

`emit_fn_return_typedef` (`src/compiler/emit_core.c:175-196`) correctly returns
NULL for a capturing-closure body (`body_yields_thin_fn` is false at
`emit_core.c:192`) -- a fat box must not be typed as a thin fn pointer. But the
plain-defn call site does not have a carrier fallback:

`src/compiler/emit_fns.c:394-417` (the signature emit) -- when `fn_ret_td`
(from `emit_fn_return_typedef`) is NULL and the body is not inline-C, it falls
to `emit_type_c_name(ctx, rft)` at `emit_fns.c:414`. For `rft = (fn [float]
float)` non-boxed, `type_c_name` returns the **result** type's C name,
`double`. The matching forward declaration (`emit_module.c:1332`) and the
consumer let-binding lower the same way, so all three agree on the *wrong*
type.

The `__inst_` (typeclass-method) variant of this exact problem was already
fixed: `emit_inst_fn_return_carrier` (`emit_core.c:214-221`) returns the
fn-ptr typedef for a thin body, else falls back to `"int64_t"` (the carrier).
Plain defns have **no** equivalent fallback -- they drop straight to
`type_c_name`. The fix is to give plain defns the same carrier fallback.

## Proposed fix directions

- **A (preferred):** generalize `emit_inst_fn_return_carrier` (or add a sibling)
  so any defn whose declared return is a non-boxed `TY_FN` and whose body does
  not yield a thin fn pointer uses the fat-closure carrier (`void *`, or
  `int64_t` to match the existing int spelling) for: the signature
  (`emit_fns.c:414`), the forward decl (`emit_module.c:1332`), and the consumer
  let-binding's declared type. Ensure the producer's `return` and the consumer's
  init both spell the carrier so no `-Wint-conversion`/incompatible-type
  diagnostic remains. The `__inst_` path proves the shape; this widens it to the
  `name && strncmp(... "__inst_")` gate at `emit_core.c:216-218`.
- **B:** mark such a return type `boxed` during elaboration when the body is a
  capturing closure, so `emit_fn_return_typedef` and `type_c_name` already steer
  onto the void* carrier (no emit-site special-case). Larger blast radius
  (touches every `type_c_name(TY_FN)` consumer) but removes the asymmetry at the
  source.

## Validation for a fix

- New fixture: the float repro above builds clean (no `cc` error, no
  `-Wint-conversion`) and prints `1.55`; the int repro builds with **no**
  warning and prints `8`.
- Emitted C: `make_hyscale` returns the carrier; the consumer binds the carrier
  and fat-dispatches; no `double (*)(double)` initialized from a box.
- Regenerate any `tests/fixtures/*/expected.c` whose closure-returning defns
  shift from `int64_t`/`double` to the carrier spelling; `bash tests/run.sh`
  green (leak detection on).

## Why this matters for the arrow-compose report

Direction B of
`docs/reported/arrow-compose-float-closure-int64-thunk-mismatch.md` wants a
register-class-correct generic `>>>` that composes `:float -> :float` closures.
For the per-monomorphization specialization to trigger, the argument closures
must carry their `(fn [float] float)` type rather than the type-erased
`ptr<void>` carrier (a `ptr<void>` argument binds no tyvars, so no float
specialization is interned and the inner body stays on the int64 thunk ABI).
But producing such fn-typed float closures from a `defn` hits *this* bug first,
so this defect is a prerequisite for Direction B's Stage E. See the updated
`docs/upcoming/poly-closure-result-specialization-plan.md`.
</content>
