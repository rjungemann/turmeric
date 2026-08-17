# Rational and Complex numbers

> **Status:** N0/N1/N2 landed (2026-07-29); N3 UNBLOCKED (J1 landed
> 2026-07-29, `tur jit <file>` exists behind `-DTUR_JIT=ON` +
> `--enable=jit`) but not started; N4 deferred by design
> **Type:** Language / stdlib / reader
> **Hard constraint:** must run unchanged under `tur`, `turi`, and the
> upcoming `tur jit` -- see [`jit-engine-plan.md`](jit-engine-plan.md)

## Status of each phase

| Phase | State | Notes |
|---|---|---|
| **N0** -- `Num` de-`:int` + builtin-miss -> `Num` dispatch | **landed** | `stdlib/typeclass.tur`, `src/compiler/elab_call.c` |
| **N1** -- `Rational` + `#rat{...}` | **landed** | `stdlib/rational.tur`, `src/compiler/reader.c` |
| **N2** -- `Complex` + `#cx{...}` | **landed** | `stdlib/complex.tur`, `src/compiler/reader.c` |
| **N3** -- JIT parity | **blocked** | `tur jit` (J1) does not exist yet |
| **N4** -- the `i` suffix | **deferred** | by design, per §5.2 |

Deviations from the plan as written, and why:

- **§3.1 said the existing `Num` instances cover `float`.** They did not --
  there was no `Num [float]` instance at all. One was added alongside the
  class-signature change.
- **§7 said `stdlib/math.tur` stays as-is.** It could not: `complex/exp` and
  `complex/arg` need `exp`, `sin`, `cos`, and `atan2`, and math.tur had only
  `sqrt`/`fabs`/`floor`/`ceil`/`pow`. The four scalar libm wrappers were added
  there (which is what that file is for) rather than to `complex.tur`, keeping
  the plan's actual intent -- no Complex-specific code in math.tur. Each got a
  matching interpreter native, as did `fabs`/`ceil`/`pow`, which had been
  missing them; without a `fabs` native an interpreted `complex/div` dies on
  "inline-C not supported in interpreter mode".
- **§4.3's checked variants** are written as explicit pre-multiply comparisons
  rather than `__builtin_*_overflow` equivalents, so the check itself never
  relies on observing a wrapped result -- which would be C UB and would also
  trip the Debug build's UBSan.

Fixtures: `tests/fixtures/{rational-basics,rational-arith,rational-overflow,
complex-basics,complex-smith-div,num-typeclass-operator-dispatch}` plus
`tests/fixtures/errors/{complex-no-ord,rat-literal-zero-denominator,
cx-literal-arity}`. All pass under both `tests/run.sh` and `tests/run-turi.sh`
against the same `expected.stdout`.

User-facing docs: [`docs/guides/numeric-tower-guide.md`](../guides/numeric-tower-guide.md)
and the numeric-literal section of
[`docs/guides/data-literals-guide.md`](../guides/data-literals-guide.md).

## 0. Summary

Add two numeric types:

- `Rational` -- an exact `num/den` pair over `int` (int64), always normalized
  (`den > 0`, `gcd(num, den) == 1`).
- `Complex` -- a `re`/`im` pair over `float` (double).

Two decisions do the heavy lifting, and both are driven by having three
execution engines rather than one:

1. **Never emit C `_Complex`, and never rely on the compiler's complex
   runtime.** `src/` contains no `_Complex` or `<complex.h>` today; keep it
   that way. `Complex` is a plain two-`double` struct with hand-written
   arithmetic.
2. **Write the arithmetic in Turmeric, not inline C.** The tree-walking
   interpreter cannot execute inline C and carries ~319 hand-written native
   overrides to compensate (`src/turi/interpreter_natives.c`; see
   `jit-engine-plan.md` §1.3). A numeric tower written in ordinary Turmeric
   needs *zero* new natives and is byte-identical across all three engines by
   construction.

Everything else -- the operator surface, literals, printing -- follows from
those.

---

## 1. Why `_Complex` is disqualified, precisely

The JIT plan selects MIR (`c2mir` + `MIR-gen`) and reuses the existing C
emitter verbatim (`jit-engine-plan.md` §0). `c2mir` implements a C11 subset;
`_Complex` is an optional C11 feature and is not part of it. That alone rules
it out.

But there is a second, sharper problem that would bite even on a C compiler
that *does* support `_Complex`: **complex multiply and divide in C do not
compile to arithmetic, they compile to calls into the compiler runtime.**
`a * b` on two `double _Complex` values lowers to `__muldc3`, and `a / b` to
`__divdc3` -- libgcc/compiler-rt helpers that implement the NaN-recovery and
overflow-avoidance rules the standard requires. Under the JIT those become
external symbols MIR must resolve through `MIR_load_external`, i.e. entries in
the runtime symbol boundary that JIT plan item **S2** is meant to make finite
and explicit. Adding two compiler-runtime helpers to that boundary buys
nothing that ~15 lines of hand-written Turmeric does not.

So: hand-written arithmetic, using Smith's algorithm for division (scale by
the larger-magnitude denominator component before dividing) so the overflow
behaviour is ours, is the same on every engine, and is testable.

This also means **JIT pre-work item S1** ("C11-subset audit of emitted C")
gains a standing rule rather than a one-time sweep: `_Complex`, `<complex.h>`,
and the `__mul*c3`/`__div*c3` family never appear in generated C. Worth a grep
in the audit script.

---

## 2. Representation

Both are single-variant record products, which the existing machinery lays out
**by value** (`adt_is_byvalue_product`, used at
`src/compiler/elab_effects.c:23` and `elab_call.c:6883`):

```turmeric
(defstruct Rational
  [num : int    ;; sign lives here
   den : int])  ;; invariant: den > 0, gcd(|num|, den) = 1

(defstruct Complex
  [re : float
   im : float])
```

Emitted C is a 16-byte `struct` of two `int64_t` / two `double`. No pointer, no
allocation, no drop glue (neither carries an owning field), so neither type
interacts with the rc/ref/borrow machinery at all -- which is most of the
reason this is a small change rather than a large one.

**Open item for JIT phase J0:** confirm `c2mir` handles 16-byte struct
arguments and returns by value on both x86-64 SysV and AArch64 AAPCS. If it
does not, the fallback is to pass by `&` pointer for these two types
specifically -- an emitter detail invisible to the language, but one worth
knowing before the stdlib is written against it. Add one 16-byte-struct
round-trip to the J0 spike's exit criteria.

**Arbitrary precision is out of scope.** There is no bignum in the tree, and
adding one is a much larger project than this. `Rational` is int64/int64 with
an explicit overflow story (§4).

---

## 3. Operators: `Num` dispatch, not new builtin rows

`+`, `-`, `*`, `/` are `BuiltinSpec` rows keyed by `TypeKind`
(`src/compiler/builtins.c:11-25`), each emitting a **C infix operator**
(`BS_VARIADIC_FOLD` with `c_op = "+"`). That shape cannot express
`Rational + Rational`: there is no C `+` for a struct, and the spec table has
no way to name a specific ADT anyway.

The right move is the general one, not a special case:

> **N0 -- when no builtin row matches the argument types, fall back to `Num`
> typeclass dispatch.**

`Num` already exists (`stdlib/typeclass.tur:108`) and the elaborator already
does instance resolution and dictionary passing. Making `+` desugar to
`Num.add` on a miss gives operator overloading to *every* user numeric type,
with `Rational` and `Complex` as the first two consumers. Primitive arithmetic
is untouched -- the builtin row still wins whenever it matches, so there is no
codegen drift on existing programs and no dictionary in the hot path.

### 3.1 `Num` has to be fixed first

```turmeric
(defclass Num [a]
  (add [x y] : int)   ;; <- every method returns :int
  ...)
```

The docstring says the quiet part out loud: *"Results are typed as `:int`
(int64_t) in v1; specific numeric types are handled by each instance."* That
is exactly the type-eraser CLAUDE.md's **No Lazy `:int` Stand-Ins** rule
forbids, and `Rational` cannot be an instance of it -- `add` would have to
claim it returns an integer. It becomes:

```turmeric
(defclass Num [a]
  (add [x : a y : a] : a)
  (sub [x : a y : a] : a)
  (mul [x : a y : a] : a)
  (div [x : a y : a] : a)
  (neg [x : a]       : a))
```

Every existing `definstance Num` in `stdlib/typeclass.tur` (int, int8/16/32,
uint*, float, float32) is already *shaped* correctly -- `(add [x y] (+ x y))`
returns the instance's own type -- so the instances need no body changes, only
the class signature moves. Expect fixture churn from the class-method
signature appearing in emitted dictionaries; regenerate in the same commit,
per the fixture-churn rule.

`Ord`/`Eq` get `Rational` and `Complex` instances too -- with the standard
caveat that **`Complex` has no `Ord` instance**, because complex numbers are
not ordered. Do not invent a lexicographic one to fill the table; a missing
instance is the correct answer and the type checker should say so.

---

## 4. Rational semantics

### 4.1 Normalization

`(rat/of n d)` divides both by `gcd(|n|, |d|)` and moves the sign to the
numerator. `(rat/of 2 4)` and `(rat/of 1 2)` are the same value, so structural
equality *is* mathematical equality and `Eq`/`Hash` are trivial.

### 4.2 Zero denominators are a `result`, not a panic

```turmeric
(defn rat/of [n : int d : int] : result<Rational, ArithError> ...)
(defn rat/of! [n : int d : int] : Rational ...)   ;; panics on d = 0
```

`ArithError` is a small ADT (`DivByZero`, `Overflow`), not an int status code.
`#rat{n/0}` (§5) is a **read-time error**, so the literal form never needs the
result wrapper.

### 4.3 Overflow

int64 numerators and denominators overflow, and `a/b + c/d` is where it
happens first. Two mitigations, both standard:

- **Cross-cancel before multiplying.** `add` reduces by
  `g = gcd(b, d)` and computes `(a*(d/g) + c*(b/g)) / (b*(d/g))`, then
  normalizes. This keeps the common case in range far longer than the naive
  `a*d + c*b` over `b*d`.
- **Checked variants.** `rat/try-add`, `rat/try-mul`, ... returning
  `result<Rational, ArithError>`, using `__builtin_*_overflow`-equivalent
  checks written as explicit comparisons in Turmeric (so turi and the JIT
  agree).

The unchecked `Num` instance methods -- the ones behind `+` -- wrap on
overflow, matching what `int` already does, and this is **documented at the
type**, not left implicit. A program that cares uses the `try-` forms. If a
later refinement-types pass can discharge the no-overflow obligation
statically, this is a natural consumer; not a dependency.

---

## 5. Literals

### 5.1 Rational: `#rat{3/4}`

Bare `3/4` is not available. `read_number` (`src/compiler/reader.c:278`) stops
at `/`, and the remainder lexes as a symbol -- so `3/4` reads today as `3`
followed by `/4`. Making bare `3/4` a rational would also collide with `/` as
division and with module-qualified names (`tur/list`), which is a lot of
ambiguity to buy one slash.

So: a `#`-dispatch, alongside `#map{...}`, `#set{...}`, `#json(...)`, and
`#rx`. The body is read **raw** (the `RM_BODY_STRING` shape the `stringed`
layer's `#s"..."` uses, `src/compiler/lang_layers.c:38-55`), which matters
because curly-infix is on in every dialect -- an ordinarily-read `{3 / 4}`
would be infix division, not a literal.

```turmeric
#rat{3/4}      ; => (rat/of! 3 4)
#rat{-3/4}     ; => (rat/of! -3 4)
#rat{6/8}      ; => normalized at read time to 3/4
#rat{1/0}      ; => read-time error
```

Always on, not a `#lang` layer -- CLAUDE.md is explicit that layers should not
accumulate and that a core data-literal dispatch belongs with `#map`/`#set`.

### 5.2 Complex: `#cx{3.0 4.0}`

```turmeric
#cx{3.0 4.0}   ; => (complex/of 3.0 4.0)
#cx{3.0 -4.0}
```

Two space-separated float literals, read normally (so `#cx{3.0 {1.0 + 1.0}}`
works). `#cx` rather than `#c` -- a single-letter dispatch is too scarce a
name to spend, and `#c` reads as "C" in a codebase full of inline-C blocks.

**Deferred: an `i` imaginary suffix** (`4.0i`, so `{3.0 + 4.0i}` reads
naturally). It is attractive and the `LiteralSuffix` machinery
(`src/compiler/forms.h:36-48`) has an obvious slot for it, but it only pays
off with mixed `float + Complex` arithmetic, which means an implicit widening
coercion in operator resolution. That is a real type-system decision and it
should not ride along on a literal-syntax change. Revisit after §3's `Num`
fallback has settled.

Per CLAUDE.md's float-testing rule: every literal test uses a non-zero
fractional part (`#cx{3.25 -1.5}`), never `#cx{3.0 4.0}` alone -- a whole-number
float cannot show a truncation bug.

---

## 6. Printing and `Show`

- `Rational` -> `3/4`, `-3/4`, and `5` when `den == 1`. Round-trips through
  `#rat{...}`.
- `Complex` -> `3.25+4.5i`, `3.25-4.5i`, sign always explicit on the imaginary
  part so the output is unambiguous.

`Show` instances live with the types. `stdlib/typeclass-show.tur` is the home
for the instance; the formatting itself is Turmeric string building, not a
native.

---

## 7. Files

```
stdlib/rational.tur   -- Rational, rat/of, rat/of!, arithmetic, try- variants,
                         Num/Eq/Ord/Show/Hash instances, rat->float
stdlib/complex.tur    -- Complex, complex/of, arithmetic (Smith division),
                         abs/arg/conj/exp, Num/Eq/Show instances
stdlib/typeclass.tur  -- N0: Num class signature de-:int-ified
src/compiler/builtins.c
src/compiler/elab_call.c -- N0: builtin-miss -> Num dispatch
src/compiler/reader.c    -- #rat{...}, #cx{...}
```

`stdlib/math.tur` stays as-is -- it is libm wrappers over `float`, and
`Complex` transcendentals (`complex/exp`, `complex/abs`) are built *on* those
in `complex.tur` rather than added to it.

---

## 8. Phases

- **N0 -- `Num` de-`:int`.** Class signature becomes `a`-returning; existing
  instances unchanged; builtin-miss falls back to `Num` dispatch. Lands alone,
  useful alone: it is what makes any user numeric type work with `+`.
- **N1 -- `Rational`.** `stdlib/rational.tur`, `#rat{...}`, instances, tests.
- **N2 -- `Complex`.** `stdlib/complex.tur`, `#cx{...}`, Smith division,
  instances, tests.
- **N3 -- JIT parity.** Once `tur jit` exists (J1), run the rational/complex
  fixtures under it. The S1 audit gains its standing `_Complex`/`__divdc3`
  grep. If J0 found a 16-byte struct ABI problem (§2), this is where the
  by-pointer fallback lands.
- **N4 (deferred) -- the `i` suffix** and mixed float/Complex arithmetic, per
  §5.2, only after N0 has been in use long enough to know what implicit
  coercion would cost.

---

## 9. Testing

- Fixtures under `tests/fixtures/rational-*` and `tests/fixtures/complex-*`,
  ASCII-only, each with `expected.stdout`.
- **Every fixture runs on all three engines.** Compiled via `tests/run.sh`,
  interpreted via `tests/run-turi.sh`, and JIT via the J3 harness once it
  exists. Because the arithmetic is pure Turmeric, the *same* `expected.stdout`
  applies to all three -- if it does not, that divergence is itself the bug
  worth finding.
- Numeric cases worth pinning: `#rat{6/8}` normalizes; `1/3 + 1/6 = 1/2`
  exactly (the case floats get wrong); `rat/try-mul` at int64 boundaries
  returns `Err Overflow`; complex division where naive `(ac+bd)/(c²+d²)`
  overflows but Smith's does not; `Ord` on `Complex` is a type error.
- Twelve-minute timeout on every suite run.
