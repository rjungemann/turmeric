# Polymorphic HOF taking a constrained-poly-as-value bakes carrier ABI

**Status:** OPEN 2026-06-18.
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
