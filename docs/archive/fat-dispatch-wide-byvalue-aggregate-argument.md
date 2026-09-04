# A wide by-value aggregate ARGUMENT and its lifted thunk disagree across the fat/poly boundary

**Severity: medium today (hard cc error), high for anything that unblocks it.**
On the default path it is a build break, which is loud and safe. The reason it
is worth a report rather than a footnote is what it becomes the moment a sum
rides by value: the same disagreement stops being a type error the C compiler
can see, and turns into a **silent wrong answer**. Measured below.

**Status: RESOLVED 2026-08-27.** Both manifestations fixed by unifying every
fat boundary on the b4box convention -- see **Resolution** at the bottom.
Filed 2026-08-26 while scoping SR4 (recursive sums by value), which this
blocked; SR4's codegen is now green behind its measurement seam.

**It is the argument-position member of a family whose other two are fixed:**

- [fat-closure-dispatch-does-not-handle-struct-return](history/fat-closure-dispatch-does-not-handle-struct-return.md)
  -- RESULT position, aggregate. Fixed.
- [poly-wrapper-forces-int64-args-non-int-fat-sink](history/poly-wrapper-forces-int64-args-non-int-fat-sink.md)
  -- ARGUMENT position, `:float` (a register-class mismatch). Fixed.
- **This one** -- ARGUMENT position, wide by-value aggregate.

The pattern across all three is one sentence: **a lifted thunk is declared with
the concrete C signature, and the dispatch site casts it to something else.**

## Resolution (2026-08-27) -- one convention, spelled in one place

The fix is what the failed experiments pointed at: the disagreement was
per-call-site, so the cure is to make every site consult ONE source of truth.
`thunk_param_slot_c_name` (emit_module.c) now spells a typed-thunk parameter
slot: a WIDE (> 8 byte) by-value aggregate is an `int64_t` box pointer -- the
b4box convention the thunk bodies already spoke -- and everything else keeps
its C type. Every consumer follows automatically or was taught to:

- the typed-thunk TYPEDEF (name and body) spells the slot int64, so the env
  struct `__fn` field, the store casts, and both typed dispatch sites agree by
  construction;
- the two LEGACY dispatch casts (the typedef-declined fallback) spell the same
  slot convention;
- the fn-field call-through (the `TUR_APPLY*_T` expansion site) does too --
  spelling the aggregate there was the SIGSEGV, because that cast is what hid
  the disagreement from the C compiler;
- `fat_dispatch_box_arg` converts each argument once at every dispatch: a
  pass-by-pointer param is retyped (it already is a pointer to the aggregate),
  a by-value expression spills and passes its address (the callee copies at
  entry; the call is synchronous);
- the typed FATSHIMS take the int64 slot and adapt to the bare fn's real ABI
  (`const T *` for a pbp callee, deref-to-value below the pbp threshold);
- the b4box suppression for fn-field closures (`byval_fn_field_closure`) is
  retired -- it existed to keep those thunks in lockstep with the OLD
  aggregate-by-value typedef spelling, and with the spelling gone the
  suppression was the disagreement.

**Both repros confirmed fixed.** Repro 1 (`^fat` sink, default path) prints
106 and is pinned by `tests/fixtures/fat-dispatch-wide-byval-arg` (capturing
closure and bare-fn/fatshim legs, values asserted). Repro 2: under
`TUR_SR4_RECURSIVE_BYVALUE=1` the logic probe prints `2 2 10 20` (all four
values correct) and the FULL suite is 2708 passed / 0 failed with recursive
sums by value.

**Two prerequisites landed with it**, both latent product bugs this report's
sums made ordinary: the by-value size accounting read `ctors[0]` only (a sum
sized by its narrow first variant classified carrier-slot-safe -- the exact
mechanism behind repro 2's silent wrong answer -- and the pbp threshold
depended on declaration order), and several pbp crossings had never seen a
pbp value consumed as a value (ctor args, match-arm returns, the match switch
path, the poly-carrier arg box).

**What did NOT ship: SR4 by default.** With the blocker gone the admission is
a one-line change and the suite is green with it -- so it was measured first,
and the measurement says the trade is memory-for-time: logic bind+walk 400k
passes runs 0.41s -> 0.57s (~1.4x slower) at 116 MB -> 51 MB peak RSS, regex
compile+match 14 ms -> 19 ms. (Follow-up 2026-08-27: profiling that gap found
half of it was the by-value ctors' whole-union zero-init; with the SR4-perf
prologue fix the regression is ~1.13x / ~1.07x -- current numbers live in the
SR plan's SR4 section and the types.c decision record.) By value halves the mallocs but each walk step
deref-copies a 24-48 byte aggregate where the carrier copied one word. Per
the SR plan (justify SR4 against the post-reclamation baseline), the default
stays carrier; `TUR_SR4_RECURSIVE_BYVALUE=1` is the seam that reproduces all
of the above, and the decision record lives at the `is_self_recursive` test
in `adt_sr1_sum_candidate` (types.c).

One find along the way was filed separately and is now also resolved:
[ascribed-fn-param-call-head-name-mismatch](ascribed-fn-param-call-head-name-mismatch.md)
-- the `((:: f (fn [P] int)) v)` spelling broke on a decl/use naming
divergence that predates all of this and hits scalars too. With that fixed,
`tests/fixtures/ascribed-fn-param-call-head` carries the same wide by-value
aggregate through that spelling, so the crossing this write-up had to dodge is
covered rather than avoided.

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
