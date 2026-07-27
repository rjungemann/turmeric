# Timing-sensitive ctest targets report false failures under parallel/contended runs

**Severity:** low (no product defect) -- but it costs real investigation time
and, worse, teaches people that red is normal.

Reported by the agent working on
[Trowel](https://github.com/rjungemann/trowel) (`turi_fixture_tests` fails
under `ctest -j4`, passes in isolation). Hit independently here twice, on two
different targets, from a different cause -- so the pattern is broader than one
test.

## Summary

Several targets assume they have the machine. When they do not, they fail in
ways that look like genuine product bugs and cost a real investigation before
being dismissed.

Three observed instances:

| Target | Trigger | Symptom |
|---|---|---|
| `turi_fixture_tests` | `ctest -j4` | fails under `-j4`, passes alone (~162s alone) |
| `tur_repl_spice_watch` | a concurrent `tests/run.sh` | `rp6-watch-with-help`, `rp6-watch-unknown-flag` fail; both pass when run directly |
| 25 x `refine-*` fixtures | rebuilding `./build/tur` mid-run | `build failed`, with `actual.stderr` reading `timeout: failed to run command './build/tur': Permission denied` |

The third is not contention but the same class of problem: the suite reads the
binary it is testing straight out of `build/`, so anything that relinks while
it runs poisons an arbitrary subset of fixtures with an error message that
looks nothing like its cause.

## Repro

```sh
# In one shell:
bash tests/run.sh
# In another, while the above is running:
ctest --test-dir build -R tur_repl_spice_watch --output-on-failure
```

`rp6-watch-with-help` fails claiming the help output does not mention
`tur repl`. It does:

```sh
$ ./build/tur repl --help | head -2
usage:
  tur repl [--watch]   start the interactive REPL
```

## Why it misleads

The failure text never mentions timing. `expected help output to mention 'tur
repl'` reads as a broken argument parser, and `build failed` on 25 refine
fixtures reads as a compiler regression. Both cost a full diagnostic pass
before the environment turns out to be the culprit. `CLAUDE.md` already warns
that a slow full-suite run usually means CPU contention rather than a hang;
the same warning is worth extending to *failures*, not just wall-clock.

## Fix directions

Cheapest first:

- **Note it where it bites.** `CLAUDE.md`'s "READ BEFORE ASSUMING A HANG"
  section covers duration; add that timing-sensitive ctest targets can report
  false *failures* under contention, and that overlapping runs should be
  avoided rather than interpreted.
- **Make the poisoned-binary case self-describing.** `tests/run.sh` could stat
  `$TUR` once at startup and re-check before reporting a `build failed` batch,
  turning `Permission denied` into "the compiler changed underneath this run".
- **Raise or remove the timing assumptions** in `tur_repl_spice_watch` and
  `turi_fixture_tests` -- e.g. `ctest` `RUN_SERIAL` on the ones that genuinely
  cannot share, or generous timeouts where a fixed sleep is doing the work.

`RUN_SERIAL` on `turi_fixture_tests` is probably the single highest-value
change: at ~162s it is the longest target in the suite and the one most likely
to be squeezed.
