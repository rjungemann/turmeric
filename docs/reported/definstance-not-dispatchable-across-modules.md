# A `definstance` is not dispatchable from another module

**Severity: medium.** Another check/build divergence: `tur check` is clean and
`cc` fails with `call to undeclared function '__inst_<Class>_<method>_<Type>'`.
The name is right -- elaboration resolved the instance correctly -- but the
method is never declared or emitted into the importing translation unit.

**Status:** open. Found 2026-09-11 building `spices/crdt`'s OR-Set on
crdt-spice-plan C2, which is the first place in that spice where a typeclass
method had to cross a module boundary.

## Repro

Two modules in one spice. `crdt/causal` declares the instance:

```turmeric
(defmodule crdt/causal
  (export DotContext dotctx-new ...)
  (defstruct DotContext [seen : int])
  (definstance JoinSemilattice [DotContext]
    (join [x y] ...)))
```

`crdt/orset` imports the type and dispatches on it:

```turmeric
(defmodule crdt/orset
  (import crdt/causal :refer [DotContext ...])
  ...
  (join (.ctx x) (.ctx y)))     ;; tur check: clean.  cc:
```

```
error: call to undeclared function '__inst_JoinSemilattice_join_DotContext';
  ISO C99 and later do not support implicit function declarations
note: did you mean '__inst_JoinSemilattice_join_ORSet'?
```

The `did you mean` is the tell: the instance defined in the SAME module
(`..._ORSet`) is declared in this TU, and the imported one is not.

## What does not fix it

Adding the type to the `:refer` list. `DotContext` is imported explicitly in
the repro above and the error is unchanged -- an import brings the type and the
exported defns, not the instance.

There is no syntax for exporting an instance, which is consistent with how
instances work elsewhere (they are global to a compilation, not module-scoped),
so the gap looks like an emission one rather than a scoping decision: the
instance is in the elaborated environment for the importing module, since
dispatch resolved to the right mangled name; it just never reaches that TU's
declarations.

## Workaround

Export an ordinary forwarder from the defining module and call that:

```turmeric
(defn dotctx-join [x : DotContext y : DotContext] : DotContext
  (join x y))        ;; `join` resolves fine WITHIN this module
```

`spices/crdt` does exactly this, with a comment pointing here.

## Why it matters beyond the workaround

The forwarder costs the abstraction. A downstream module cannot write a
function generic over `[^JoinSemilattice A]` and have it work for a type whose
instance lives in another module -- it can only call the concrete forwarder. For
a spice whose whole point is a lattice vocabulary shared across modules, that
is the difference between a typeclass and a naming convention.

## Fixture owed

A two-module fixture in the multi-module/spice harness (not a single-file one:
the single-file path has no boundary to cross, which is presumably why this
survived). `tests/spice-c-sources-tests.sh` already builds small purpose-made
spices and is the natural home.
