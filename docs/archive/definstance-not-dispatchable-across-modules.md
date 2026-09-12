# A `definstance` is not dispatchable from another module

**Severity: medium.** Another check/build divergence: `tur check` is clean and
`cc` fails with `call to undeclared function '__inst_<Class>_<method>_<Type>'`.
The name is right -- elaboration resolved the instance correctly -- but the
method is never declared or emitted into the importing translation unit.

**RESOLVED 2026-09-11** -- see Execution at the end.

**Status when filed:** open. Found 2026-09-11 building `spices/crdt`'s OR-Set on
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


## Execution -- RESOLVED 2026-09-11

### Where it was

Two halves, and both had to move:

- **Linkage.** An instance method is never `is_exported`, and the three places
  that decide a defn's storage class all gate on that flag -- the definition
  (`emit_fns.c`), the forward declaration (`emit_module.c`), and the CPS entry
  (`emit_cps_ir.c`). All three emitted `static`.
- **Declaration.** The header loop in `emit_module.c` only declares top-level
  `EX_FN_DEF` items that are exported, so instance methods never reached the
  module header at all.

The CPS site is worth noting: its own comment already warned that a bare
`static` there "contradicts its non-static prototype -- static declaration
follows non-static declaration", and patching the other three without it
produced exactly that error.

### The gate: origin file, not `is_from_stdlib`

The obvious guard -- "external unless it came from the stdlib" -- is wrong, and
measurably so. It links, then fails with

```
duplicate symbol '___inst_Monoid_mempty_Any' in 4 TUs
```

because `stdlib/typeclass-lattice.tur` arrives by `(load ...)`, and a loaded
file's forms are spliced into **every** module that loads it -- so each TU
emits its own copy. `is_from_stdlib` is false for an explicit
`(load "stdlib/...")`, the same reason the duplicate-instance guard in
`elab_typeclasses.c` keys on the defining file's PATH rather than that flag.

`emit_inst_method_wants_external` (`emit_module.c`) is the single predicate all
four sites now consult: external only when the instance has an owner, is not
`is_from_stdlib`, and its `origin_file_id` does not resolve under `stdlib/`.

**Residual, recorded rather than hidden:** a NON-stdlib file `(load ...)`-ed
into two modules of one spice would still duplicate. The path test does not
catch that shape. It has no instance of it in the tree today, and the honest
fix is to compare the instance's origin file against the module's own source
rather than against a path prefix -- which needs the emitter to know its
module's file id, and it does not.

### Verified

- `tests/fixtures/spices/cross-module-instance` -- a purpose-made two-module
  spice, wired into `tests/spice-c-sources-tests.sh`. It asserts the build
  links **and** that the method is declared in the header and non-static in the
  .c: asserting on the build alone would pass if a future regression re-static'd
  the method and the importing TU happened to emit its own copy.
  A single-file fixture cannot test this -- there is no boundary to cross,
  which is how it survived.
- `spices/crdt` drops its `dotctx-join` forwarder and dispatches `join` on a
  `DotContext` across the module boundary directly.
- `tests/run.sh`: **2960 passed, 0 failed**. `spice-c-sources-tests.sh`: 10/10.
