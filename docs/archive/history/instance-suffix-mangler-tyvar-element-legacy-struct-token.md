# Fix paper trail -- instance-suffix mangler tyvar element token

Resolved 2026-06-22.

## Problem

`build_inst_type_suffix` (`src/compiler/elab_typeclasses.c`) builds the
per-instance C identifier suffix from the instance head's type args. After the
ECS E2d-P6 work named the parametric head element (`A` in `(Dense A)`) as a
real `TY_TYVAR` (so associated-type element projection can recover it), the
TY_APP arm special-cased a `TY_TYVAR` head-arg back to the legacy `"<struct>"`
token to avoid renaming ~85 unrelated parametric instances' C symbols. That
left the mangler "lying": a real type variable mangled as `<struct>`, coupling
the type representation to a C-identifier back-compat concern.

## Fix

Dropped the special-case. The TY_APP arm now renders the head-arg via
`type_name(*aarg)` unconditionally; a `TY_TYVAR` renders as `"tyvar"` (the
`type_name` default at `types.c:1480`). The element name half is normalized,
so two declarations of the same parametric instance that pick different tyvar
letters still mangle identically, and the conflict-comparison path
(`build_inst_type_suffix` called on `prev` around line 2768) stays stable.

```c
/* before */
const char *n = (aarg->kind == TY_TYVAR) ? "<struct>" : type_name(*aarg);
/* after */
const char *n = type_name(*aarg);
```

## Snapshot regen

Per CLAUDE.md's codegen-change protocol, all `fixtures/*/expected.c` were
regenerated in the same commit. 86 fixtures changed; the diff is *purely* the
`__ltstruct_gt` -> `_tyvar` token rename:

```sh
git diff -- 'tests/fixtures/*/expected.c' | grep -E '^[+-]' \
  | grep -vE '^(\+\+\+|---)' | grep -cvE 'ltstruct_gt|_tyvar'
# => 0   (no changed line lacks one of the two tokens)
```

No function body changed; the behavioural surface is identical.

## Verification

```sh
cmake --build build -j --config Debug
bash tests/run.sh
# => summary: 1760 passed, 0 failed
```
