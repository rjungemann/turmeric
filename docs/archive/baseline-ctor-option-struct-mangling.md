---
title: Baseline regression — `ctor_Option__struct` emitted without definition
severity: Blocks slice-5 B work and any other change measured against `bash tests/run.sh`.
status: RESOLVED 2026-06-29. Fixed in the mangler (`src/compiler/types.c`); full
  by-value suite green (1874/0). See "Resolution" below.
---

# Baseline regression — `ctor_Option__struct` emitted without definition

## Resolution (2026-06-29)

Fixed in `append_type_mangle` (`src/compiler/types.c`).  The actual defect was a
**name collision**, not a missing definition: three distinct inner type kinds all
mangled to the literal `"opaque"` --

- `TY_FN` (the `default:` arm) -- a `void *` by-value fat-closure handle;
- `TY_TYVAR` (also the `default:` arm) -- an int64 carrier placeholder;
- def-less `TY_STRUCT` (the previous interim patch routed it to `"opaque"` too).

`(Option <fn>)` and `(Option <placeholder>)` therefore both emitted
`tur_adt_Option__opaque`, with **different ABIs** (`void *` by-value vs int64
boxed).  They share one `TUR_FN_…`/`TUR_TY_…` include guard, so the first
definition won and every call site of the other got the wrong signature
(`ctor_Option__opaque(bool, void *)` called with an `int64_t`, or the reverse).
The previous interim patch (def-less `TY_STRUCT` -> `"opaque"`) made the collision
worse by merging the placeholder onto `TY_FN`'s token.

The committed snapshots already encoded the correct, non-colliding convention:
`Option__opaque` is **always** `void *` (TY_FN) and `Option__struct` is **always**
`int64_t` (placeholder) -- verified across all 92 snapshot fixtures.  The fix
restores that split:

- def-less `TY_STRUCT` mangles back to `"struct"` (reverting the interim patch);
- `TY_TYVAR` and `TY_UNKNOWN` are split out of the `default:` arm and routed to
  the **same** `"struct"` token, so the def-emitter and the call site agree on
  the placeholder's name regardless of which placeholder representation the
  elaborator happened to leave behind (the producer inconsistency that surfaced
  the bug);
- `TY_FN` (and other resolved compounds) stay on `"opaque"`.

`emit-c` output now compiles cleanly and the full by-value suite is green
(1874/0).  Snapshots regenerated in the same change.  This restores an honest
`bash tests/run.sh` gate so slice-5 B2-B4 can resume in safely-measurable
increments.

Note: a *separate* gap remains in the multi-file split path
(`tur build <dir>` / `emit_header`), where four monomorph typedefs
(`Endo__int`, `Schema__int`, `Option__struct`, `Option__Zipper__struct`) are
referenced but not emitted into `input.h`.  This does NOT affect the suite gate
(`tests/run.sh` builds fixtures single-file via `emit_program`, the same path as
`emit-c`) and is unrelated to the mangler -- it also drops fully-concrete
monomorphs (`Endo__int`).  Tracked separately; not part of this fix.

## Summary

`bash tests/run.sh` at main `e042eb822` ("structdef-retirement slice 5 (step
1): migrate `defopaque` off `StructDef`") reports **1838 passed, 44 failed**.
The slice-5 plan claims this commit landed at **1874/0**; the gap was missed
because the suite was not gated on a clean run before merge.

All 44 failures trace to **one** codegen bug.

## Repro

```sh
./build/tur build tests/fixtures/defstruct-fn-field-single-arg
# /tmp/.../input.c:5791: error: call to undeclared function 'ctor_Option__struct'
```

Every "build failed" fixture in the failing set hits the same first
diagnostic.  The five "stdout mismatch" fixtures (`hamt-delete`,
`image-roundtrip`, `image-reload-hook`, `image-hooks-tracked`,
`load-in-imported-module`, `vec-push-byvalue-aggregate-escapes-frame`) are
the same bug under their `hook.sh` harness, which does not fail-fast on
build errors so the stale `actual.stdout` from a prior successful run is
compared against `expected.stdout`.

## Root cause

`append_type_mangle` in `src/compiler/types.c:622-624`:

```c
case TY_STRUCT:
    buf_puts(b, t.as.struct_.def && t.as.struct_.def->name
                ? t.as.struct_.def->name
                : "struct");
    break;
```

When monomorphization mangles `(Option <X>)` and `<X>` is a `TY_STRUCT`
with `def == NULL` (the elaborator's "tyvar / unknown / opaque container
arg" placeholder — see
[../artifacts/ty-struct-null-def-inventory.md](../artifacts/ty-struct-null-def-inventory.md)),
the mangler emits the literal string `"struct"` as the type-name suffix
and produces `ctor_Option__struct`.

The corresponding monomorph is never declared, because no struct named
`struct` exists in the elaborated program; the call site is therefore an
undeclared-function error at cc time.

The fixtures are not minimal repros of the bug — they all happen to drive
the codegen through an Option-monomorph emission where the inner type
slot was filled with the def-less placeholder.

## When it started

Bisect:

| Commit | Suite |
| --- | --- |
| `e042eb822` slice-5 step 1 (defopaque migration) | 1838 / 44 |
| `2bd25b1b0` slice 4 (`:linear` defstructs lower) | 1839 / 43 |
| `86cbb9700` defstruct-as-defadt graduation | (same fixture failing) |

The bug pre-dates slice 5; it was introduced (or exposed) when
defstruct-as-defadt graduated and `defstruct`s started lowering through
the record-ADT path unconditionally, which sent more `(Option <tyvar>)`
shapes through monomorphization.  Both the slice-4 and slice-5-step-1
PRs claimed "suite green" -- the gate did not catch it.

## Connection to slice 5

The bug **is** the slice-5 problem -- a def-less `TY_STRUCT` leaks into
codegen as a literal name.  Two ways forward, in order of risk:

1. **Patch the mangler.**  Change `append_type_mangle`'s `TY_STRUCT`
   default arm to emit something the surrounding monomorphization
   recognises as "do not instantiate" (or emit a stable
   per-call-site-unique tag) so the resulting Option monomorph is either
   not generated or matches the def-less convention.  Smallest change;
   does not unblock slice 5 by itself but un-breaks the suite so slice-5
   B work can measure regressions cleanly.
2. **Fix at the source -- slice-5 B (this is what task B is for).**
   Migrate the def-less `TY_STRUCT` producers
   (`elab_fns.c:2437`, `elab_types.c:2321`, `elab_typeclasses.c:2266`)
   to named `TY_TYVAR`.  The mangler's `default:` arm already emits
   `"opaque"` for `TY_TYVAR`, which still collides across tyvars but at
   least matches the kind's convention.  Better: extend the mangler to
   emit `tyvar_<name>` for a named tyvar and skip monomorphization
   entirely for an unresolved tyvar slot.

The right long-term answer is "monomorphization should not be running over
unresolved type arguments at all" -- but that is the slice-5 deletion's
scope, not a fix for this report.

## Suggested next step (de-risks B)

Before resuming slice-5 B2:

a. Patch `append_type_mangle` `TY_STRUCT` def=NULL arm to call into the
   `TY_TYVAR` arm (or share a helper), so the bug presents identically
   regardless of which placeholder representation is used.
b. Suppress the Option-monomorph emission when ANY type argument is an
   unresolved placeholder (def-less `TY_STRUCT` OR unnamed `TY_TYVAR`).
   The call site is dead from the codegen's perspective -- the value is
   carried as int64 via the carrier path.
c. Confirm `bash tests/run.sh` returns to green, then proceed with B2.

This is plausibly a one-day fix; it is the smallest change that restores
the suite gate so slice-5 B can land in safely-measurable increments.

## Notes

- The slice-4 and slice-5-step-1 commit messages both claim "Suite green
  (1874/0)" / "(1871/0)".  Reproducing locally shows 1838/44 at HEAD --
  neither commit ran a clean local suite before merge, or the reported
  count was a partial subset.  The plan's claim that the gate is green
  is incorrect.
