# A nested class-method call in a constrained generic picks the first instance

**Severity:** high -- **silent wrong answer** in compiled code. A `float`
specialization calls the `int` instance and truncates the double. No
diagnostic, no warning from `tur`, and the interpreter answers correctly, so a
`--interpret` cross-check hides it rather than exposing it.

**Status:** open. Found 2026-09-11 while probing the typeclass shapes for
[crdt-spice-plan.md](../upcoming/crdt-spice-plan.md). Reproduced against
`build/tur` at **v0.46.1** (Debug, macOS arm64) -- binary and stdlib stamps
matched.

Sibling of, not a duplicate of,
[typeclass-method-resolution-ignores-the-class.md](typeclass-method-resolution-ignores-the-class.md).
That report states plainly that "neither [symptom] is a wrong answer at
runtime"; this one is. Its Symptom B is an *unconstrained* generic that fails
in `cc`. Here the constraint **is** present and the body still resolves to the
wrong instance -- so the fix landed for Symptom A in `39c0dac53` does not cover
this, and the sentence "the constraint is what tells the specializer which
instance a call site needs" (that report, root-cause section) is not true for a
nested call.

## Repro

```turmeric
(defclass JS [a] (join [x : a y : a] : a))
(definstance JS [int]   (join [x y] (if (< x y) y x)))
(definstance JS [float] (join [x y] (if (< x y) y x)))

;; Constrained generic. The NESTED class-method call is the trigger.
(defn f [^JS A] [x : A y : A] : A (join (join x y) y))

(defn main [] : int
  (println (f 2.5 7.1))   ;; expected 7.1
  0)
```

```
$ tur run bug.tur
7
$ tur --interpret bug.tur
7.1
```

## Conditions -- all three are required

Removing any one of these makes the program correct, which is what makes the
bug easy to walk past:

1. **The body is a constrained generic** (`[^JS A]`). Spelling the same
   nesting at concrete `float` gives `7.1`.
2. **The class-method call is nested** -- the result of one `join` feeds
   another. The flat `(join x y)` gives `7.1`.
3. **The instance is not the first one declared.** `int` (declared first) is
   correct in every arrangement; `float` is wrong whether it is called first
   or second, so this is *not* an instantiation-order or specialization-cache
   effect.

A nullary class method (`(bottom [] : a)`) is **not** required. It was in the
program that first showed the symptom, which sent the initial reduction down a
blind alley.

## Mechanism -- visible in the emitted C

`tur emit-c` on the repro, float specialization:

```c
double  __ps_276 = (__inst_JS_join_float(x, y));      /* inner: correct */
int64_t __ps_277 = (__inst_JS_join_int(__ps_276, y)); /* outer: WRONG instance */
```

Both instances are emitted and correct in isolation
(`__inst_JS_join_int(int64_t, int64_t)`, `__inst_JS_join_float(double, double)`).
The inner call resolves to `float`; the outer call, in the same specialization
of the same body, resolves to `int` and is handed a `double`, which C converts
by truncation. The int specialization is right only by luck -- `int` is the
fallback it would have picked anyway:

```c
int64_t __ps_270 = (__inst_JS_join_int(x, y));
int64_t __ps_271 = (__inst_JS_join_int(__ps_270, y));
```

So the receiver type for a nested call is taken from something other than the
inner call's result type -- it falls back to the first matching instance. The
search loop above `src/compiler/elab_typeclasses.c:6675` is the mechanism the
sibling report already identifies; what is new here is that the *result type of
a class-method call is not propagated into the receiver position of an
enclosing class-method call* during specialization. The exact line that drops
it is not pinned.

## The cheap half of the fix is not in the compiler

`-Wfloat-conversion` on the emitted C catches this **statically and with zero
noise**:

```
$ cc -fsyntax-only -Wfloat-conversion bug.c
bug.c:9041:58: warning: implicit conversion turns floating-point number into
  integer: 'double' to 'int64_t' [-Wfloat-conversion]
bug.c:9041:48: warning: ... same line ...
2 warnings generated.
```

Two warnings, both on the miscompiled line, and **zero** on an equivalent
correct float program. (Adding `-Wconversion` brings ~7 more from the
hand-written preamble/runtime -- `-Wshorten-64-to-32`, `-Wsign-conversion` --
so `-Wfloat-conversion` alone is the clean signal; do not reach for the
broader flag without cleaning the preamble first.)

This is worth doing independently of the root-cause fix: it converts an entire
recurring defect class -- an int carrier reaching a float slot, which this
codebase has now hit through the `any` seam, the JIT engine, the `Sym` ctor,
and the specializer -- from a silent wrong answer into a build failure. macOS
CI already treats `-Wint-conversion` as a hard error (AppleClang 21), so the
precedent and the plumbing exist.

## Fix directions

1. **Propagate the result type.** A class-method call's elaborated result type
   must be the receiver type used to resolve an enclosing class-method call.
   This is the actual bug.
2. **Refuse rather than guess.** When a nested receiver's type is not
   determined, the resolver currently falls back to the first name-matching
   instance. That fallback is what turns a missing inference into a wrong
   answer; a diagnostic naming the class and the ambiguous receiver is strictly
   better, and matches the direction `39c0dac53` took for Symptom B.
3. **Add `-Wfloat-conversion` to the fixture compile** (see above). Cheap,
   independent, and catches the class rather than the instance.

## Fixtures owed

- `tests/fixtures/typeclass-nested-method-call-float/` -- the repro, asserting
  `7.1`. Must use a non-zero fractional part; an integer-valued float literal
  cannot show the truncation (CLAUDE.md's float-probe rule exists for exactly
  this shape).
- A second instance declared *before* the one under test, since "not the first
  instance" is condition 3.
- An `--interpret` counterpart is **not** useful as a guard here: turi answers
  correctly, so only the compiled fixture pins the defect.

## Blast radius

Any typeclass whose methods compose -- which is most of them once a method
returns its own class type rather than `bool` or `cstr`. stdlib's classes are
mostly safe today by accident: `Eq`/`Ord`'s methods return `bool`, `Show`
returns `String`, so there is nothing to nest. `Num` (`stdlib/typeclass.tur:113`)
is the existing class most exposed, and a `Semigroup`/`Monoid`/lattice family
-- where `combine : a -> a -> a` nests by construction -- would be **entirely**
exposed. That is why this report exists: it blocks
[crdt-spice-plan.md](../upcoming/crdt-spice-plan.md) and any lattice-vocabulary
work until fixed or worked around.
