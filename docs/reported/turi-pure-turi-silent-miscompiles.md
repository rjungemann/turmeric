# Pure-turi silent miscompiles (11 real interpreter bugs + 1 legit carve)

**Summary:** Of the ~244 pure-turi (no inline-C) fixtures that fail under
`--interpret` after W1-W3, only **12** are *silent* (rc matches the expected exit
but the output is wrong); the other 232 are clean errors. These 12 matter most
because they are **not carve-able as inline-C** and would turn the
allowlist->denylist flip (W5) red while looking like passes. One is a legitimate
path-divergence (carve); the other **11 are real interpreter bugs** -- the
tree-walker returns a wrong value with a zero exit.

**Severity:** High (silent wrong answers on pure-turi programs -- the worst
failure mode, and the compiled path is correct).

## The 12

| Fixture | Got | Expected | Class |
| --- | --- | --- | --- |
| `reader-cond` | `interpreted` | `compiled` | **legit carve** -- `#?(:tur ... :turi ...)` reader conditional; the interpreter output is *correct*, the expected.stdout is compiled-path-specific. Carve `requires.compiled` for turi. |
| `rt-return-dispatch-basic` | `42 / 1` | `42 / true` | return-type dispatch: a `:bool` is shown as `1` (wrong Show instance / bool-as-int) |
| `rt-return-dispatch-param` | (wrong) | -- | same return-type-dispatch family |
| `any-box-adt` | (wrong tag) | -- | Any-type boxing: tag/name mismatch |
| `any-box-struct` | `Point` | `struct` | Any-type boxing: prints the struct name instead of the `struct` tag |
| `any-cast-mismatch-panic` | rc=0 | (nonzero) | Any-cast mismatch should panic; interpreter does not |
| `rc-unique-violation` | rc=0 | (nonzero) | rc-unique violation not detected at runtime |
| `result-of-typed-eq` | `false` | `true` | typed `eq?` over a Result returns wrong |
| `range-bound-show-ord` | (wrong) | -- | range bound / Show / Ord interaction |
| `panic-catch-unwind-defer` | (wrong) | -- | defer + catch-unwind ordering/value |
| `codegen-private-defn-collision` | (wrong) | -- | private defn name resolution under interpret |
| `extern-c-spaced-typeann` | (wrong) | -- | extern-c with a spaced type annotation |

(Reproduce: `ASAN_OPTIONS=detect_leaks=0 ./build/tur <flags> --interpret
tests/fixtures/<name>/input.tur` and diff against `expected.stdout`.)

## Notes / likely groupings

- **`rt-return-dispatch-*` (2)** look like one root cause: return-type-directed
  dispatch picking the wrong instance under the interpreter (a `:bool` printed as
  `1`). Start here -- highest chance of a single fix covering two fixtures.
- **`any-box-*` + `any-cast-mismatch-panic` (3)** are the Any-type machinery: tag
  naming on box, and the cast-mismatch panic not firing.
- **`reader-cond`** is not a bug: carve it. Audit the rest of the
  silent-miscompile set for other `#?(...)` reader-conditional fixtures that are
  also legit divergences (none of the other 11 use `#?`).

## Why these block W5

W5 flips `run-turi.sh` to "run everything minus markers." These 12 currently
*pass* the rc check but produce wrong output, so they would surface as harness
FAILs (or, worse, as silent passes if only exit codes were checked). Each must be
**fixed** in `src/turi/eval.c` (the 11 bugs) or **carved** with a reason
(`reader-cond`). Do NOT bulk-carve the 11 -- they are real interpreter
divergences and carving them hides wrong-answer bugs.

## Status

Filed while executing TI8.b/W4. The `ic_exec_accessor` boolean-return class was
fixed in the same pass; these pure-turi cases are the remaining W4 surface (each
its own root-cause investigation). See
[docs/upcoming/v1/turi-interpreter-gap-closure-plan.md](../upcoming/v1/turi-interpreter-gap-closure-plan.md).
