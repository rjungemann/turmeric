---
status: open
severity: medium
discovered: 2026-08-01
area: JIT engine (MIR) x GC / Rc / weak refs, macOS arm64 only
---

# Six GC / Rc / weak-reference fixtures fail under the JIT engine on macOS only

## Summary

With the harness bug in
[`ci-macos-suites-fail-while-linux-passes`](ci-macos-suites-fail-while-linux-passes.md)
fixed, `JIT engine (macos-latest)` went from 407 failures to **6**. These six
are the real findings that bug was masking -- they were always failing, buried
in a list of 401 spurious `jit diagnostic mismatch` lines nobody could read
past.

```
jit fixture summary: 2409 passed, 6 failed, 47 skipped
failed:
  - gc-auto-collects-without-gc-call
  - gc-collects-strong-cycle
  - gc-live-cycle-survives
  - hkt-fmap-rc-result-droppable
  - hkt-instance-rc-construct-result
  - weak-breaks-parent-child-cycle
```

(PR #753, run 30685603734, job 91330616390, head `03ec0d4a`.)

They are one coherent family: **cycle-collecting GC, `Rc`, and weak
references.** Nothing else in the corpus fails.

## What is and is not affected

| Configuration | Result |
| --- | --- |
| JIT engine, macOS arm64 | **these 6 fail** |
| JIT engine, Linux x86-64 | green -- `tur_jit_fixture_tests ... Passed`, 0 failed, 418.97s (run 30685603734, job 91330616363) |
| AOT (`cc`), macOS arm64 | green -- these are not among that leg's 4 failures, which are all int-conversion straddles |
| AOT (`cc`), Linux | green (`tests/run.sh` 2500 passed, 0 failed) |

So it is specifically **JIT x macOS**. The Linux JIT leg being green is a
real signal, not `continue-on-error` masking it: its log shows the ctest target
passing outright.

## Not a regression from PR #753

That PR touches `emit_expr.c` / `emit_module.c` (a fat-closure spill shim
that only fires on a shape none of these fixtures reach) and
`tests/run-jit.sh`. The six were present in the 407 on `main` at head
`8b1ea4380` -- the same six names appear at the tail of that run's failed
list. The harness fix changed which failures are *visible*, not which exist.

## The local-vs-CI gap is real here

`.github/workflows/ci.yml:195-212` made the macOS JIT leg blocking on a
hand-measured local baseline of `2414 passed, 0 failed, 47 skipped`. Corpus
totals line up (2409 + 6 = 2415, against 2414 + 1 for this PR's added
fixture), so the same fixtures ran -- but these six passed on the developer's
Mac and fail on the GitHub runner.

Unlike the AOT half of the sibling report, where the local/CI "disagreement"
turned out to be a misreading, this one is genuine and needs explaining. The
most likely axis, unverified: `TUR_DEBUG_SANITIZE` defaults ON everywhere, and
a local macOS build commonly turns it off (or uses Homebrew LLVM) to dodge the
ASan startup deadlock CLAUDE.md documents. A GC that walks its own heap is
exactly the kind of code whose behaviour ASan can change. Worth testing first
because it is cheap and would explain all six at once.

## What was not determined

- **The per-fixture failure mode.** The summary lists names; the
  `FAIL <name> -- <reason>` lines (output mismatch? non-zero exit? timeout?)
  sit above the window that was fetched. Get these first -- "GC did not
  collect" and "binary crashed" are very different investigations.
- Whether the JIT's `cc` fallback is involved. The run reports 17 fixtures
  passing via the fallback (TUR-W0070); if any of the six are fallback
  candidates that stopped falling back, that narrows it quickly.

## Fix directions

1. Get the failure reasons (see above). Everything else is speculation first.
2. Test the sanitizer axis: run the macOS JIT suite once with
   `-DTUR_DEBUG_SANITIZE=OFF` and once ON. If the six flip, it is an
   ASan/GC interaction, not a JIT codegen bug.
3. If they fail at both settings, it is the MIR engine on arm64 -- the
   platform-specific hazards `ci.yml` already names (MAP_JIT/W^X, the AAPCS64
   `__uint128_t` alignment skew) are the places to look for something that
   would corrupt a heap walk.
4. Reproducing needs a macOS box -- unlike the sibling report's two halves,
   this one genuinely does, because Linux JIT is green.

## Related

- [`ci-macos-suites-fail-while-linux-passes`](ci-macos-suites-fail-while-linux-passes.md)
  -- the harness bug that hid these, now fixed. That report closes when its
  AOT half is fixed; this one survives it.
- `.github/workflows/ci.yml:210-212` asks that the **Linux** JIT leg be
  flipped to blocking once it publishes a clean run. It now has
  (run 30685603734). Doing so is a gating-policy change, not part of this
  finding, but the precondition is met and the file asks not to let the
  asymmetry sit indefinitely.
