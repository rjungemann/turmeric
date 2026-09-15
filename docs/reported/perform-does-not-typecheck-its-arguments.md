# `perform` does not type-check its arguments at all

**Status: open, NARROWED 2026-09-15.** The memory-safety half is closed; the
general check is not, and is still worth doing on its own terms.

## What is fixed

The report's own "cheaper interim" is implemented, **one notch narrower than it
proposed**: `elab_perform` now rejects a LITERAL scalar (an integer, a bool, a
float) arriving at a POINTER-shaped parameter (`cstr`, `ptr<T>`,
`ref`/`rc`/`weak`, a borrow), with the ordinary `TUR-E0001` at the argument's
span, naming the declared parameter and both types.

The narrowing is measured, not cautious. The filing says a scalar argument
reaching a pointer-shaped parameter "admits no legitimate program"; it admits
one, and the fixture corpus found it in seven places. An **unannotated lambda
parameter defaults to `int`** at elaboration and is refined later, so

```turmeric
(defstruct App :copy [run : fn #fx{Log}])
(make-struct App (fn [msg] (perform (Log msg))))   ;; Log [msg : cstr]
```

reads as an int argument at the perform site and is entirely correct --
`tests/fixtures/effect-type-alias` and `cps-backend-fn-param-effectful` are two
of the seven, and they are now this check's controls. A literal's type is never
provisional, so restricting to one gives a rule with **no false positives at
all**, at the cost of missing a mismatch that arrives through a variable.
Widening it wants the same factored-out `arg_ok` predicate the general check
wants; it is not a better guess at this site.

The check runs AFTER both existing `any` seams in the same loop, so an `any`
argument that was just unboxed to the concrete parameter type is judged on what
it became, not on what it arrived as.

Deliberately NOT treated as pointer-shaped: `TY_STRUCT` / `TY_ADT` (by-value
aggregates, whose slot is not an address), `TY_FN` (already handled by the
boxing shim a few lines below), and `TY_TYVAR` / `TY_ANY` / `TY_UNKNOWN` (no
declared shape to disagree with). Not treated as scalar: `TY_NIL`, which an
ordinary call already accepts for a pointer parameter, and `TY_NEVER`.

Pinned by `tests/fixtures/errors/perform-arg-scalar-into-pointer-param`, with
the `cstr`/`int`/`float`/by-value-struct parameters and the Saffron `any`-seam
path all verified to still compile and run.

## What remains -- the general check

Everything the report describes under "Fix directions": factoring the
argument-compatibility decision out of `elab_call_fn_inner` (the ~300 lines
from `elab_call.c:6195` that thread implicit coercions, borrows, type
variables, HKT carriers, by-value aggregates and the seam through a mutable
`arg_ok` with coercion side effects) into a predicate both call sites can use.
Until that exists, these still pass unchecked at a `perform` site:

- **a scalar reaching a pointer-shaped parameter through a VARIABLE** rather
  than a literal -- the same defect, one indirection away, which is the price
  of the no-false-positive restriction above;
- a `cstr` argument reaching an `:int` parameter (the reverse direction -- a
  silent misread rather than a crash, so it is not in the narrow rule);
- a float argument reaching an `:int` parameter and vice versa;
- an aggregate whose def differs from the declared one;
- **arity**: `n_args` is never compared against `n_params`, so extra arguments
  are silently ignored and missing ones leave the handler reading a stale slot.

Arity is the cheapest of these and was left out of this pass only because
`defeffect` parameter defaulting exists and the interaction was not measured.

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
