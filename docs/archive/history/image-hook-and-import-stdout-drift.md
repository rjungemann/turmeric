# Verification trail: image-hook-and-import-stdout-drift

**Resolved / archived:** 2026-06-28

## What the report described

Four gate-suite fixtures with stdout mismatches:

| Fixture | Family | Reported symptom |
| --- | --- | --- |
| `image-hooks-tracked`   | image / persistence | stdout mismatch |
| `image-reload-hook`     | image / persistence | stdout mismatch |
| `image-roundtrip`       | image / persistence | stdout mismatch |
| `load-in-imported-module` | module loading / import | stdout mismatch |

The headline defect was the image init-hook firing on **warm** resume when it
should only run at **cold** load (`init ran` printed twice for
`image-roundtrip`), plus a cross-module `(load ...)` init-order issue for
`load-in-imported-module`.

## How resolution was confirmed

1. Built the Debug compiler from a clean tree
   (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5`
   then `cmake --build build -j`).
2. Full gate run: `bash tests/run.sh` -> `summary: 1870 passed, 0 failed`.
   All four fixtures reported `PASS`.
3. Confirmed the pass is a real fix, not a masked snapshot: inspected each
   `expected.stdout`. They still encode the CORRECT behavior --
   `image-roundtrip`'s expected output has `init ran` only under `cold:`, never
   under `warm:` -- and the actual output matches. If the snapshot had been
   rewritten to accept the bug, the warm section would contain a second
   `init ran`; it does not.
4. Determinism: re-ran all four fixtures individually twice with a fresh
   `/tmp/tur-build` between each; every run matched `expected.stdout`.

The init-hook double-fire and the cross-module load init-order both behave
correctly on the current default by-value path, so the report is closed.
