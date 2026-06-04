# Stale codegen snapshot: `pair-signals-typed/expected.c`

**Summary:** `tests/fixtures/pair-signals-typed/expected.c` was a stale codegen
snapshot using an obsolete name-mangling scheme; it failed `tests/run.sh` on a
clean checkout, independent of any local change.

**Severity:** Low (test-suite hygiene, not a miscompile). It nonetheless
violates the repo's STRICT fixture-snapshot rule ("a codegen mismatch failure is
a real failure") and would block any PR run from going green.

## Observed vs. expected

`bash tests/run.sh` reports:

```
summary: ... 1 failed
  - pair-signals-typed (codegen mismatch)
```

The diff is purely in identifier mangling for names ending in `?`/`!`:

```
< static bool contract_enabled_();          (expected.c, stale)
> static bool contract_enabled_qu();        (current compiler)
< static void set_contract_handler_(...);
> static void set_contract_handler_ex(...);
```

The current compiler mangles `?` -> `_qu` and `!` -> `_ex`; the snapshot still
has the older `_` scheme. This was confirmed pre-existing by `git stash`-ing all
local changes, rebuilding, and re-running just this fixture (still MISMATCH).
Every other `expected.c` in the tree already uses the `_qu`/`_ex` scheme, so
this one fixture simply escaped a prior mangling migration's regen sweep.

## Root cause

A past change to the C identifier mangling (`?`->`_qu`, `!`->`_ex`) regenerated
the other snapshots but missed `pair-signals-typed/expected.c`. No `.tur` source
or compiler logic is wrong; only the checked-in snapshot drifted.

## Fix

Regenerated the snapshot with the current compiler:

```sh
./build/tur emit-c tests/fixtures/pair-signals-typed/input.tur \
  > tests/fixtures/pair-signals-typed/expected.c
```

This was done alongside the `stdlib-effect-rows` change (which itself broke zero
snapshots). Validation: `bash tests/run.sh` now reports zero `FAIL` lines.
