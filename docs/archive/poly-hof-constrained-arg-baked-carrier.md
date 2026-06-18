# Polymorphic HOF taking a constrained-poly-as-value bakes carrier ABI

**Status:** RESOLVED 2026-06-18 (fixed in the same session). See the
"Resolution" section at the bottom for the landed fix; validated by
`tests/fixtures/poly-hof-constrained-arg-spec/`.

**Status (original):** OPEN 2026-06-18.
**Severity:** Hard compile error in emitted C (clang `-Wint-conversion` /
incompatible-pointer-types). Loud, not a silent miscompile -- the
generated C does not build, so no runtime miscompile reaches the user.
Still a real M5-class defect: a perfectly reasonable Turmeric program
fails to lower.
**Discovered:** while scoping M5 of
[`end-to-end-monomorphization-plan.md`](../upcoming/end-to-end-monomorphization-plan.md).

## Repro

```turmeric
(defstruct Box [n : int])
(defclass Size [a] (size [x] : int))
(definstance Size [int] (size [x] -1))
(definstance Size [Box] (size [x : Box] 7))

(defn count-it [^Size A] [x : A] : int (size x))

;; HOF is itself polymorphic over A.
(defn apply-it [A] [f : (fn [A] int) a : A] : int (f a))

(defn main [] : int
  (println (apply-it count-it (make-struct Box 0)))   ; want 7
  (println (apply-it count-it 42))                    ; want -1
  0)
```

## Observed

`tur build` fails:

```
error: incompatible type for argument 2 of 'apply_hyit'
   printf("%lld\n", (long long)(apply_hyit(
       (tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_910 },
       (Box){.n = INT64_C(0)})));
                                ^~~~~~~~~~~~~~~~~
note: expected 'int64_t' but argument is of type 'Box'
   static int64_t apply_hyit(tur_poly_fn_t f, int64_t a) { ... }
```

The polymorphic HOF `apply-it` is emitted **once**, with the carrier
signature `int64_t apply_hyit(tur_poly_fn_t, int64_t)`. The two call
sites pass a `Box` value and an `int64_t`, neither of which matches the
emitted shape, and the C compiler rejects the call.

## Expected

`apply-it` should monomorphize per `A` at each call site -- one clone for
`A = Box`, one for `A = int` -- analogous to how unconstrained
polymorphic defns already specialize today (see e.g.
`tests/fixtures/cgi-constrained-generic-dispatch`, where
`geq__spec__bool_const_char___const_char__` is emitted at the `cstr`
call site of `geq`). Each clone should accept `(fn [A] int)` typed
concretely for that `A`, and the `count-it`-as-value argument should
resolve to the corresponding `count_it__spec__...` clone whose body
hardwires the right `Size` instance.

Concretely, the expected emit looks like:

```c
static int apply_it__spec__int_Box(int (*f)(Box), Box a) { return f(a); }
static int apply_it__spec__int_int64_t(int (*f)(int64_t), int64_t a) { return f(a); }
static int count_it__spec__int_Box(Box x) { return __inst_Size_size_Box(x); }
static int count_it__spec__int_int64_t(int64_t x) { return __inst_Size_size_int(x); }
```

with each call site dispatched to the matching pair.

## Working baseline

The non-polymorphic-HOF variant compiles and runs correctly today:

```turmeric
(defn apply-it [f : (fn [Box] int) b : Box] : int (f b))  ; concrete A = Box
(defn main [] : int
  (println (apply-it count-it (make-struct Box 0))))      ; prints 7
```

So the gap is specifically **per-`A` monomorphization of the
polymorphic HOF**, not constrained-poly value-passing in general.

## Root-cause direction (unverified)

The per-call-site monomorphizer (`emit_abi_intern_spec` in
`src/compiler/emit_module.c:655-742` -- see survey under
[`upcoming/m5-scope-audit-2026-06-18.md`](../upcoming/m5-scope-audit-2026-06-18.md))
already fires on:

- constrained-poly defns with concrete arg types
  (`count_it__spec__int_Box`-style emit verified in probes 1-3b);
- unconstrained-poly defns called with concrete arg types
  (existing infrastructure pre-dating M4).

It does **not** appear to fire on a polymorphic-HOF defn whose
concrete-A use is reached through a `tur_poly_fn_t`-typed `f`
parameter. The carrier-ABI `tur_poly_fn_t` value path likely short-
circuits the spec-intern check at the HOF's emit site, so the HOF stays
in carrier form even when every call site uses concrete `A`s.

## Proposed fix direction

Extend the spec-intern entry point at the HOF defn emit site so that
when all call sites resolve `A` to a concrete type, a per-`A` clone is
interned and called. This is exactly the "Rust generics" emit M5
already does for direct constrained-poly defns; the gap is just
recognizing it through one layer of polymorphic-HOF nesting.

If at least one call site genuinely needs the carrier (e.g. passes a
runtime-erased `tur_poly_fn_t`), keep the carrier clone alongside the
specialized ones, and route by call-site type.

## Validation

A new fixture under `tests/fixtures/poly-hof-constrained-arg-spec/`
mirroring the repro above; expected output:

```
7
-1
```

Plus a `grep` assertion in the emitted C for
`apply_it__spec__int_Box` and `apply_it__spec__int_int64_t` (the
specialized clones must actually be emitted, not just typed away).

## Resolution

Landed 2026-06-18. The root cause was *three* coupled gaps, all on the
path that decides whether a function-typed parameter monomorphizes or is
demoted to the `tur_poly_fn_t` carrier:

1. **`fn_type_has_named_tyvar` did not recurse into `TY_FN`**
   (`src/compiler/elab_fns.c`). A parameter `f : (fn [A] int)` carries its
   named tyvar `A` *inside* a `TY_FN` annotation, but the carrier-eligibility
   check only looked at `TY_TYVAR`/`TY_APP`/unions, so the nested `A` went
   undetected and `carrier_ok` wrongly fired -- demoting `f` to the int64
   carrier ABI (`tur_poly_fn_t`). The fix adds the `TY_FN` recursion
   (mirroring `call_type_has_named_tyvar`), so a polymorphic fn-typed
   parameter stays on the regular generic `TY_FN` path and monomorphizes
   per instantiation. The comment at `elab_fns.c:1346-1350` already
   documented this as the *intended* behavior; the predicate just failed to
   detect the case.

2. **Eta-expansion of the constrained-generic value argument did not fire
   through the polymorphic-HOF layer** (`src/compiler/elab_call.c`).
   `try_eta_expand_generic_fn_arg` only eta-expands when the parameter's fn
   type is already concrete; for a poly HOF the parameter is `(fn [A] int)`
   with `A` still abstract at the point the `count-it` argument is
   processed. A look-ahead now resolves the parameter's tyvars from sibling
   arguments that are exactly a bare tyvar (`a : A`) -- elaborating just
   those siblings early (cached via `arg_done[]`) -- then instantiates the
   parameter fn type so the eta look-ahead sees a concrete `(fn [Box] int)`
   and specializes `count-it` to its `Box` instance. (`call_instantiate_type`
   has no `TY_FN` case, so the fn-type instantiation is done locally.)

3. **The ABI-spec body emitted the indirect `(f a)` call with the generic
   carrier signature** (`src/compiler/emit_expr.c`). In the spec body the
   argument `a : A` still carries its generic type (the int64 carrier), so
   the fn-pointer cast read `(int64_t (*)(int64_t))` while the value was a
   by-value `Box` -- a `-Wint-conversion` hard error. Both indirect-call
   cast sites now resolve each argument through the active spec's
   `arg_types[]` (via the existing `emit_var_spec_arg_type`), so the cast
   signature and the carrier-bridge decision use the concrete monomorphized
   type (`(int64_t (*)(Box))`).

With all three, the repro lowers to the expected shape: `apply-it`
monomorphizes to `apply_it__spec__..._Box(int64_t f, Box a)` whose body is
`((int64_t (*)(Box))(intptr_t)f)(a)`, the `Box` call site passes an eta
wrapper around `count_it__spec__..._Box` (`Size[Box]` -> 7), and the `int`
call keeps the carrier base `apply_hyit` with `count_hyit` (`Size[int]` ->
-1). The carrier base is retained alongside the specialized clone, exactly
as this report's "Proposed fix direction" anticipated.

Validated by `tests/fixtures/poly-hof-constrained-arg-spec/` (output
`7` / `-1`; a wrong instance prints `-1` / `-1`). Full suite green
(`bash tests/run.sh`: 1678 passed, 0 failed).

### Known remaining gap (filed separately)

A *reversed argument order* poly HOF whose tyvar is pinned to a
**primitive** by a value argument appearing *before* the constrained-generic
fn argument -- `(defn apply-it [A] [a : A f : (fn [A] int)] ...)` called as
`(apply-it 42 count-it)` -- still fails to type-check (`expected
(fn [int] int), got (fn [tyvar] int)`). The struct case in that order now
works (eta fires for non-primitives); the primitive case does not (the
eta gate requires a struct/ADT/app pin). This was already fully broken
before this fix (it failed earlier, on the carrier coercion); it is a
pre-existing, orthogonal ordering limitation. See
`docs/reported/poly-hof-reversed-order-primitive-pin.md`.
