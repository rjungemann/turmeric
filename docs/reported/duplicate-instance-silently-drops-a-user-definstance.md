---
title: "A user `definstance` for a type the autoloaded stdlib already covers is silently dropped: first definition wins, no diagnostic"
category: Reported
description: "`(definstance Eq [int] (eq? [a b] false))` in a user file is a no-op -- `(.eq? 3 3)` still answers `true` from stdlib's instance, on both back ends, statically and under Saffron's dynamic dispatch alike. The cause is the deliberate idempotent re-instance guard in elab_definstance (`build_inst_type_suffix` match => return nil), written for repeated `(load ...)`s of the same file. It cannot tell that case from a user overriding a stdlib primitive instance, and says nothing in either."
---

# A user instance colliding with an autoloaded stdlib instance is a silent no-op

**PARTIALLY RESOLVED 2026-09-09 -- it is no longer silent.** Fix direction 2
landed: the guard now warns `instance Eq [int] is already defined (first
definition wins): this definstance has no effect`, keyed on the defining FILE
(anything outside `stdlib/`), since `in_stdlib_load` is false for an explicit
`(load "stdlib/...")` -- the repeated-load case the guard was written for, which
stays silent. A census found no fixture re-instancing a stdlib class for a
primitive without its own local `defclass`, so nothing in the tree was warning
noise. Pinned by `tests/fixtures/duplicate-instance-warns` (the warning text and
that the first definition still wins).

**Still open: the language decision** -- replace (T1's stated intent for
ambiguous candidates, extended to exact duplicates) or reject. The warning
pre-empts neither. The report stays open for that.

**Severity: low-medium.** Nothing miscompiles and nothing crashes; the program
simply runs the OTHER instance. What earns the filing is the silence: a user
writes an instance, the compiler accepts it, and it has no effect -- with no
warning, and with a guide section on shadowing (`TUR-W0039`) that covers the
method-vs-defn case but not this one.

Found while answering saffron-lang-plan D8's open question 4 ("what happens
when two instances match one box tag?"). The answer is that they never reach
the registry: the second is discarded at `definstance`. So question 4 is not a
dynamic-dispatch question at all, and this is the general behaviour it exposed.

## Repro

```turmeric
(definstance Eq [int]
  (eq? [a b] : bool false))
(defn main [] : int (println (.eq? 3 3)) 0)
```

```
$ tur run p.tur          # true  -- stdlib's Eq[int], not the user's
$ tur interpret p.tur    # true
```

Same in a `#lang saffron` file with the receiver unannotated (dynamic
dispatch): the interpreter answers `true`; the compiled path panics, but on
S9's v0 two-parameter limit, not on this.

## Root cause -- read, and it is intentional for a different case

`src/compiler/elab_typeclasses.c`, `elab_definstance`, the "Idempotent
re-instance guard":

> An instance whose (typeclass, type-arg suffix) matches one already registered
> would re-emit the same dictionary struct/singleton and `__inst_*` method
> functions, producing a hard C "redefinition" ODR error. This fires whenever
> the same instance is seen twice -- e.g. a module that explicitly
> `(load "stdlib/typeclass.tur")`s while an auto-loaded partial typeclass stub
> already supplied the same primitive instance. **The first definition wins;
> the redundant one is a silent no-op**, matching the include-guard mental
> model for repeated loads.

The guard keys on `(typeclass, build_inst_type_suffix)` and returns `e_nil`.
It was written for the case where the SAME definition arrives twice through
two load paths (see `docs/archive/history/load-not-idempotent-typeclass.md`),
where silence is right. It cannot distinguish that from a DIFFERENT definition
for the same type arriving from the user, where silence is wrong -- and since
stdlib autoloads before user code, the user's definition is always the one
dropped.

This is consistent everywhere, which is why it looked like a design rather than
a defect: the static resolver's exact-match path takes the first instance
(`goto found_method`), the interpreter's instance walk takes the first, and
S9's registry dedupes first-wins on `(class, tag)`. All three agree because
there is only ever one instance to find.

Note the T1 rule one screen down in the same file -- "if exactly one of the
ambiguous candidates is a user-defined (non-stdlib) instance, it shadows the
stdlib instance(s)" -- applies only to the AMBIGUOUS-FALLBACK path, i.e. when
no exact match exists. An exact duplicate never gets there. So the codebase
already has a stated intent that user instances shadow stdlib ones, and this
guard defeats it for the most direct case.

## Fix directions

1. **Distinguish "same definition again" from "different definition, same
   type".** The guard has the previous instance in hand; compare method
   provenance (same source span, or `from_stdlib` on both) rather than only the
   type suffix. A genuine re-load matches on span and stays silent; a user
   redefinition does not, and can either
   - **replace** the stdlib instance (consistent with T1's intent -- user
     shadows stdlib), or
   - **error** (`instance Eq [int] is already defined by the stdlib; ...`),
     which is what most typeclass systems do.
   Either is defensible; silence is not.
2. **At minimum, warn.** A `TUR-W00xx: instance Eq [int] already defined
   (first definition wins); this definstance has no effect` costs nothing and
   turns a mystery into a message. Do this even if 1 is chosen, for the
   stdlib-vs-stdlib duplicate case that must stay a no-op.

Whichever is chosen, S9's registry and the interpreter's walk inherit it for
free -- there is still only one instance per `(class, type)` after
`definstance`, just a different one.

## Not this bug

Two instances for genuinely different types that share a box tag cannot occur:
the tag is a hash of the full type name, per instantiation. And a
type-variable-receiver instance (`definstance Clone [T]`) beside a ground one
(`Clone [int]`) is not a duplicate under this guard (different suffix); S9
excludes the former from the registry for having no ground tag, so it does not
compete there either.
