---
title: Two Codegen Snapshots Committed Stale on main (off-by-one __fn_* gensym)
category: Reported Bug
description: The `expected.c` snapshots for `tests/fixtures/fn-typed-return-thin-closure/` and `tests/fixtures/closure-transitive-grandparent-capture/` do not match what the current tree's compiler emits. The only difference is a uniform off-by-one shift in internal `__fn_<N>` gensym numbering (e.g. `__fn_573` -> `__fn_574`); the generated code is otherwise byte-identical and the programs run correctly. `bash tests/run.sh` reports two "codegen mismatch" FAILs on a pristine main/branch checkout, before any feature work. Regenerated as part of the fn-type-bare-identifier work since the repo policy forbids leaving codegen mismatches.
---

# Two Codegen Snapshots Committed Stale on main -- Reported Bug

> **Status:** FIXED in this session (snapshots regenerated). Filed for the
>   record because the breakage pre-existed on `main` and points at a
>   snapshot-generation hygiene gap.
> **Found:** 2026-06-04, while running `bash tests/run.sh` for Phase 1 of
>   [fn-type-bare-identifier-plan](../upcoming/fn-type-bare-identifier-plan.md).
> **Severity:** Low -- purely cosmetic internal symbol numbering; the
>   emitted C is otherwise identical and the fixtures' `expected.stdout`
>   runtime checks pass. But it is a *hard* suite FAIL ("codegen mismatch"),
>   so it makes `tests/run.sh` red on a clean checkout and would block any
>   PR under the repo's "zero FAIL lines" rule.

## Summary

On a pristine checkout of `main` (== branch HEAD, commit `fe45dd5`), two
snapshot fixtures fail with "codegen mismatch":

- `tests/fixtures/fn-typed-return-thin-closure/`
- `tests/fixtures/closure-transitive-grandparent-capture/`

The committed `expected.c` differs from `tur emit-c <input>` only in the
internal `__fn_<N>` gensym numbers, uniformly shifted by one
(`__fn_573` -> `__fn_574`, `__fn_597` -> `__fn_598`, ...). Filtering those
lines out, the diff is empty:

```sh
diff tests/fixtures/fn-typed-return-thin-closure/expected.c \
     <(./build/tur emit-c tests/fixtures/fn-typed-return-thin-closure/input.tur) \
  | grep '^[<>]' | grep -vE '__fn_[0-9]+'
# (no output -- only gensym numbers differ)
```

`tur emit-c` is deterministic across repeated runs, so this is not flakiness.

## Root cause

Both snapshots were introduced by `0dc3e69` ("Fix fn-typed-return lowering
for thin closures in defn (#222)"). The commits after it
(`a63b998`, `c4ac474`, `fe45dd5`, all "Doc stuff") touch only `TEMP.md`,
`docs/`, and new `expected.stdout`-only fixtures -- nothing that feeds the
`__fn_*` gensym counter of an *unrelated* fixture (each fixture compiles
independently). Since the current tree is deterministic and emits `__fn_574`
where the committed snapshot says `__fn_573`, the snapshot must have been
generated against a tree state with one fewer auto-loaded lambda than what
`#222` actually committed -- i.e. the snapshot was stale at commit time.

## Fix / validation

Regenerated both snapshots from the current tree:

```sh
for d in fn-typed-return-thin-closure closure-transitive-grandparent-capture; do
  ./build/tur emit-c tests/fixtures/$d/input.tur > tests/fixtures/$d/expected.c
done
```

After regeneration both fixtures match and `bash tests/run.sh` shows zero
FAIL lines.

## Prevention

The codegen change in `#222` should have regenerated *all* snapshots and
committed them alongside (per CLAUDE.md's "Fixture Snapshots -- STRICT
RULE"). A pre-PR `bash tests/run.sh` would have caught the mismatch. No code
fix is needed beyond the regenerated snapshots; this report exists so the
hygiene gap is not silently repeated.
