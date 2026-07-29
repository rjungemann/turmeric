# gc-leak-gate: `gc-collects-strong-cycle` sanitized output drifts on Darwin

**Status:** resolved 2026-07-29 -- fix direction (1). See
[Resolution](#resolution-2026-07-29--fix-direction-1).

**Severity:** low (opt-in diagnostic gate only; one of its 11 checks red on
macOS; no evidence of a collector defect). Pre-existing: reproduces on `main`
before the rc-scalar-default-glue fix, byte-identical output.


## Resolution (2026-07-29) -- fix direction (1)

Taken as filed: `tests/run-gc-leak-gate.sh` no longer runs an output check of
any kind on a fixture whose stdout comes from a malloc probe. The
`ASAN_ONLY_FIXTURES` list is renamed `PROBE_OUTPUT_FIXTURES` -- it now governs
both the on/off control (which it already excluded) and the expected-output
comparison (which it did not). Both skips print a line naming the reason; they
are not silently absent.

The fixture's own inline-C comment and the CG7 plan notes carry the same
measurement so nobody has to re-derive it.

### The measurement that settles which direction to take

Run on Linux/glibc, this tree, `gc-collects-strong-cycle` built both ways:

|            | collector on | collector off |
| ---        | ---          | ---           |
| plain      | `0`          | `1087232`     |
| ASan       | `0`          | `0`           |

So on glibc the check the report found red on Darwin was **already not
asserting anything on Linux**: it compared `0` against an expected `0` that the
collector had no hand in producing. The bottom row is the whole story -- the
probe cannot see the program's heap once ASan is underneath it, and every
consequence follows:

- the on/off control is impossible (identical output either way) -- which the
  gate already knew, and
- the expected-output check is vacuous on glibc and quarantine-inflated on
  Darwin (~800000 = 160 B/iteration of ASan-quarantined frees over 5000
  iterations, exactly as the report computed).

### Why not fix direction (2)

Making the Darwin probe return 0 under `__has_feature(address_sanitizer)` would
turn Darwin green, but it keeps a check that measures nothing looking like a
check that passed -- in a gate whose own header argues at length against exactly
that ("LSan would silently pass both sides of the pair ... leave it off rather
than imply an assertion it is not making"). It would also push sanitizer
conditionals into a fixture that `tests/run.sh` compiles unsanitized, where the
probe is the real assertion and must stay untouched.

Direction (1) keeps the honest split: the sanitized gate asserts what it can see
(ASan-cleanliness of the collector's sweep, which is why this fixture is in the
gate at all), and `tests/run.sh` asserts the heap delta on a build where that
number means something.

### Verification

- `bash tests/run-gc-leak-gate.sh` -- 14 passed, 2 skipped, 0 failed (Linux).
- `bash tests/run.sh` -- 2412 passed, 0 failed; `gc-collects-strong-cycle` still
  asserts `0` unsanitized, which is the check that was never in question.

**Not verified on Darwin.** No macOS host in this session. The change is a
harness-side skip that removes the failing comparison outright, so the reported
`10 passed, 1 failed` becomes a pass by construction rather than by fixing a
measurement -- but the Darwin run itself is unconfirmed. The two ASan-clean
checks for this fixture are untouched and were already green there.

---

## Original report

## Repro

```sh
bash tests/run-gc-leak-gate.sh
# => FAIL gc-collects-strong-cycle-collector-on-output-matches
#    gc-leak-gate: 10 passed, 1 failed        (macOS)
```

The fixture's first heap-delta line prints `800000` instead of `0` -- but only
when compiled with `-fsanitize=address`, and only on Darwin. The plain
(unsanitized) build matches `expected.stdout` exactly, and the two ASan-clean
checks plus the collector-off control still pass.

## Root cause direction

The same probe-vs-allocator mismatch family as the two archived Darwin heap
reports (`gc-heap-struct-rc-nonzero-on-darwin.md`,
`gc-heap-struct-rc-darwin-probe-drift` before it):

- On glibc, `mallinfo2` reads glibc's allocator, which ASan **replaces** -- so
  the sanitized delta reads 0 and the check is vacuously green (the CG7 notes
  record exactly this blindness).
- On Darwin, `malloc_zone_statistics(malloc_default_zone(), ...)` reads
  whatever zone ASan installs, so the probe is NOT blind there -- and ASan's
  quarantine keeps freed blocks accounted as in-use, so a fixture that frees
  its garbage still shows a large positive `size_in_use` delta. `800000` over
  5000 iterations is a clean 160 B/iteration of quarantined frees, not a leak.

The gate's own design notes anticipated the class ("a heap probe that does not
measure what the assertion assumes") but only excluded the glibc-blindness
direction; the Darwin-quarantine direction was presumably never run on a Mac
(the plan records `11 passed, 0 failed`, measured on Linux).

## Fix directions

1. Skip (or degrade to ASan-clean-only) the `output-matches` check for
   fixtures whose expected output comes from a malloc probe rather than the
   CG6 counters, matching the exemption the gate already applies to the
   on/off control for this same fixture.
2. Or make the fixture's Darwin probe return 0 under
   `__has_feature(address_sanitizer)`, the vacuous-probe pattern the archived
   Darwin report already applied to the non-sanitized flake.
