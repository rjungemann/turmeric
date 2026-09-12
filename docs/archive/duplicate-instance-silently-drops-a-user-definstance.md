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
noise. That warning has since been promoted to the TUR-E0373 error below;
its fixture was replaced by `tests/fixtures/errors/duplicate-instance-rejected`.

**RESOLVED 2026-09-11 -- the decision is REJECT.** A colliding `definstance`
from outside `stdlib/` is now a hard error, `TUR-E0373`, naming the class, the
type, and the file that defined the existing instance. Pinned by
`tests/fixtures/errors/duplicate-instance-rejected`. Full execution notes at
the end of this file.

**2026-09-11 -- a second consequence, for a class that is NOT autoloaded.**
First-wins is a stable rule only while the stdlib instance is guaranteed to be
elaborated first, which autoloading provides. For a `load`-on-demand stdlib
module the winner is decided by **load order**, and the warning blames whoever
lost -- including stdlib itself. Verified at v0.46.1 with a stdlib-shaped
`Semigroup [int]` of sum against a user's of product: lib-first prints `7` and
warns on the user's line; user-first prints `12` and warns on the library's.

This matters more for some classes than others. `Eq [int]` has one right
answer, so first-wins is harmless. `Semigroup [int]` has four (sum, product,
min, max), so whichever the library ships is both arbitrary and
order-dependent. It is a live design constraint on
[lattice-vocabulary-plan.md](../upcoming/lattice-vocabulary-plan.md) section
3.5, which is why that plan now defers shipping bare-primitive instances until
this decision lands.

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


## Execution -- RESOLVED 2026-09-11

### The decision: reject, not replace

Both were defensible and the report deliberately left it open. Reject wins on
three grounds, the first decisive:

1. **Replace would be incoherent, not merely awkward.** T1's "a user instance
   shadows the stdlib one" reads like it wants replace -- but T1 governs the
   AMBIGUOUS-FALLBACK path, where nothing has been emitted yet. At an exact
   duplicate the first instance's dictionary struct, singleton and `__inst_*`
   methods are **already emitted**, and every call site elaborated before this
   point is already bound to them. Replacing would leave stdlib's own internal
   uses (`map-get`'s `Eq`, say) on the old instance while user code took the
   new one: one type, two behaviours, in one program.
2. **Reject is what the rest of the compiler already assumes.** The static
   resolver's exact-match path, the interpreter's instance walk and S9's
   registry all take the first match. They agree only because there is never
   more than one instance per `(class, type)` -- which rejecting preserves and
   replacing would not.
3. **It removes an order-dependence that first-wins carried.** For a stdlib
   module that is load-ON-DEMAND rather than autoloaded, "first" means LOAD
   ORDER. Measured before the fix, with a stdlib-shaped `Semigroup [int]` of
   sum against a user's of product: lib-first printed `7`, user-first printed
   `12`, and the warning blamed whichever lost -- including stdlib itself. A
   hard error cannot be order-dependent. This is what made the decision urgent
   rather than theoretical: it blocks
   [lattice-vocabulary-plan.md](../upcoming/lattice-vocabulary-plan.md) 3.5,
   where `int` genuinely has four monoids and no canonical one.

### What did NOT change

The repeated-load case the guard was written for is untouched. A stdlib file
stays silent via the existing path test (`in_stdlib_load` alone is not the
signal, since it is false for an explicit `(load "stdlib/...")`), and `load` is
already idempotent per path -- verified: a user file `(load ...)`-ed twice
prints its answer with no diagnostic, because the second `definstance` never
reaches the guard at all.

### Diagnostic

```
error: instance Eq [int] is already defined by stdlib/typeclass-eq.tur
(TUR-E0373): an instance is emitted once and is already bound at every call
site elaborated before this point, so a second definition cannot replace it --
remove this definstance, or give the instance a type of its own with a
`defopaque` newtype
```

The previous definer's path comes from `TypeClassInstance.origin_file_id` and
is **trimmed** to its `stdlib/...` tail (or basename) rather than printed
absolute -- an absolute path differs per machine and per CI runner and would
make the `errors/` fixture's `expected.diag` unusable.

### Verified

`tests/run.sh`: **2945 passed, 0 failed.** The census in the 2026-09-09 entry
predicted this -- nothing in the tree re-instances a stdlib class for a
primitive -- and promoting the warning to an error confirmed it.
