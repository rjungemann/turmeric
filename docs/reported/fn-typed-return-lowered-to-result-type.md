---
title: Function-Typed Return Annotation Mis-Lowered to its Result Type in Codegen
category: Reported Bug
description: A `defn` whose declared return type is a function type `(fn [...] T)` has its C signature emitted with the *inner* return type `T` instead of a pointer-typed closure box. The body returns the box (a `void *`), producing C compile errors and (if those were silenced) a register-class miscompile -- e.g. `(fn [:float] :float)` lowers to `double`, while the body returns `void *`. Surfaced by language-readiness-for-typed-signal G2 (signal-constant-poly).
---

# Function-Typed Return Annotation Mis-Lowered to its Result Type -- Reported Bug

> **Found:** 2026-06-03, during the G2 spike of
>   [language-readiness-for-typed-signal-plan](../upcoming/language-readiness-for-typed-signal-plan.md).
> **Severity:** Medium -- caught by `cc` today (incompatible-result-type
>   warnings escalated to errors at link), so it is not silent. But the
>   moment the codegen path widens (or a warning is downgraded), the same
>   register-class mismatch as
>   `bare-fat-param-non-int-result-miscompiles` would silently miscompile
>   instead. The annotated-^fat fix relied on threading the *correct* fn
>   return type through; this bug is the producer-side mirror.

---

## Summary

A `defn` that explicitly declares its return type as a function type
emits a C signature whose return type is the function's *result* type,
not a pointer-to-closure-box. The body produces a closure box
(`void *`), so the emitted C is type-mismatched and refuses to compile.

## Repro

Minimal:

```turmeric
(defn make-id [] : (fn [:float] :float)
  (fn [x :float] :float x))

(defn main [] :int 0)
```

Emitted (excerpt from `tur emit-c`):

```c
static double make_id();              /* expected: void *make_id(); */

static double make_id() {
        return __fn_855;              /* __fn_855 is void * */
}
```

`tur build` errors out:

```
error: returning 'void *' from a function with incompatible result type 'double'
        return __t26;
               ^~~~~
```

The same pattern appears for `(fn [:float] :int)` -- lowered to
`int64_t (*)(double)` at the *consumer* let-binding site
(`int64_t (*s_seven)(double) = constant_int(...)`) while the producer
returns a plain integer-typed value -- another mismatch.

## Repro fixture (workaround)

`tests/fixtures/signal-constant-poly/` uses `:ptr<void>` as the return
type instead and recovers the inner type at the consumer via an
annotated `^fat g :(fn [:float] #{} :T)` parameter. That is the
supported path today; the producer-side annotation `: (fn [...] T)` is
not.

## Root cause (suspected)

The codegen path that lowers a `defn`'s declared return type appears
to recurse into the function type and pick the *result* slot, instead
of treating "this defn returns a function" as "this defn returns a
closure box pointer". Likely site: the C-emit pass for function
signatures, where a TY_FN return is treated as if the defn directly
returned a value of the fn-type's result -- consistent with the way
inline-C function values themselves are lowered as raw C function
pointers (their declared return type is the result type), but wrong
for a turmeric-level `(fn ...)` closure literal whose runtime
representation is a heap-allocated box.

Look in `src/compiler/elab_call.c` / the codegen pass for the
TY_FN-return branch and verify it folds to a pointer box, not the
result type.

## Implications

- Closure-returning constructors can be declared today only with
  `:ptr<void>` as their return type. This loses the *signature* of the
  produced closure at the producer boundary -- the consumer must annotate
  with `^fat g :(fn [...] T)` to recover it.
- For typed pipeline constructors (the entire signal-library shape:
  `constant`, `sine`, filter constructors, arrow combinators) the
  `:ptr<void>` shape is sufficient *only because* the consumer-side
  annotated form threads the correct result type through. If the
  producer's declared return type were silently miscompiled rather
  than diagnosed by `cc`, the same xmm0/rax register-class mismatch
  documented in `bare-fat-param-non-int-result-miscompiles` would
  reappear at the producer side.

## Proposed fix

When emitting the C signature for a `defn` whose declared return type
is `TY_FN`, lower the return as a closure box pointer (`void *` today,
or a typed closure pointer once
[closure-first-class-type-plan.md](../upcoming/closure-first-class-type-plan.md)
lands). Threading the inner function type through to call-site type
recovery is already handled by the annotated `^fat` path; this is
strictly the signature-emission side.

## Validation when fixed

1. The minimal repro above compiles cleanly and `make_id()` returns
   a closure box that, when applied to `1.5`, yields `1.5` (or whatever
   the closure computes).
2. The G2 fixture `tests/fixtures/signal-constant-poly/` can be
   upgraded back to declaring `: (fn [:float] :float)` /
   `: (fn [:float] :int)` returns (dropping the `:ptr<void>` workaround)
   and still pass.
3. Full `bash tests/run.sh` stays green.

## Cross-references

- Producer-side mirror of
  [bare-fat-param-non-int-result-miscompiles](bare-fat-param-non-int-result-miscompiles.md)
  (consumer side, resolved).
- Surfaced by:
  [language-readiness-for-typed-signal-plan](../upcoming/language-readiness-for-typed-signal-plan.md)
  G2 (signal-constant-poly).
- Related end-state:
  [closure-first-class-type-plan](../upcoming/closure-first-class-type-plan.md).
