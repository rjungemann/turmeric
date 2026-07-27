# gc-leak-gate: `gc-collects-strong-cycle` sanitized output drifts on Darwin

**Severity:** low (opt-in diagnostic gate only; one of its 11 checks red on
macOS; no evidence of a collector defect). Pre-existing: reproduces on `main`
before the rc-scalar-default-glue fix, byte-identical output.

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
