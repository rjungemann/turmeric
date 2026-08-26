# A wide by-value aggregate ARGUMENT and its lifted thunk disagree across the fat/poly boundary

**Severity: medium today (hard cc error), high for anything that unblocks it.**
On the default path it is a build break, which is loud and safe. The reason it
is worth a report rather than a footnote is what it becomes the moment a sum
rides by value: the same disagreement stops being a type error the C compiler
can see, and turns into a **silent wrong answer**. Measured below.

**Status:** OPEN. Filed 2026-08-26 while scoping SR4 (recursive sums by value),
which this blocks. Diagnosed with a minimal repro and two candidate fixes tried
and rejected; not fixed.

**It is the argument-position member of a family whose other two are fixed:**

- [fat-closure-dispatch-does-not-handle-struct-return](../archive/history/fat-closure-dispatch-does-not-handle-struct-return.md)
  -- RESULT position, aggregate. Fixed.
- [poly-wrapper-forces-int64-args-non-int-fat-sink](../archive/history/poly-wrapper-forces-int64-args-non-int-fat-sink.md)
  -- ARGUMENT position, `:float` (a register-class mismatch). Fixed.
- **This one** -- ARGUMENT position, wide by-value aggregate.

The pattern across all three is one sentence: **a lifted thunk is declared with
the concrete C signature, and the dispatch site casts it to something else.**

## Repro 1 -- default path, hard error

```turmeric
(defdata P :copy (P [a : int b : int c : int]))          ;; 24 bytes -- wide

(defn sum-p [p : P] : int (match p (P a b c) (+ a (+ b c))))

(defn apply-it [^fat f : (fn [P] int) v : P] : int (f v))

(defn mk [bump : int] : (fn [P] int)
  (let [lb bump]
    (fn [p : P] (+ lb (sum-p p)))))                       ;; CAPTURING

(defn main [] : int
  (println (apply-it (mk 100) (P 1 2 3)))                 ;; expect 106
  0)
```

```
error: incompatible type for argument 2 of '*(int64_t (**)(void *, tur_adt_P))f'
note: expected 'tur_adt_P' but argument is of type 'const tur_adt_P *'
```

`v` is a pass-by-pointer parameter (`const tur_adt_P *`), the fat dispatch hands
it over as-is, and the typed thunk wants the aggregate. Two sites, two answers.

## Repro 2 -- the same defect as a SILENT WRONG ANSWER

Force recursive sums by value (`AdtDef.is_self_recursive` is the gate in
`adt_sr1_sum_candidate`, `types.c`) and `tests/fixtures/logic-reify` prints:

```
1        <- correct
0        <- WRONG, expected 10
20       <- correct
```

No diagnostic, no crash, no sanitizer report. The substitution chain is
truncated: `chain-len` reads 1 where it should read 2, and `subst-next` reads 0
where it should read 2 -- the second conjunct ran against a state that lost the
first conjunct's binding.

Why it goes quiet here and loud in repro 1: the value crosses through
`tur_poly_fn_t` (the untyped `:fn` carrier) rather than a `^fat` slot. That call
site **boxes** the aggregate and casts slot 0 through a function-pointer type:

```c
/* stdlib/logic.tur st-bind, emitted */
f.fn(f.env, (int64_t)(({ tur_adt_Subst *__tur_pbox = malloc(sizeof(tur_adt_Subst));
                         *__tur_pbox = (v_1442); (int64_t)(intptr_t)__tur_pbox; })))
```

while the thunk it is calling is

```c
static int64_t __fn_1541(void *__env_p, tur_adt_Subst s) { ... }
```

The cast `(int64_t(*)(void*,int64_t))` makes the two type-check. At runtime the
callee reads the first 16 bytes of a `tur_adt_Subst` out of the register that
holds the box POINTER. Whatever is there becomes the substitution.

**This was invisible for as long as it has existed** because a multi-variant ADT
rode the int64 carrier, so `type_c_name(Subst)` was `int64_t`, the thunk really
did take an int64, and the cast was the identity. The bug is not new; the
representation that hid it is what changed.

## Root cause

Two independent sites decide how a lambda parameter travels, and nothing makes
them agree:

1. **The lifted thunk's declaration.** A lifted lambda `__fn_NNNN` is declared
   from its own parameter type, so `(fn [state : Subst] ...)` becomes
   `int64_t __fn_1541(void *, tur_adt_Subst)`.
2. **The dispatch site.** `tur_poly_fn_t` is `{ void *env; int64_t (*fn)(void *,
   int64_t); }`, and the `^fat` path spells its own cast
   (`*(int64_t (**)(void *, tur_adt_P))f`). Neither consults (1).

`use_typed_thunk_abi` / `thunk_type_has_concrete_c_abi` (`emit_module.c:330`)
admits `TY_ADT` on nothing more than `def != NULL`, so an aggregate parameter is
accepted into a typed thunk signature that a poly call site will then
misdescribe.

## Two fixes tried and REJECTED -- do not re-derive these

**1. Decline by-value aggregates in `thunk_type_has_concrete_c_abi`,** so the
thunk takes the int64 carrier the caller already hands over.

Rejected: it regresses **5 fixtures on the default path** --
`catch-unwind-aggregate-thunk`, `defstruct-fn-field-struct-cstr`,
`dot-parametric-fn-field-call`, `lens-compose-wide-byvalue-get-put`,
`make-struct-parametric-fn-field-infer`. Those reach the same thunks through
callers that DO know the concrete signature and use the typed thunk typedef
correctly. The typed thunk ABI is not the problem; it is right for its own
callers.

**That is the load-bearing conclusion: this is a per-CALL-SITE disagreement, not
a per-TYPE one.** The fix belongs at the poly/fat dispatch site (stop casting a
typed thunk to the int64 signature -- either call it through its real typedef,
or emit a shim that unboxes and forwards), not in the predicate that decides the
thunk's signature.

It also does not go far enough on its own: the lifted-lambda declaration path
picks the parameter's C type from the lambda's own annotation and never consults
`use_typed_thunk_abi`, so the thunks kept their aggregate parameters and repro 2
still printed `0`.

**2. Deref a pass-by-pointer source in the aggregate box helper**
(`emit_agg_box`'s caller, `emit_expr.c`) so `*__tur_pbox = *(v)` instead of
`*__tur_pbox = (v)`. Correct as far as it goes -- assigning a `const T *` to a
`T` is never right -- but it only moves repro 1 to the next error, and is
unreachable without a fix for the real disagreement. Worth folding into whatever
does fix this.

## Why it matters beyond itself

**This is the only thing between SR4 and being done.** With recursive sums
admitted to the by-value path, the whole suite is **2705 passed, 2 failed** --
and both failures are this bug, in the one module (`stdlib/logic.tur`) that
reaches a wide by-value aggregate through an untyped `:fn`. Every other
recursive sum in the tree (`Term`, `Regex`, `RxCls`, `RxPos`, `RxStrs`, the
fixture trees and lists) already lowers by value and runs correctly.

That is a much smaller blocker than
[sr1-gate-results.md](../upcoming/sr1-gate-results.md) estimated -- it predicted
a `logic.tur` rewrite of unknown size. One of the two ascriptions it flagged was
indeed just bad typing and is now fixed in stdlib (`fmap-goal-raw`'s callback
was a bare `:fn`; giving it its real `(fn [Subst] Subst)` type deleted the
carrier reinterpret outright). What remains is not a source problem at all.

## Fix directions

1. **Make the dispatch site call the thunk through its real signature.** The
   typed thunk typedef already exists and is already named
   (`typed_thunk_typedef_name`); the poly path needs to reach for it instead of
   casting to the int64 shape. Narrowest fix, and it is where the disagreement
   actually is.
2. **Or emit an unboxing shim per aggregate-parameter thunk** -- an
   `int64_t shim(void *env, int64_t boxed)` that derefs and tail-calls the typed
   thunk -- and register THAT in the `tur_poly_fn_t` slot. This is what
   `__tur_fatshim*` already does for arity/shape adaptation, so there is a
   precedent to follow rather than a mechanism to invent.
3. Fold in the pass-by-pointer box deref from rejected fix 2.

A regression fixture should assert **both** manifestations: repro 1 (builds and
prints 106) and repro 2's shape (a wide aggregate through an untyped `:fn`,
checking the VALUE, since this defect's dangerous form is a wrong answer rather
than a build break).

## Guides to update when fixed

- [docs/guides/fat-closure-annotation-guide.md](guides/fat-closure-annotation-guide.md)
  -- what may cross a `^fat` / `:fn` boundary, and in what representation.
- [docs/guides/value-representations-guide.md](guides/value-representations-guide.md)
  -- a repr cell for the aggregate-argument crossing, alongside the two closed
  cells for the result-position and float-argument siblings.
