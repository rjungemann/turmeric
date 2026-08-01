---
status: NOT FIXED -- ABSORBED 2026-08-01 into
  docs/reported/turi-return-directed-method-keeps-baked-instance.md
severity: medium
discovered: 2026-07-31
area: interpreter (turi HKT dict routing, Option value readback)
---

# turi: `bind`-then-`pure` over constrained `^m` at `(Option int)` returns the is-some flag, not the value

**The defect is still open; only this file is closed.** This was a duplicate:
it re-discovered, a day later, the same fixture and the same three numbers
already filed as
`turi-hkt-constrained-byvalue-bind-pure-wrong-values.md`, and both are symptom
reports on one root cause -- `--interpret` keeping the elaboration-baked
instance for a RETURN-directed class method. Read
[`turi-return-directed-method-keeps-baked-instance`](../reported/turi-return-directed-method-keeps-baked-instance.md)
instead.

The "is-some flag / wrong-slot readback" reading below is wrong: nothing is
being read back, because no `pure` runs. See the payload-independence probe in
the successor.

What this report established and the successor keeps: the failure is
**pre-existing**, verified independently of the other duplicate by reverting
consolidation increment 3's only interpreter-side change.

The `cps-tramp-resume-*` observation in the Notes below is a separate,
harness-level issue tracked in
[`ci-cps-tramp-turi-timeouts-under-load`](../reported/ci-cps-tramp-turi-timeouts-under-load.md).

## Summary

`tests/fixtures/hkt-constrained-byvalue-bind-pure` passes compiled but fails
under `tur --interpret`: it prints `1 / -1 / 1` where `42 / -1 / 7` is
expected. The wrong values look like the `Option`'s `is-some` flag (or a
constant 1) being surfaced where the payload (`41+1`, `7`) should be --
i.e. an interpreter-side readback of the wrong slot after the dict-routed
`bind`/`pure` composition. The short-circuit row (`-1`) is correct, so
construction and none-detection work; only the some-payload readback is off.

Found while running `bash tests/run-turi.sh` during consolidation
increment 3 (2026-07-31). Verified NOT caused by that increment: the
failure reproduces with the increment's only interpreter-side change
(`try_retag_carrier_struct` 1-field relax) reverted, and the increment's
other changes are emit-only, which `--interpret` never executes.

## Repro

    ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret \
        tests/fixtures/hkt-constrained-byvalue-bind-pure/input.tur
    # prints: 1 / -1 / 1   (expected: 42 / -1 / 7)

## Root cause (direction)

Unknown; candidates are the turi paths mirroring the fixture's three
compiled fixes (return-directed `pure` against the constraint, the
continuation's boxed `(Option int)` return, the Route B dict-clone
routing). `unwrap-or` receiving a mis-tagged/mis-slotted TuriValue after
the dict-routed bind would print exactly a struct header word or bool
flag. Bisect against main history where `run-turi.sh` was last green.

## Notes

- Two more `run-turi.sh` reds observed in the same session,
  `cps-tramp-resume-deep` / `cps-tramp-resume-multicase`, produced EMPTY
  stdout under the parallel harness but pass standalone -- consistent with
  the per-fixture 15s interpreter timeout under load, not a product bug.

## Guide upkeep

If resolving this changes any representation/bridge described in
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
update the guide in the same PR.
