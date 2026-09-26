# A constrained generic's float result, passed into another generic, is value-converted

**Severity:** high -- **silent wrong answer** in compiled code. `-4.25` prints
`-nan`, a `float32` `7.1` prints `9.809089e-45`, with `tur check` clean and no
diagnostic.
`tur --interpret` answers correctly, so a turi cross-check hides it.

**RESOLVED 2026-09-26** -- see Execution at the end.

**Found by:** the nightly type fuzzer (`.github/workflows/fuzz.yml`,
[type-confusion-detection-plan](type-confusion-detection-plan.md) F2), four
times: seeds `20260913`, `20260917`, `20260918` and `20260925`, each as one
`BUG_wrong_output` on a float leg tagged `class_nested` plus `gid` or
`class_thru`. **None of the four was filed.** The workflow opened its issue
with `--label fuzz`, that label did not exist, and `gh issue create` failed
every time with `could not add label: 'fuzz' not found`. The only trace was a
red X on a scheduled run. The workflow now creates the label itself.

Sibling of
[method-result-float-spec-return-value-converts](method-result-float-spec-return-value-converts.md)
(a class *method's* float result crossing into a generic, fixed 2026-08-16).
This one is a *constrained generic function's* result, and the 08-16 fix does
not reach it.

## Minimal repro

```turmeric
(defclass N [a] (n [x : a y : a] : a))
(definstance N [int]   (n [x y] x))
(definstance N [float] (n [x y] x))

(defn flat [^N A] [x : A] : A (n x x))
(defn gid  [A]    [x : A] : A x)

(defn main [] : int
  (println (gid (flat -4.25)))   ;; expected -4.25, printed -nan
  0)
```

## Conditions

| Shape | Result before the fix |
| --- | --- |
| `(println (flat -4.25))` | `-4.25` |
| `(let [t (flat -4.25)] (println (gid t)))` | `-4.25` |
| `(gid (flat -4.25))` | **`-nan`** |
| `(gid (flat -4.25))` with only a `float` instance of `N` | `-4.25` |
| `(gid (bare -4.25))`, `bare` = `(defn bare [^N A] [x : A] : A x)` | `-4.25` |
| `(fid (flat -4.25))`, `fid` taking a concrete `: float` | `-4.25` |
| `(cid (flat -4.25))`, `cid` a *constrained* identity | **`-nan`** |
| `(gid (nest -4.25))`, `nest` = `(n (n x x) x)` | **`-nan`** |
| `(gid (flat (:: 7.1 float32)))` | **`9.809089e-45`** |
| `(gid (flat 7))`, `(gid (nest "s"))` | correct |

So all three are required:

1. **A constrained generic whose tail is a class-method call.** A body that
   returns its parameter unchanged is fine.
2. **The class has an instance declared before the float one.** With a single
   instance, the tail elaborates as `float` and the existing bridge fires.
3. **The result lands in another generic's argument** (constrained or not).
   Printing it directly, or `let`-binding it first, gives the call a concrete
   `float` result type and a `double`-returning spec.

Nesting is not required. The fuzzer's leg nested only because `class_nested`
is the one generator shape that declares a decoy instance first.

## Mechanism

The call site wants the int64 carrier (the argument slot of `gid` is a type
variable), so the spec is minted with a carrier return:

```c
static int64_t flat__spec__int64_t_double(double x) {
        double __ps_176 = (__inst_N_n_float(x, x));   /* re-targeted: right */
        if (tur_panicking) return ((int64_t)0);
        return __ps_176;                              /* VALUE conversion  */
}
...
int64_t __ps_172 = (flat__spec__int64_t_double(-4.25));
double  __ps_173 = (gid__spec__double_double(
                      ((union { int64_t s; double d; }){.s = (__ps_172)}).d));
```

The producer converts `-4.25` to the integer `-4`, and the consumer reads the
bits of `-4` back as a double, which is a NaN. `-Wfloat-conversion` flags the
bad `return` (it is exactly the class F0's ratchet exists for); no fixture
exercised the shape, so the ratchet had nothing to see.

`emit_fn_return_spelling` (`src/compiler/emit_fns.c`) already has the right
arm: a function whose **declared** result is a type variable, returning a
float through the carrier, bit-casts via `emit_carrier_bridge`. Two gates kept
it from firing:

1. **The tail's float-ness was read from its elaborated type.** Elaboration
   typed `(n x x)` against `N`'s representative, first-declared instance, so
   the tail read `int`. The emitter then re-targets the call
   (`emit_reresolve_method_call`) to `__inst_N_n_float` and spills it into a
   `double`, but the return arm never asked.
2. **The tyvar-declared test read only `result_kind`.** A *constrained*
   generic's `result_kind` is already lowered to `TY_INT`; its
   `result_full_type` still spells `A`.

## Execution -- RESOLVED 2026-09-26

- `emit_tail_float_kind` (new, `src/compiler/emit_fns.c`): the float kind a
  tail value has **in the emitted C**. That is its elaborated type, unless the
  tail is a dictionary-dispatched class-method call. For such a call, it is
  the declared result of the instance `emit_reresolve_method_fndef` selects
  for the active spec. That is the same instance, found the same way, as the
  one the call emitter re-targets to, so the producer's bridge and the temp's
  C type cannot disagree. It deliberately does **not** gate on
  `call_dispatch_is_static`, because the call emitter does not either. A
  nested receiver elaborates to a concrete `int`, so the outer call of
  `(n (n x x) x)` reads as static while still being re-targeted.
- The tyvar-declared arm now also accepts
  `result_full_type->kind == TY_TYVAR`, and takes its bridge source from
  `emit_tail_float_kind`.

The value-conversion convention for a function **declared** `: int` or
`: float` is untouched. Both gates still require a type-variable declared
result, which is the reading
[method-result-float-spec-return-value-converts](method-result-float-spec-return-value-converts.md)
established: the consumer bit-reinterprets exactly when the call's resolved
result type is a float.

Pinned by `tests/fixtures/constrained-generic-float-result-into-generic`
(flat, nested, `if`-arm, into a constrained generic, generic into generic, two
hops, float32, plus int and cstr controls), a FIXED row in
`tests/type-fuzz-src.py`'s `KNOWN_PROBES`, and seeds `20260913` and `20260918`
in `tests/fuzz-seed-corpus.txt`, both reachable at smoke size.

**Not fixed here, filed separately:** a class-method result `let`-bound
inside the same kind of constrained generic truncates even without the second
generic --
[let-bound-class-method-result-in-constrained-generic-truncates](let-bound-class-method-result-in-constrained-generic-truncates.md)
(since resolved at elaboration). Its generic-FUNCTION twin, a let-bound
`(gid x)` result with no typeclass involved, is also resolved:
[let-bound-generic-call-result-in-generic-truncates](let-bound-generic-call-result-in-generic-truncates.md).
