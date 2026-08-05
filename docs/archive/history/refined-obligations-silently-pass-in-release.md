# `refined` obligations "silently pass" in Release -- NOT A BUG (retracted)

**Resolved 2026-07-31: the report was wrong.** Filed the same day at
`docs/reported/refined-obligations-silently-pass-in-release.md` claiming
**high severity** -- that a Release `tur` "reports an experiment that checks
nothing." That claim does not survive reading the code. The behavior is
deliberate, documented, and has an opt-in flag.

## What the report got wrong

It concluded that refinement obligations were *not detected* in Release. They
are detected exactly as in Debug. What differs is whether the **runtime
contract check** an unproved obligation falls back to is *emitted*, and that
is CT3 policy, not a refinement defect:

```c
/* elab_fns.c:250 */
bool rt_contracts_emitted(void) {
#ifdef NDEBUG
    if (!g_keep_contracts_in_release) return false;
#endif
    return !g_no_contracts;
}

/* elab_fns.c:5287 -- the CT1 contract-injection gate */
bool should_check = true;
#ifdef NDEBUG
    if (!g_keep_contracts_in_release) should_check = false;
#endif
```

Contract checks are stripped from Release builds unless `--keep-contracts`
is passed (`main.c:8956`, `--keep-contracts   retain contract checks in
release builds (CT3)`). This is the same bargain C makes with
`assert()`/`NDEBUG`. Refinement obligations ride on that mechanism by design
-- `refine_discharge.c:11` says an obligation no backend can decide "falls
straight to its runtime contract check" -- so they inherit the policy.

Verified directly, on one fixture whose contract must fire:

| build | flags | exit |
| --- | --- | --- |
| Debug | `--enable=refined` | 134 (SIGABRT -- fires) |
| Release | `--enable=refined` | 0 (stripped) |
| Release | `--enable=refined --keep-contracts` | **134 (fires)** |

The opt-in restores it exactly. Nothing is unsound, and no obligation is
mis-proved.

## What was actually wrong -- the fixtures, and it is fixed

The nine `refine-*` fixtures asserted a runtime abort while declaring only
`--enable=refined`, so they silently depended on `tur` having been built
Debug. They passed under `tests/run.sh` (which uses `./build/tur`) and failed
under any Release-built `tur` -- which is how this surfaced at all: the
mir-interp-tier I0 spike measured on Release.

Fixed by making each fixture state its own requirement instead of inheriting
it from the build type -- `--keep-contracts` added to the `flags` of:

```
refine-let-shadow-not-split        refine-typeclass-not-congruent
refine-match-arm-hyps-not-shared   refine-typeclass-result-unproved
refine-match-field-wrong           refine-off-is-contracts-only
refine-match-impure-arm            errors/refine-impure-predicate
refine-memo-distinct-obligations
```

`errors/refine-impure-predicate` is the same cause by a different route: with
contracts stripped, CT1 injection never runs, so the predicate-purity check
never fires and TUR-E0375 is never reported. `--keep-contracts` restores the
diagnostic.

`refine-off-is-contracts-only` is the one to keep in mind if these are ever
revisited: it deliberately runs with refinement OFF to assert contracts still
work, so `--keep-contracts` is its *only* flag.

After the change all nine behave identically on Debug and Release.

## Verification

- `tests/run.sh` (Debug): 2499 passed, 0 failed
- `tests/run-jit.sh` (Release, `-DTUR_JIT=ON`): **2414 passed, 0 failed**,
  47 skipped -- previously 9 failed

## Lesson

The report was filed off a Debug-vs-Release behavioral difference without
reading the gate that produced it, and reached for the most alarming reading
("checks nothing") rather than the most likely one (a documented build-mode
policy with a flag). One `grep` for `NDEBUG` in `elab_fns.c` -- which the
report itself noted it had not run to ground -- would have settled it.

The residual value is real but small: a fixture that depends on a build mode
should say so in its own `flags`, not inherit it from how the compiler
happened to be compiled.
