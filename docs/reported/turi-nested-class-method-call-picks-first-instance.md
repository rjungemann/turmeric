# The interpreter picks the first instance for a nested class-method call

**Severity: high** (raised from medium 2026-09-11 -- see "A second symptom"
below). It is not only a hard error: over `defopaque` newtypes the same defect
is a **silent wrong answer**, which is strictly worse than the crash the
original repro produced. Confined to `--interpret`; the compiled path is
correct.

**Status:** open. Found 2026-09-11 while fixing the compiled half,
[nested-class-method-call-picks-the-first-instance](../archive/nested-class-method-call-picks-the-first-instance.md).
Pre-existing: that fix touched only `src/compiler/emit_core.c`, and this
reproduces identically before and after it.

## Repro

```turmeric
(defclass JS [a] (join [x : a y : a] : a))
(definstance JS [int]  (join [x y] (if (< x y) y x)))
(definstance JS [cstr] (join [x y] y))

(defn j2 [^JS A] [x : A y : A] : A (join (join x y) y))

(defn main [] : int (println (j2 "a" "b")) 0)
```

```
$ tur run p.tur          # b   -- correct
$ tur --interpret p.tur  # tur: eval: unknown infix builtin '<'
```

The `<` comes from the **int** instance, which the program never reaches on
this path: the nested call resolved to `JS [int]` and ran its body against two
`cstr` values.

## What separates the two halves

| Instance type | compiled | interpreted |
| --- | --- | --- |
| `float` | correct | **correct** |
| `bool` | correct | wrong (`unknown infix builtin '<'`) |
| `cstr` | correct | wrong (`unknown infix builtin '<'`) |

Float being correct is what kept this hidden: the compiled defect's own repro
used float, and its report recorded "`tur --interpret` answers correctly, so a
turi cross-check hides it". That is true for float and false for bool and cstr,
so the interpreter is not a reliable oracle for this class after all.

Same three conditions as the compiled half: a constrained generic body, a
nested same-class call, and an instance that is not the first declared. The
flat `(join x y)` is correct under `--interpret` for every type.

## Root cause -- not yet pinned

The compiled half was `emit_reresolve_disp_type` refusing to look through a
receiver that is itself a re-resolved class-method call: the receiver's
elaborated type is the representative instance's carrier, so the outer call
kept the representative. The interpreter has no emit-side re-resolution, so its
copy is a different mechanism reaching the same wrong answer -- presumably its
instance walk keying on the receiver's runtime tag where the nested receiver
carries the representative's.

`src/turi/eval.c`'s instance walk is the place to start; the compiled fix is
not portable to it.

## Fixtures owed

`tests/fixtures/typeclass-nested-method-call-float` covers the compiled side
including the bool/cstr rows, and carries a `requires.compiled` marker naming
this report. **Remove that marker when this lands** -- the rows are already
written and will start exercising the interpreter the moment it can run them.


## A second symptom, 2026-09-11: a silent wrong answer

Found running lattice-vocabulary-plan L2/L3's law functions under turi. Where
the instance type is a `defopaque` newtype rather than `cstr`/`bool`, there is
no `<` to fail on -- the wrong instance simply computes a different answer and
the program completes:

```turmeric
(load "stdlib/typeclass-lattice.tur")
(defopaque Diff :int)
(definstance Eq [Diff] (eq? [x y] (= (:: x int) (:: y int))))
(definstance Semigroup [Diff] (combine [x y] (:: (- (:: x int) (:: y int)) Diff)))

(defn main [] : int
  ;; (1-2)-3 = -4 vs 1-(2-3) = 2, so subtraction is NOT associative
  (println (law-associative? (:: 1 Diff) (:: 2 Diff) (:: 3 Diff)))
  0)
```

```
$ tur run p.tur          # false  -- correct
$ tur --interpret p.tur  # true   -- WRONG
```

A law suite that answers `true` for a deliberately non-associative instance is
worse than no law suite: it certifies exactly what it was written to catch.

### The trigger is nesting, measured

Both back ends agree on each of these, which is what narrows it:

| Shape | compiled | interpreted |
| --- | --- | --- |
| `(combine x y)` at a concrete type | ok | ok |
| `(combine (combine x y) z)` at a concrete type | ok | **ok** |
| `(combine x y)` inside a constrained generic | ok | ok |
| `(eq? (combine x y) (combine y x))` inside a constrained generic | ok | ok |
| `(combine (combine x y) z)` **inside a constrained generic** | ok | **WRONG** |

So it needs the same two conditions the compiled half did -- a constrained
generic body and a nested same-class call -- and it is not about `cstr`/`bool`
at all. The original repro merely happened to pick an instance whose body used
an operator the wrong receiver type could not accept, which turned the
misdispatch into a crash instead of a wrong number.

### The sharpest single case

`tests/fixtures/typeclass-nullary-method-newtype-tyvar` folds the same generic
at two newtypes over one carrier:

```
compiled:    10 21     (Sum: 0+3+7,  Product: 1*3*7)
interpreted: 21 21     (both answer with Product's instance)
```

Two call sites, two distinct instances, one answer -- so the interpreter is not
merely picking a wrong instance per call, it is collapsing both instantiations
onto the same one. That is the clearest statement of the defect: the compiled
path splits a specialization per instance type (and Gap H names same-carrier
newtypes apart with `__h<n>`); the interpreter has no such split.

### Affected fixtures

`tests/fixtures/typeclass-lattice-semigroup-monoid`,
`tests/fixtures/typeclass-lattice-join-meet` and
`tests/fixtures/typeclass-nullary-method-newtype-tyvar` all carry
`requires.compiled` naming this report. Their assertions are already written and include the
deliberately-failing instances; removing the marker when this lands turns them
into the interpreter's regression suite for free.
