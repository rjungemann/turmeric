# `perform` does not type-check its arguments at all

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
