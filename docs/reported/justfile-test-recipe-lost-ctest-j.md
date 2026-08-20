# Justfile test recipe runs ctest without -j -- the documented soft regression

**Severity: low** (CI wall-clock regression, not correctness). Found in the
2026-08-20 docs audit.

## Repro

The Justfile `test` recipe is
`timeout 300 ctest --output-on-failure --progress --test-dir build` (no `-j`),
which is exactly the "soft regression" test-suite-portability-guide.md
section 5 warns about. The RUN_SERIAL markings that make `-j` safe are still
present (CMakeLists.txt:136,681,705).

## Fix direction

Restore `-j` to the recipe (and revisit the 300s timeout note).

## Guides to update when fixed

- none -- test-suite-portability-guide.md already documents the intended
  state; the guide was deliberately not "fixed" down to the regressed recipe.
