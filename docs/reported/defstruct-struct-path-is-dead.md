---
title: elab_defstruct's StructDef branch is dead, and its comment says the opposite
category: Reported
description: defstruct_lowers_to_adt has been widened until it rejects nothing, so the `else` in elab_defstruct is unreachable. CORRECTED 2026-09-05: it does not keep any legacy StructDef elaboration alive -- DS-D deleted that two days before this was filed, and what remains is a three-line defensive diagnostic. The value here was never the branch; it is the StructDef machinery behind it, which is still reachable.
---

# `elab_defstruct`'s StructDef branch is dead

**Severity: low.** No user-visible defect -- this is dead code plus a comment
that actively misdescribes it. Filed rather than fixed because the deletion
belongs to the structdef-retirement track, not to a drive-by.

## The finding

`defstruct_lowers_to_adt` (`elab_structs.c`) has been widened slice by slice --
`:linear` (slice 4), `:no-auto-ctor` (slice 2), `:heap` including parametric
(seam 3), parametric type-param vectors -- until it rejects nothing. So the
`else` arm of

```c
if (defstruct_lowers_to_adt(e, call)) { ... }   /* elab_structs.c */
```

is unreachable, and the legacy StructDef elaboration it guards is unreachable
with it.

`elab_toplevel.c` already reached this conclusion and acted on it:

```c
/* structdef-retirement DS-C: defstruct_lowers_to_adt is always true
 * now (every field shape lowers or is rejected at the ADT field
 * parser), so the former `else if (is_defstruct)` StructDef-stub
 * branch was unreachable and is removed. */
```

The two files have disagreed since. The pre-pass registers an ADT stub
unconditionally, while `elab_defstruct` still carries a branch for a case the
pre-pass no longer prepares for -- so if the dead branch were ever reached it
would be reached with an ADT stub already registered.

## Measurement (2026-09-04)

Instrumented **both** outcomes of the predicate and ran the whole suite:

- `bash tests/run.sh` -- 2781 fixtures, **zero** struct-path hits.
- By hand, every shape the surrounding comment claims still takes the struct
  path, plus the ones its header names:

| shape | outcome |
|---|---|
| `(defstruct Pt [a : int])` | ADT |
| `(defstruct Lin :linear [h : int])` | ADT |
| `(defstruct Hp :heap [n : int])` | ADT |
| `(defstruct Par [A] [val : A])` | ADT |
| `(defstruct Boxy [v : (Option int)])` (applied type) | ADT |
| `(defstruct Ex [v : (exists [A] A)])` | ADT |
| `(defstruct Fnf [f : (fn [int] int)])` | ADT |
| `(defstruct Nest [p : (Pair int int)])` | ADT |

The stale prose is in two places and was wrong in every clause:

- The call-site comment said "anything the gate rejects (`:heap` / `:linear`
  outer structs) still elaborates as a struct." Both lower.
- The function header said the field checks "still legitimately keep a
  `:linear` outer struct, or one carrying an applied-type / `exists` field, on
  the StructDef path." All three lower.

Both are corrected in place as of this report; the branch itself is untouched.

## Correction, 2026-09-05

**The premise above is stale, and was already stale when it was filed.** The
`else` arm does not guard "the legacy StructDef elaboration". That elaboration
was deleted on 2026-09-02 (`structdef-retirement DS-D`, commit `f9c4cd12`), two
days before this report; what sits after the `if` today is three lines:

```c
    diag_emit(DIAG_ERROR, call->span,
              "defstruct '%s': unsupported field form", name->name);
    return NULL;
```

So fix direction 1 as written -- "delete the `else` arm and sweep whatever
StructDef machinery becomes unreachable" -- has nothing to sweep. Deleting a
defensive diagnostic is not a cleanup, and it is the whole of what direction 1
would achieve.

Two more things checked while confirming this:

- The predicate's `false` returns for MALFORMED input are all shadowed by
  earlier diagnostics in `elab_defstruct` itself. `(defstruct Foo)` and
  `(defstruct Foo :copy)` both report "defstruct requires a field list";
  `(defstruct Foo 7)` reports "field list must be a vector". None reaches the
  fallthrough, which is consistent with the zero-hit measurement above and
  explains WHY it is zero rather than just recording that it is.
- The machinery direction 2 names is still there and is not small: ~47
  `StructDef` references across ten files, plus twelve `struct_defs` registry
  uses, several already commented "always empty".

**So this stays parked, for the reason the report already gave** -- the point is
the machinery, not the branch -- but a future reader should not go looking for
an elaboration path to delete. Direction 2 is the entire remaining content.

## Fix directions

1. ~~Delete the `else` arm in `elab_defstruct`~~ -- see the correction above:
   there is no elaboration behind it, only a diagnostic. Dropping the predicate
   and making its callers unconditional is still possible, but on its own it
   trades a defensive error for nothing.
2. Then sweep whatever `StructDef` machinery becomes unreachable. That is the
   part worth doing deliberately: `struct_defs` registries are already described
   as "always-empty" in at least one place (`elab_structs.c`, the DS-C note on
   the redefinition scan), so the reachable surface may be larger than the
   branch.

Worth pairing with whatever slice of structdef-retirement comes next rather
than landing alone, since the point of the deletion is the machinery behind it,
not the seven lines of branch.
