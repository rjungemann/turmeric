# Two ADTs sharing a constructor name collide in the emitted C

**Severity: medium, raised by SR2b.**  A hard cc error (`redefinition of
'ctor_Mk'`), so it is loud -- but the trigger set just grew from "two of your
own ADTs happen to share a ctor name" to "your ADT names a constructor
`Some`, `None`, `Ok`, or `Err`", because stdlib Option/Result are sums now and
their constructors are always in the program.

**Status:** OPEN.  Filed 2026-08-27 during SR2b.  Pre-existing: reproduced
with two user ADTs and no stdlib involvement at the SR2b base.

## Repro

```turmeric
(defdata A1 [t] (Mk t) (Nil1))
(defdata A2 [t] (Mk t) (Nil2))
(defn main [] : int 0)
```

```
error: redefinition of 'ctor_Mk'
```

## Root cause

The base (un-monomorphised) constructor's C symbol is `ctor_<CtorName>` --
the owning ADT's name is not part of the mangle.  Elaboration handles the
shadowing correctly (`elab_lookup_ctor` scans most-recently-registered first,
so the LANGUAGE semantics are fine); it is only the emitted C that merges the
two.  Monomorph clones collide the same way when the type-arg suffixes match
(`ctor_None` vs a user `(defdata Opt [a] (None) ...)`'s `ctor_None`).

## Fix directions

Mangle the owning ADT into the base ctor symbol (`ctor_<Adt>_<Ctor>`), the way
every other emitted family already namespaces.  The churn is wide but almost
entirely in regenerated snapshots; the risky part is the handful of sites that
BUILD the name by hand (`buf_printf("ctor_%s%s", ...)` in emit_expr.c, the
sig-table keys, `record_adt_app_ctor_sigs`) -- they must move in one change.

## Workaround

Rename one side's constructors.  `class-method-hkt-tyvar-grounding` was
renamed to `ONone`/`OSome` for exactly this in SR2b.
