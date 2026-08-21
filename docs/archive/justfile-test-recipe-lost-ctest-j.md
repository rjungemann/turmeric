# Justfile test recipe runs ctest without -j -- the documented soft regression

**Severity: low** (CI wall-clock regression, not correctness). Found in the
2026-08-20 docs audit.
**Status: RESOLVED**, but not the way the report expected -- see below.

## Repro

The Justfile `test` recipe was
`timeout 300 ctest --output-on-failure --progress --test-dir build` (no `-j`),
exactly the "soft regression" test-suite-portability-guide.md section 5 warns
about. The RUN_SERIAL markings that make `-j` safe are still present
(CMakeLists.txt:136,681,705).

## The fix as filed would not have worked

The report says "restore `-j` to the recipe", and section 5 of the guide
documents the recipe as `ctest -j --output-on-failure ...`. **A bare `-j` does
nothing.** Through CMake 3.28 the option is documented as `-j <jobs>` and
requires a value; a value-less `--parallel` only arrived in 3.29. On 3.28 a
bare `-j` is accepted in silence and has no effect.

Measured here (ctest 3.28.3, 4-core box, five non-RUN_SERIAL targets):

| invocation | wall |
|---|---|
| serial (no flag) | 11.16s |
| `ctest -j` | 11.54s |
| `ctest -j 8` | 5.82s |
| `ctest -j "$(getconf _NPROCESSORS_ONLN)"` | 6.07s |

So a bare `-j` is the worst outcome available: it reads in review as the
parallel recipe while behaving as the serial one. That is how the guide's own
snippet came to document a no-op, and restoring it verbatim would have closed
this report while changing nothing.

## Resolution

An **explicit** job count, at all three call sites:

- `Justfile` `test` -- `timeout 720 ctest -j "$(getconf _NPROCESSORS_ONLN)" ...`
- `Justfile` `test-tsan` -- same flag added.
- `.github/workflows/ci.yml` "Run auxiliary suites" -- ~105 targets that were
  running serially in CI. (The step above it runs only `tur_tests`, a single
  RUN_SERIAL target, where `-j` would do nothing.)

`getconf _NPROCESSORS_ONLN` rather than `nproc`: `nproc` is GNU coreutils
only, and the macOS CI runner has neither it nor a value-less `-j`.

The 300s timeout is also raised to 720s, the second half of the report's fix
direction ("revisit the 300s timeout note"). `tests/run.sh` alone is ~265s on
a 4-core box and is RUN_SERIAL, so a 5-minute cap killed the whole suite on
any machine slower than the one it was tuned on -- and a `timeout` kill reads
as a hang, not as a timeout. 12 minutes is the repo-wide suite cap per
CLAUDE.md.

Each site carries a comment recording the bare-`-j` trap so it does not come
back.

## Verification

- `ctest -j "$(getconf _NPROCESSORS_ONLN)"` measured at 6.07s against 11.16s
  serial on the same target set (above).
- `tur run <recipe>` expands the `$(...)` substitution correctly -- checked
  with a scratch Justfile driving a real ctest target through `tur run`.
- `.github/workflows/ci.yml` re-parsed as YAML after editing.

## Guide updated

- docs/guides/test-suite-portability-guide.md section 5 -- the snippet no
  longer documents a no-op, and a new paragraph records the bare-`-j`
  measurement, the CMake 3.28-vs-3.29 split, and why `getconf` over `nproc`.
  The report said "none -- the guide already documents the intended state";
  that was true of the `-j` presence and wrong about its form.

## Follow-up filed

`tur run test` still does not reach ctest: the recipe is `test: build doctest`
and `tools/run-doctests.sh` exits 1 on ~50 pre-existing failures, stopping the
chain before the ctest line. Filed as
docs/reported/tur-run-test-blocked-by-doctest-failures.md. That is orthogonal
to this report -- the ctest line is now correct, but currently unreachable via
`tur run test`.
