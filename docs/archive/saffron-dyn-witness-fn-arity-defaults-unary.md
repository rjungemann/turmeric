# A dispatched method's arity-less `: fn` parameter is cast as unary

**Severity: low.** Compiled only, and a clean panic at a checked cast, never a
wrong answer; `--interpret` answers. Split out of
[saffron-lang-plan](../archive/saffron-lang-plan.md) S9's remaining limits
(the "fn-arity default" item) when the plan was archived 2026-09-28.

When a Saffron program dispatches a typeclass method on an `any` receiver,
the compiled path calls a per-instance WITNESS that casts each erased-fn extra
argument to a Saffron lambda's type. The arity of that cast comes from the
CLASS declaration. A class that spells the parameter as a bare `g : fn` gives
no arity, so the witness assumes one argument, and a two-argument lambda
panics at the cast.

## Repro

```turmeric
#lang saffron
(defclass Comb [^t]
  (comb [ta : (t a) g : fn] : any))
(defdata Two [a] (Two [l : a r : a]))
(definstance Comb [Two]
  (comb [t g] (g (.l t) (.r t))))
(defn go [x] (.comb x (fn [a b] (+ a b))))
(defn main []
  (println (go (Two 1.5 2.25)))
  0)
```

Measured 2026-09-28 with `./build/tur` (v0.56.2):

- `tur --interpret`: prints `3.75` (expected).
- `tur run`: `panic at ...: cast: any holds a function this cast cannot accept
  -- a different signature, or a closure that captures where a plain function
  is required`, exit 134.

The same happens on a ground (kind-`*`) receiver: `(defclass Ap2 [a] (ap2 [x
g : fn] : any))` with an `Ap2 [Pt]` instance calling `(g (.x p) (.y p))`
panics the same way compiled and prints `3.75` interpreted.

Spelling the arity fixes it: with `g : (fn [a a] b)] : b` the same program
prints `3.75` on both back ends.

## Root cause

`saffron_mint_dyn_witness` (`src/compiler/elab_typeclasses.c`), the
`erased_fn` arm at line 6414: `fn_arity` starts at 1 and is raised only when
the class's `param_types[k + 1]` is a `TY_FN` with a known arity. A `: fn`
class parameter records `TYPE_PTR_VOID` with `param_is_fn` set
(`elab_typeclasses.c:871-879` for the spaced `g : fn`, 834-836 for `:fn`; the
Phase CCL comment at lines 753-755 calls it "a single-argument callable"), and
the impl records an inferred `g` as the
erased fn carrier too, so neither side carries the arity. The cast is then
`(cast __a1 (fn [any] any))`.

A FULLY unannotated class parameter never gets this far: a class method
records `[x g]` as `int` (the head type on a parametric head) in both
dialects, so the instance body's `(g ..)` is already "'g' is not a function or
continuation" at elaboration.

## Fix directions

1. Read the arity from the impl body: every `(g a b)` call site in the
   instance fixes it, and the witness is minted per instance anyway.
2. Or reject it at the instance body: a `: fn` parameter called with other
   than one argument gets a diagnostic that asks for the fn type to be spelled
   (`g : (fn [a a] b)`), turning the runtime panic into a compile-time error.
3. Add a fixture: the repro above, asserted on both back ends.

## Related, not this defect

With the arity spelled but the method's result declared `: any` -- `(comb [ta
: (t a) g : (fn [a a] b)] : any)` -- the compiled program panics `+: no
operator for a value of that type argument` while `--interpret` prints
`3.75`; declaring the result `: b` (or `: a` with `(fn [a a] a)`) works. Not
investigated here. Reduced 2026-09-28, it is not about fn parameters at all:
[erased-instance-body-tags-a-type-variable-widened-to-any](../archive/erased-instance-body-tags-a-type-variable-widened-to-any.md)
(the elements reach the lambda boxed, but tagged `TY_TYVAR`, because an
`: any` result mints no spec).

**Resolved 2026-09-29** (that report is archived): the `: any` shape with the
arity spelled now prints `3.75` compiled, pinned by
`tests/fixtures/saffron-instance-any-result-widens-element`.  THIS report's
bare `g : fn` repro still panics at the witness's cast exactly as described
above -- re-checked the same day.  The new `: any` spec deliberately stays off
a body whose widened tail calls an untyped `: fn` carrier (minting it turned
this panic into a cc error, `aggregate value used where an integer was
expected`), so fixing the arity here will also want that guard revisited
(`m7_body_returns_byvalue_element`, its `EX_UNION_INJECT` arm).

## Resolution (2026-09-30)

The report was worse than filed. The UNARY `g : fn` case did not work either.
It printed the element's TYPE TAG (`4` for `2.5`, `3` for an int) compiled,
because two representations disagreed at once:

- The dyn witness handed the erased instance body a `(Two any)` receiver
  whose elements are 16-byte tagged boxes, and the body read `->l` as an
  `int64_t` word, which is the box's tag.
- The body then called a Saffron lambda (`tur_tagged_t (void*, tur_tagged_t)`)
  through the poly-fn carrier's `int64_t (*)(void*, int64_t)`.

It went unfound because the Saffron fuzzer had never passed a lambda to a user
class method. Its only class was `Kind`, unary and extra-less, so this whole
family was outside its shape space.

**Fixed by semantics, not by arity.** In a dynamic dialect an arity-less `fn`
class parameter is now what every Saffron function value already is: an `any`
called dynamically (`tc_fn_param_type` in `src/compiler/elab_typeclasses.c`).
`EX_DYN_CALL` checks the arity against the closure that actually arrived, so
unary, binary and n-ary all work, and the int64 carrier is out of the picture.
Four follow-ons made that complete:

- the M7 gate (`m7_body_returns_byvalue_element`) admits an `EX_DYN_CALL`
  through a local fn value, and an `EX_ANY_CAST` over an element read, so the
  spec that knows the element type is minted instead of the erased body
  running;
- the static method-call path widens an argument whose instance parameter is
  `any` (`A <: any`, as `elab_call_fn` already did), so a lambda passed by
  direct dispatch compiles;
- a dynamic instance body under a `: a` result is narrowed with the checked
  unbox, as a `defn`'s concrete result already was. The emitter treats a cast
  whose target resolves to `any` as the identity, and the interpreter treats a
  type-variable-target cast as the identity, so a boxed `Sym` keeps its name;
- an `any` widened while its type is still an unresolved type variable now
  gets a reserved tag that makes `TUR_TAG` trap at runtime, instead of the
  bare `TY_TYVAR` kind no consumer understands. Any erased body that still
  runs fails loudly rather than silently.

The witness arity inference first drafted for this (reading `g`'s arity from
the impl body) was dropped. With the dynamic parameter it no longer serves
Saffron. For a typed class consumed from Saffron it would have let the cast
PASS into the same thin-call mismatch, trading a loud panic for a silent
wrong answer.

Pinned by `tests/fixtures/saffron-class-fn-extra`: dynamic and direct
dispatch, unary and binary, `: any` and `: a` results, float, int and cstr
elements. It is clean under clang `-fsanitize=function`. The Saffron fuzzer
gained the shape family (`hof_method`, tags `hofm_*`): 900 cases across
three seeds, all ok, where the first run on the pre-fix tree found 40
invalid-C programs.
