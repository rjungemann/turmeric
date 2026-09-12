# A default method's spliced instance copy is typed at the carrier, not the instance

**Severity: high** -- a **silent wrong answer**. The instance dispatches
correctly and the emitted function has the wrong signature, so the arguments
are converted on the way in. `tur check` is clean, cc is happy (the conversion
is legal C), and only a non-`int` instance is affected.

**Status:** open. Found 2026-09-11 executing
[lattice-vocabulary-plan.md](../upcoming/lattice-vocabulary-plan.md) L1, whose
design (`max`/`min` as defaulted `Ord` methods) it blocks outright.

Caught by the `-Wfloat-conversion` ratchet
(`type-confusion-detection-plan.md` F0), **not** by the fixture's stdout diff:
the concrete `(max 2.5 7.1)` is correct and only the call through a generic is
wrong, so a fixture that checked the obvious case would have passed.

## Repro

```turmeric
(defclass C [a]
  (tag  [x : a] : bool)
  (pick [x : a y : a] : a (if (tag x) x y)))   ;; DEFAULT returning the class var

(definstance C [int]   (tag [x] true))
(definstance C [float] (tag [x] true))

(defn g [^C A] [x : A y : A] : A (pick x y))

(defn main [] : int
  (println (pick 2.5 7.1))    ;; 2.5 -- correct
  (println (g 2.5 7.1))       ;; 2   -- WRONG
  0)
```

Replace the default with an explicit `(pick [x y] x)` in each instance and both
lines answer `2.5`.

## Mechanism -- one line of emitted C

The two spellings emit different instance signatures for the *same* method:

```c
/* explicit instance method */
static double  __inst_C_pick_float(double, double);
/* spliced default */
static int64_t __inst_C_pick_float(int64_t, int64_t);
```

Dispatch is correct in both -- the call site reaches `__inst_C_pick_float`.
But the spliced copy declares the int64 carrier, so the caller's `double`
arguments are converted on the way in and the result on the way out:

```c
static double g__spec__double_double_double(double x, double y) {
        int64_t __ps_276 = (__inst_C_pick_float(x, y));   /* 2.5 -> 2 */
        return __ps_276;
}
```

## Root cause

`elab_defclass` deliberately does **not** elaborate a sibling-calling default at
the class -- there is no instance for `.tag` to resolve against yet -- and
records the method *form* instead (`methods[i].default_method_form`).
`elab_definstance` then splices that form in for an omitted method
(`elab_typeclasses.c`, the `if (!found && tc->methods[i].default_method_form)`
arm) so it "elaborates as an ordinary instance method against a concrete
receiver".

It does not, quite. An explicit instance method is written with **bare**
parameters (`(pick [x y] ...)`) and inherits its types from the class signature
under the instance substitution. The spliced default carries the class's own
**annotations** (`[x : a y : a] : a`), and those are elaborated literally: `a`
is a type variable, which lowers to the int64 carrier. The instance's type
argument never reaches them.

## The annotations are not optional

Dropping them does not work either. With the class variable appearing only in
the return type, every call site becomes:

```
error: cannot infer type for return-directed method 'pick'; add a type
ascription, e.g. (:: (pick ...) T)
```

So a default method whose result is the class variable is currently
**unwritable**: annotated, it is miscompiled; unannotated, it is unusable. That
is what makes this high rather than medium -- there is no spelling that works.

A default returning a fixed type (`: bool`, `: cstr`) is unaffected, which is
why the feature has looked fine: every default in the tree today returns
something concrete.

## Fix direction

Substitute the class's type parameters for the instance's type arguments when
the spliced form's annotations are elaborated -- the same
`elab_subst_class_tyvars` treatment the instance-method parameter path already
applies to a `TY_TYVAR` / `TY_APP` parameter type. The splice currently hands
the form over unchanged; the substitution has to reach the annotations, not
only the inherited signature.

## Workaround, and what L1 did instead

An ordinary **constrained generic defn** has none of this problem:

```turmeric
(defn max [^Ord A] [x : A y : A] : A (if (gte? x y) x y))
```

It is correct at every instance, works inside another generic, emits no
conversion, and costs a user-written instance nothing. `stdlib/typeclass.tur`
defines `max`/`min` that way, with a comment pointing here. Worth knowing more
generally: **if a "default method" only needs the class's other methods and not
per-instance code, a constrained generic defn is the better tool anyway** --
it is not overridable, which is usually what was wanted.

## Fixtures owed

- The repro above, asserting `2.5` twice.
- A sibling with `int` instances only, so a fix that works by accident at the
  carrier type is not mistaken for a real one.
- `tests/fixtures/ord-max-min` already covers the workaround shape.
