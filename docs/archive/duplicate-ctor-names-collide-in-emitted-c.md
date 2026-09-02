# Two ADTs sharing a constructor name collide in the emitted C

**Severity: medium, raised by SR2b.**  A hard cc error (`redefinition of
'ctor_Mk'`), so it is loud -- but the trigger set just grew from "two of your
own ADTs happen to share a ctor name" to "your ADT names a constructor
`Some`, `None`, `Ok`, or `Err`", because stdlib Option/Result are sums now and
their constructors are always in the program.

**Status:** RESOLVED 2026-09-02.  Filed 2026-08-27 during SR2b.  Pre-existing:
reproduced with two user ADTs and no stdlib involvement at the SR2b base.  See
the Resolution section at the bottom.

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

## Resolution (2026-09-02)

The base constructor's C FUNCTION symbol is `ctor_<Adt>_<Ctor>` now, built in
one place -- `mangle_ctor_symbol` (emit_core.c) -- and used by every definition
site, call site, and signature-table key.  Both repros compile and run; the
suite is 2748 passed / 0 failed with 148 snapshots regenerated in the same
change.

The union MEMBER name (`as.<Ctor>._N`) deliberately stays bare: it is already
scoped by the ADT's own struct.  Three sites used ONE variable for both meanings
(the monomorph ctor emitter in types.c most confusingly, spelling the symbol at
two lines and the member at a third), so the change is partly just splitting
them.  Using the shared builder for the symbol also puts types.c on the same
C-keyword guard as the call sites -- its hand-rolled fold had none, so a
constructor named after a C keyword had been spelling its definition and its
calls differently.

`docs/guides/name-mangling-guide.md` gained an
**"ADT constructors -- two names, and only one of them is scoped"** section.

### The bare spelling is an API surface, and it survives

The fix direction did not anticipate this: **hand-written inline C calls
constructors by their emitted name, and stdlib documents it** --
`stdlib/either.tur` says "Construct with `ctor_Left(v)` / `ctor_Right(v)`".
Five stdlib files and seven fixtures do it (`ctor_Left`, `ctor_Right`,
`ctor_Static`, `ctor_SVNil`, `ctor_SVCons`), and out-of-tree spices may too.  A
straight rename is a breaking change to that surface.

So a constructor name owned by **exactly one** ADT also gets a bare-name macro
alias next to its definition, and that inline C keeps working untouched, in this
tree and out of it.  When two ADTs own the name there is no correct bare alias,
so none is emitted: inline C naming it then fails at cc with an implicit
declaration pointing at the ambiguous constructor, instead of silently binding
to whichever ADT was emitted first.  Fail-closed is the point -- a missing alias
is a loud compile error, a wrong one is a silent wrong answer.

### Three things that had to be got right, each found by a failure

1. **A synthesized ctor call has no CtorDef.**  `elab_partial_apply` curries a
   constructor into a `__pap` lambda whose body calls it with `call_.ctor`
   unset, so naming the owner from the CtorDef alone emitted a bare
   `ctor_Person(...)` against the qualified definition.  `emit_ctor_owner_adt`
   falls back to the call's own result type, then to the callee binding's type.
2. **ADTs are registered before their constructors are attached.**  The first
   census read `def->ctors` at `elab_register_adt_def` and recorded nothing at
   all -- every query answered "not unique", so no alias was ever emitted and
   the six inline-C fixtures still failed to link.
3. **Holding the AdtDef pointers instead is a use-after-poison.**  A procedural
   macro runs a nested elaboration whose arena is released; ASan caught the
   census walking freed defs on eight `macro-*` fixtures.  Moving the snapshot
   to the end of elaboration then hit a *second* lifetime bug -- the
   non-session path frees `e.adt_defs` before the return, so the snapshot has to
   sit above the teardown, not at the return.  The census owns string copies now
   and holds no elaborator pointer.

Worth noting the shape: each of these three surfaced only as a suite failure,
and each looked like an unrelated area (currying, then nothing at all, then
procedural macros).  A missed site in a change like this is always an undefined
symbol or a lifetime error, never a wrong answer, which is what made the suite a
sufficient verifier.

### Pinned by

- `tests/fixtures/duplicate-ctor-names/` -- both halves of the report (two user
  ADTs sharing `Mk`; a user ADT naming its constructors `Some`/`None`/`Ok`/`Err`
  alongside the stdlib sums), asserting each constructor reaches its own ADT and
  that the stdlib sums still work beside the shadowing ones.
- `tests/run-option-niche-seam.sh` and `tests/run-sr4-seam.sh` -- both canaries
  hardcode a ctor spelling and both failed with their own "update this harness
  in the same change" message; updated to `ctor_Option_Some__String` and
  `ctor_Subst_SBind`.

### Deliberately not done

The residual ambiguity is not closed: every non-alphanumeric character folds to
`_`, so ADT `a-b` constructor `c` and ADT `a` constructor `b-c` both spell
`ctor_a_b_c`.  That is the pre-existing type-arg suffix convention's ambiguity
too (`ctor_Ok__Rat__Oops`), and closing it means changing the separator scheme
tree-wide.  It needs two ADTs whose names differ by exactly where one separator
falls; the bug fixed here needed only a shared constructor name, which is
ordinary.
