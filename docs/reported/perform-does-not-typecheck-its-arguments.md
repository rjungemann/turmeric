# `perform` does not type-check its arguments at all

**Status: open, NARROWED TWICE (2026-09-15).** Arity is fully checked on both
sides; primitive argument types are checked and now agree with an ordinary
call. What remains is aggregates, `ptr<void>`-shaped parameters, and one
deliberate laxness that wants a human decision (see "The carrier question").

## What is fixed

### Arity -- both sides, and both had a segfaulting direction

Neither side of the effect ABI compared itself against the declaration.

| Shape | Before |
| --- | --- |
| `perform` supplies too FEW arguments | **segfault** -- the slot array is sized by the declaration, so the tail slots were uninitialised and the handler read them |
| `perform` supplies too MANY | silent: every seam in `elab_perform` is written `if (i < n_params)`, so the surplus was dropped |
| handler clause binds too MANY | **segfault** -- reads past the end of the slot array |
| handler clause binds too FEW | silent garbage word |

All four are now `TUR-E0002`. Strict equality is right: a `defeffect`
parameter list counts bare symbols, with no `& rest` form and no default
VALUES.

**This is also the correction to why arity was deferred the first time.** The
first pass left it out because "`defeffect` parameter defaulting exists and the
interaction was not measured". That defaulting (commit `4de8906a`) defaults a
parameter's *TYPE* when the annotation is omitted -- it never makes an argument
optional. The two were conflated; there was nothing to measure.

### Primitive argument types -- and the `arg_ok` factoring turned out not to be needed

The filing's central claim is that a perform-site check "has to agree with what
an ordinary call already accepts", and that agreeing means first factoring the
~300 lines of `arg_ok` out of `elab_call_fn_inner`. **For primitives that is not
so, and measuring it is what unblocked this.** Those 300 lines are about type
variables, HKT carriers, by-value aggregates, borrows and fn values. Between two
plain primitives a call is exact `TypeKind` equality, with two alias pairs and
no implicit widening at all:

```
(defn f [n : int] ...)      (f 7.25)    rejected: expected int, got float
(defn f [x : float] ...)    (f 7)       rejected: expected float, got int
(defn f [n : int] ...)      (f "hi")    rejected: expected int, got cstr
(defn f [n : int] ...)      int8 arg    rejected
(defn f [n : int] ...)      bool arg    rejected
int64  param <- int   arg               ACCEPTED   (aliases)
float64 param <- float arg              ACCEPTED   (aliases)
```

`perform` now applies that same rule to the same shapes -- verifiably rather
than by imitation -- via `perform_primitive_norm`, which returns a normalized
kind for a plain primitive and `TY_UNKNOWN` for everything else, so the check
fires only when BOTH sides are plain primitives and stays silent wherever the
call path has a coercion arm (`ptr<void>`, `nil`, `fn`, tyvar, `any`,
aggregates). The narrower scalar-into-pointer rule still covers `ptr<T>` /
`ref` / `rc` / `weak` parameters, which are not plain primitives.

### The carrier default -- the real reason the first pass could only check literals

The first pass could only check LITERAL arguments, because a TY_INT argument was
ambiguous. An **un-annotated lambda parameter defaults to `int`** and routinely
carries a `cstr` through it -- and that is deliberate, not a missing inference:
the bidirectional inference that would refine it from an expected fn type is
gated OFF for primitive expected types, to avoid codegen churn (see `elab_fn`'s
`expected_type` arm). So `int` there means "one untyped word", and checking it
reports working code, in seven fixtures.

Two things were ruled out before fixing it:

- **Running the check in a later pass does not help.** A probe in
  `effect_check.c` shows the argument is still `expr_ty=int binding_ty=int
  want=cstr` at that point. The carrier IS the representation, not a
  not-yet-refined guess.
- **There was nowhere to read the answer from.** Neither `Binding` nor `FnDef`
  recorded whether a parameter's type was written by the author.

So the fix records the missing information rather than guessing at it:
`Binding.type_is_carrier_default`, set at the two sites that take
`saffron_default_param_kind` and cleared by an annotation (and by the
bidirectional inference, when it fires). An un-annotated `let` bound directly to
such a parameter inherits the provenance, so `(let [m msg] (perform (Log m)))`
is declined one indirection along too. With a declared `int` now distinguishable
from a carrier `int`, the check reaches variables, call results, field reads and
operators -- everything except a carrier variable.

Fixtures: `errors/perform-arity-mismatch`,
`errors/handler-clause-arity-mismatch`,
`errors/perform-arg-scalar-into-pointer-param`,
`errors/perform-arg-declared-var-into-pointer-param`,
`errors/perform-arg-primitive-kind-mismatch`, and the positive
`perform-arg-primitive-aliases`. The carrier controls are the pre-existing
`effect-type-alias` and `cps-backend-fn-param-effectful`.

## The carrier question -- a decision, not a bug

Worth stating plainly, because it is the crux of what is left and it is
**deliberately** unresolved here: the ordinary call path does NOT exempt a
carrier parameter. It rejects one.

```turmeric
(defstruct App :copy [run : fn])
(make-struct App (fn [msg] (takes-cstr msg)))
;; error [TUR-E0001]: function 'takes-cstr' arg 1: expected cstr, got int
```

The identical `msg` reaching a `perform` is accepted, by the exemption above.
So `perform` is now *laxer than a call* in exactly one place, and the seven
corpus fixtures depend on that laxness -- written as function calls they would
not compile today.

Two defensible positions, and picking between them is a language decision rather
than a defect fix:

1. **Keep the exemption** (what is implemented). `perform` checks what it can;
   a carrier parameter stays unchecked on both the perform and handler sides.
2. **Drop it**, making `perform` agree with a call exactly. Then those seven
   fixtures become errors and want `(fn [msg : cstr] ...)` annotations -- which
   is arguably what they should have said all along, and would close the last
   type hole at this site.

(2) is the more principled end state and is a small change (delete the
`perform_arg_type_is_declared` guard); it is not taken here because it changes
what existing, working programs compile.

## What remains

- **The carrier question above** -- the largest remaining hole, and a decision.
- **Aggregates.** A by-value struct/ADT argument whose def differs from the
  declared parameter's is unchecked; `TY_STRUCT` / `TY_ADT` are excluded from
  the primitive rule because a call has real coercion behaviour for them.
- **`ptr<void>`-shaped parameters.** Excluded because a call accepts `TY_FN` and
  `TY_NIL` for them; telling those apart is where the original `arg_ok`
  factoring would genuinely earn its keep.
- **The `arg_ok` factoring itself**, for the non-primitive cases. Still worth
  doing on its own terms, as the filing says -- just not, as it turns out, a
  prerequisite for any of the above.

---

*Original report follows.*

**Severity: high -- memory safety, both dialects.** `elab_perform` elaborates
each argument and stores it in the effect slot without ever consulting the
effect's declared parameter types. A `cstr` parameter handed an `int` compiles
clean, with no diagnostic, and segfaults when the handler dereferences the
integer as a `char *`.

Found while fixing
[saffron-perform-argument-skips-the-any-seam](saffron-perform-argument-skips-the-any-seam.md),
which is the `any`-shaped corner of this same hole. That one is fixed; this is
the general case underneath it, and it is **not** Saffron-specific.

## Repro -- plain Turmeric, every signature annotated

```turmeric
(defeffect Log [msg : cstr] : int)
(defn work [] : int (perform (Log 42)) 1)
(defn main [] : int
  (println (handle (work) (Log [msg] k) (do (println msg) (resume k 0))))
  0)
```

```
Segmentation fault
```

`tur check` and `tur emit-c` both exit 0. The declared type is right there in
the `defeffect` two lines up.

## Root cause

`elab_perform` (`src/compiler/elab_effects.c`) walks the argument forms and
calls `elab_form` on each, then stores the results straight into `PerformExpr`.
`effect->constructor->param_types` / `param_full_types` are read by the
HANDLER, so the declaration is not being ignored for lack of information -- the
perform site simply never looks at it.

## Why this was not a one-line fix alongside the `any` seam

The `any` seam is a narrow rule (`any` argument, concrete parameter -> checked
unbox) with a single answer. A general check has to agree with what an ordinary
call already accepts, and that logic is not small or shared: `elab_call_fn_inner`
computes `arg_ok` across roughly 300 lines from `elab_call.c:6195`, threading
implicit coercions, borrows, type variables, HKT carriers, by-value aggregates
and the seam itself. A naive `args[i]->type.kind == param_types[i]` at the
perform site would reject a large amount of code that is correct today.

## Fix directions

Factor the argument-compatibility decision out of `elab_call_fn_inner` into a
predicate both call sites can use -- it is worth doing on its own terms, since
`perform` is not the only construct that takes a declared parameter list
without going through the call path. Then `perform` reports the ordinary
TUR-E0001 on a mismatch, and its `any` seam becomes the same special case it is
at a call rather than a parallel one.

A cheaper interim that covers the memory-safety case specifically: reject a
scalar argument (int/bool) reaching a POINTER-shaped parameter (`cstr`, `ptr`,
a handle) at the perform site. That is the shape the segfault takes, and it
admits no legitimate program.
