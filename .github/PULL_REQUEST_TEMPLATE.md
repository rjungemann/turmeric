## Summary

<!-- What does this change do? -->

## Codegen-touching changes

<!-- Did this PR modify any file under src/codegen/ or change emitted C output? -->

- [ ] No codegen change (skip the snapshot section below)
- [ ] Yes — codegen changed, snapshots regenerated in this PR

**If yes, two-commit convention:**

1. Commit 1 — the codegen change itself (`src/codegen/` + tests)
2. Commit 2 — `tur run regen-snapshots`, then paste the summary below

**Snapshot diff summary** (paste output of `git diff HEAD~1 -- 'tests/fixtures/**/expected.c' | python3 tools/snapshot-diff-summary.py`):

```
(paste here)
```

## Test plan

- [ ] `bash tests/run.sh` passes (zero FAIL lines)
- [ ] Snapshot check passes: `tur run regen-snapshots -- --check`
