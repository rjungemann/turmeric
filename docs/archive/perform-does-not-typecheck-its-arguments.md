# `perform` does not type-check its arguments at all

**RESOLVED 2026-09-16** -- the aggregate and pointer-shaped half is done the same way the primitive half was; see Resolution at the end.

**Status: open, NARROWED TWICE (2026-09-15).** Arity is fully checked on both
sides; primitive argument types are checked and now agree with an ordinary call
exactly -- the carrier exemption that was the last divergence has been dropped.
What remains is aggregates, `ptr<void>`-shaped parameters, and the `arg_ok`
factoring for the non-primitive cases.

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
such a parameter inherits the provenance. With a declared `int` now
distinguishable from a carrier `int`, the check reaches variables, call results,
field reads and operators.

That flag first served to SUPPRESS the check on a carrier argument. It no longer
does -- see the next section -- and now serves the **diagnostic** instead: a
mismatch on a carrier-defaulted parameter is not fixed by changing the argument,
since the value is already the right thing at run time, but by writing the
annotation that was missing. So the error carries a note that names it:

```
error [TUR-E0001]: effect 'Log' declares parameter 'msg' as 'cstr', but this
                   argument has type 'int' -- the handler would read the value
                   as an address
note: this parameter has no type annotation, so it took the default 'int'
      carrier; annotate it (e.g. '[msg : cstr]') to give it the declared type
```

Fixtures: `errors/perform-arity-mismatch`,
`errors/handler-clause-arity-mismatch`,
`errors/perform-arg-scalar-into-pointer-param`,
`errors/perform-arg-declared-var-into-pointer-param`,
`errors/perform-arg-primitive-kind-mismatch`,
`errors/perform-arg-unannotated-carrier-param`, and the positive
`perform-arg-primitive-aliases` (the two alias pairs, the only thing separating
this rule from plain kind equality).

## The carrier exemption -- dropped (2026-09-15)

For a short while `perform` exempted a carrier-defaulted argument from the
check, so that `(fn [msg] (perform (Log msg)))` against `Log [msg : cstr]` kept
compiling. That exemption is **gone**, because the ordinary call path never had
it and rejects the very same value:

```turmeric
(defn takes-cstr [s : cstr] : int 0)
(make-struct App (fn [msg] (takes-cstr msg)))
;; error [TUR-E0001]: function 'takes-cstr' arg 1: expected cstr, got int
```

The identical `msg` reaching a `perform` compiled. That made the effect ABI the
one place in the language where an un-annotated parameter silently escaped its
declared type, so keeping the exemption would have left this report's headline
-- "`perform` does not type-check its arguments" -- true for the most common
shape in the corpus.

**The migration cost was one annotation per site, and nothing else.** All seven
affected fixtures were fixed by writing the type the parameter always wanted
(`(fn [s : cstr] ...)`), and every one of them produces byte-identical output
afterwards -- which is the evidence that the annotation was the only thing
missing and that no behaviour depended on the laxness.

One of the seven is worth singling out. `errors/effect-fn-type-mismatch` asserts
a TUR-E0009 effect-row violation, and its un-annotated parameter made the new
TUR-E0001 fire first and mask it. The annotation is load-bearing there, not
decoration -- a reminder that a new check can hide an existing one rather than
only adding to it.

Pinned by `errors/perform-arg-unannotated-carrier-param`, which keeps the
carrier shape in the corpus as an ERROR (with its annotation note) rather than
letting it disappear from the fixtures altogether.

## What remains

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

## Resolution (2026-09-16) -- the third pass

The two remaining families were closed the way the second pass closed
primitives: by MEASURING what an ordinary call accepts, one shape per
program, and giving `perform` that table rather than an imitation of
`arg_ok`. The table (in `perform_arg_shape_mismatch`, `elab_effects.c`):

| parameter | argument | call's verdict |
| --- | --- | --- |
| `Point` | `Other` / `5` / `"hi"` / `cb` / `nil` | rejected |
| `Point` | `(Point 1 2)` | accepted |
| `(Option int)` | `5` / `(some "x")` | rejected |
| `(Option int)` | `(some 1)` | accepted |
| `H` (opaque) | `ptr<void>` / `5` | rejected |
| `ptr<void>` | `"hi"` / `Point` / `(some 1)` / `H` / `:Sym` / `bool` | rejected |
| `ptr<void>` | `cb` / `nil` / `ptr<int>` | accepted |
| `ptr<int>` | `"hi"` | rejected |
| `cstr` | `cb` / `nil` / `Point` | rejected |
| `float` / `bool` | `Point` | rejected |
| `int` | `Point` / `(Circle 1)` | **accepted** (the carrier word) |

`perform` accepted every rejected row silently before this -- `(perform (E
(Other 1)))` against `E [p : Point]` read `Other`'s first field as `.x` and
printed it -- and now rejects each with the same TUR-E0001 the primitive
pass introduced. The rule stays silent wherever the call path has an
inference arm this site cannot reproduce: an argument or parameter that
still mentions a type variable (a generic body's `x : A`, the W2 `(vec-new)`
into `(Vec int)` unification), `any` (unboxed above it), `!`, unknown -- and
for an `int` parameter, which is the carrier word and accepts an aggregate at
a call too.

**The `ptr<void>` question the filing left open is answered by the table**:
a call admits a fn value, `nil`, and any pointer at a `ptr<void>` parameter
and nothing else, so telling `TY_FN`/`TY_NIL` apart from a scalar needed no
factoring -- it is three kinds in an allowlist.

**The `arg_ok` factoring was not done**, and this pass is the second piece
of evidence it is not needed for `perform`: the 300 lines are inference and
coercion machinery (tyvars, HKT carriers, borrows, fn values, the `any`
seam), and everything a `perform` site must reject is a concrete shape
against a concrete declaration, which is a table. Worth doing on its own
terms if a third construct ever needs it; not as this report's leftover.

Pinned by six `errors/` fixtures (`perform-arg-aggregate-def-mismatch`,
`-scalar-into-aggregate-param`, `-aggregate-into-pointer-param`,
`-cstr-into-ptr-void-param`, `-option-element-mismatch`,
`-nil-into-cstr-param`) and the positive
`perform-arg-aggregate-and-pointer-shapes`. One accepted row is not pinned
positively: a by-value `(Option int)` through the effect slot passes the
check but the CPS emitter cannot yet lower `(some 5)` there -- that is
`colored-call-inside-match-evicts-the-cps-backend`'s second shape, still
open, not this check's.
