# 9 `errors/` fixtures whose diagnostic diverges under `--interpret`

**Summary:** With `tests/fixtures/errors/*` now wired into `run-turi.sh` (W3),
**282 of 298** negative fixtures emit the exact same diagnostic under
`tur --interpret` as on the compiled path -- the interpreter shares the
elaborator, so move/linearity/affine/type checks match by construction. **9**
genuinely diverge: the interpreter either succeeds where the compiler errors, or
errors at runtime (empty stderr) instead of emitting the elaboration diagnostic
the fixture expects. They are denylisted in `run-turi.sh` (`TURI_ERRORS_DENY`)
with a one-line reason each; this report is the consolidated tracking entry.

**Severity:** Low-Medium. None are silent miscompiles of *positive* programs;
they are gaps in the interpreter's *error-reporting* path. Most surface as "no
diagnostic" or "wrong-stage diagnostic," not wrong answers.

## The 9 divergences

| Fixture | Expected diag | Interpreter behaviour | Class |
| --- | --- | --- | --- |
| `unbound-call-head` | `unknown function or operator 'foo'` | exits 1, **empty stderr** (errors at runtime, not as an elab diag) | reporting-stage |
| `unknown-helper-load-hint` | `unknown function or operator 'float->int'` | same -- no elab diag / load hint | reporting-stage |
| `tce3-map-heterogeneous-val` | `TUR-E0001` | exits 1, empty stderr (runtime, not elab) | reporting-stage |
| `lifetime-cyclic` | `TUR-E0106` | **exits 0** -- lifetime-cycle check not run | missing-check |
| `reader-macros-strict-collision` | `already registered` | exits 0 -- strict-collision not raised | missing-check |
| `lang-unknown` | `not yet implemented` | **runs the program** (prints output) | missing-check |
| `lang-not-implemented` | `not yet implemented` | no diag | missing-check |
| `serial-context-not-capturable` | `TUR-E0706` | exits 1, empty stderr | TI3.2 carve-out |
| `serial-context-do-not-capturable` | `TUR-E0706` | exits 1, empty stderr | TI3.2 carve-out |

## Notes by class

- **reporting-stage (3).** The interpreter *does* reject these (non-zero exit),
  but the unbound-call / heterogeneous-map error is raised during evaluation and
  printed (if at all) differently from the compiled elaboration diagnostic.
  `cmd_eval` treats elaboration errors as "already printed by the diagnostic
  system" (`src/main.c`), but for these the diagnostic system emits nothing --
  the failure is detected later. Fix: route the unbound-call / heterogeneous-map
  checks through the same diagnostic emission the compiled path uses, or have
  `cmd_eval` print the `turi_error` message to stderr when no diagnostic was
  emitted.
- **missing-check (4).** `lifetime-cyclic` (`TUR-E0106`), the reader-macro
  strict-collision, and the `#lang` unknown/not-implemented checks are not run
  on the interpret path at all -- the program elaborates/runs clean. These are
  genuine interpreter coverage gaps in specific elaborator passes; each needs the
  corresponding check enabled under `--interpret`.
- **TI3.2 carve-out (2).** `serial-context-{,do-not-}capturable` exercise the
  `TUR-E0706` serial-shift capturability diagnostic, part of the
  context-capturing-shift carve-out
  ([turi-capturing-shift-unimplemented.md](turi-capturing-shift-unimplemented.md)).
  Expected to stay denylisted until TI3.2 lands.

## Validation

For each, after a fix: `tur <flags> --interpret <input>` must put the
`expected.diag` substring(s) on stderr; then remove the entry from
`TURI_ERRORS_DENY` in `tests/run-turi.sh` and confirm the errors pass stays
green.

## Status

Filed while executing TI8.b/W3 (the errors/ harness-coverage piece). The 282
matching fixtures are now CI-validated under turi; these 9 are tracked here and
denylisted. See
[docs/upcoming/v1/turi-interpreter-gap-closure-plan.md](../upcoming/v1/turi-interpreter-gap-closure-plan.md).
