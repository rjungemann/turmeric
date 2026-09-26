# A `let`-bound class-method result in a constrained generic truncates a float

**Severity:** high -- **silent wrong answer** in compiled code. `-4.25` prints
`-4`. No diagnostic; GCC's `-Wfloat-conversion` flags the emitted line, but
no fixture reaches it, so the ratchet never sees it.

**RESOLVED 2026-09-26** -- see Execution at the end. Fixing it turned up a
second path to the same symptom that needs no typeclass at all. That one was
filed separately, and has since been resolved too, as
[let-bound-generic-call-result-in-generic-truncates](let-bound-generic-call-result-in-generic-truncates.md).

**Status when filed:** open. Found 2026-09-26 while reducing
[constrained-generic-float-result-into-generic-value-converts](constrained-generic-float-result-into-generic-value-converts.md),
which is fixed. Reproduced on `build/tur` at the commit that fixed that report
(Debug, Linux x86-64, GCC 13.3). The result is the same with and without that
fix.

## Repro

```turmeric
(defclass N [a] (n [x : a y : a] : a))
(definstance N [int]   (n [x y] x))   ;; int declared FIRST
(definstance N [float] (n [x y] x))

(defn one [^N A] [x : A] : A (let [y (n x x)] y))
(defn two [^N A] [x : A] : A (let [y (n x x)] (n y x)))

(defn main [] : int
  (println (one -4.25))   ;; expected -4.25, prints -4
  (println (two -4.25))   ;; expected -4.25, prints -4
  0)
```

No second generic is needed, unlike the archived sibling. The same body
without the `let`, `(n (n x x) x)`, is correct. `tur --interpret` prints
`-4.25` twice, so a turi cross-check hides this one too.

## Mechanism -- visible in the emitted C

`tur emit-c` on the repro, `two`'s float specialization:

```c
static double two__spec__double_double(double x) {
        {
            double __ps_181 = (__inst_N_n_float(x, x));   /* re-targeted: right */
            if (tur_panicking) return ((double)0);
            int64_t y_1618 = __ps_181;                    /* VALUE conversion  */
            (void)y_1618;
            int64_t __ps_182 = (__inst_N_n_int(y_1618, x)); /* wrong instance  */
            if (tur_panicking) return ((double)0);
            return __ps_182;
        }
}
```

The inner call is elaborated against `N`'s representative, first-declared
instance, so its type is `int`. The emitter re-targets the *call* to the float
instance (`emit_reresolve_method_call`) and spills it into a `double`. But the
`let` binding takes its C type from the elaborated `int`, so the double is
converted by value. Everything downstream of `y` is typed `int` too. The outer
call's receiver is then a concrete-`int` variable, so
`emit_reresolve_disp_type` has nothing to recover and dispatches to
`__inst_N0_n0_int`.

This is the let-bound case of
[nested-class-method-call-picks-the-first-instance](nested-class-method-call-picks-the-first-instance.md).
That fix recovers the dispatch type when the receiver **is** a re-resolved
class-method call. Here the receiver is a variable **bound to** one.

## Why the fuzzer never found it

`tests/type-fuzz-src.py` never puts a `let` inside a generated generic's
body. `class_nested` writes `(meth (meth x x) x)` directly, which the fix
above covers. Adding a let-bound variant of `class_nested` would reach this.
It belongs in the known-bug table until this is fixed, per the harness's
discipline.

## Fix directions

1. **At elaboration (the real fix).** Inside a constrained generic, a class
   method whose declared result is the class variable should give the call
   the type `A` (the constrained type variable), not the representative
   instance's type. Then the `let` binding, every later use, and the return
   are typed `A` and resolve per spec like any other `A`-typed value. This is
   the premise the emit-side recoveries keep working around.
2. **At emission (a patch).** Give the binding the re-resolved instance's
   declared result as its C type, and teach `emit_reresolve_disp_type` to
   follow a variable receiver to a `let` init that is a re-resolved call.
   Every other reader of `y` would still see `int`, so expect more shapes.
3. **Pin it:** a compiled fixture asserting `-4.25` for both `one` and `two`,
   with `int` declared before `float`, plus the fuzzer shape above.


## Execution -- RESOLVED 2026-09-26

Fix direction 1: at elaboration.

`elab_method_call` (`src/compiler/elab_typeclasses.c`) binds a call on an
abstract-tyvar receiver to a carrier **representative** instance (`N [int]`),
tags it with a dictionary, and leaves the real instance to emit-side
re-resolution. The call's *type* was still read from the representative's
binding, so it was `int`. Now, when the class declares both the receiver and
the result as its own variable (every `a -> ... -> a` method), the call is
typed with the receiver's `A`. The return-directed path already did this
("`out->type` stays the tyvar `bound`"); the receiver-directed path never
had. Gated on a `TY_TYVAR` receiver and `definstance_depth == 0`, so a
concrete receiver and an instance body are untouched.

The base clone is unchanged, because `A` lowers to the same int64 carrier the
`int` did. Each spec now resolves the binding's `A` to its own type, so `y` is
a `double` in the float spec. The next dispatch on it re-resolves like any
other `A`-typed receiver:

```c
static double two__spec__double_double(double x) {
        {
            double __ps_181 = (__inst_N_n_float(x, x));
            if (tur_panicking) return ((double)0);
            double y_1618 = __ps_181;
            (void)y_1618;
            return __inst_N_n_float(y_1618, x);
        }
}
```

### One behaviour change

`(println (n x x))` inside a `[^N A]` generic used to compile, and at `0.5`
printed `0`. It is now the same compile error `(println x)` on an `A` value
has always been (TUR-E0006, no `println` for a type variable). A wrong answer
became a diagnostic. No fixture in the full suite (3191 passed, 0 failed)
relied on the old behaviour.

### The second path -- split out, fixed separately

`(let [y (one x)] (n y x))`, with `one` a generic *function*, still truncated
after the fix. Reduced, it needs no typeclass:
`(defn wrap [A] [x : A] : A (let [y (gid x)] y))` returns `9` for `9.75`.
`elab_call.c` types a generic call instantiated to the caller's own `A` as
`int` too, by a separate path. The obvious fix, keeping the tyvar there, was
tried and failed 16 fixtures. The gate cannot tell a caller's tyvar from an
unbound one, and several emitter paths depend on that `int`. It is filed with
the full record as
[let-bound-generic-call-result-in-generic-truncates](let-bound-generic-call-result-in-generic-truncates.md).

### Pinned

- `tests/fixtures/let-bound-class-method-result-keeps-its-tyvar`: `let`,
  `let` chain, `if`, dot syntax, a second class dispatched on the let-bound
  value, the result into another generic, float32, plus int/cstr controls.
  Without the fix it fails on the F0 `-Wfloat-conversion` ratchet.
- `tests/type-fuzz-src.py`: a new `class_let` crossing in the default pool,
  and a FIXED row in `KNOWN_PROBES`. The generic-function half has a
  `gid_let` crossing. It was `--emit-known` only while its report was open,
  and has been in the default pool since that report was resolved.
