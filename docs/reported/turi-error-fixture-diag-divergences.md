# 9 `errors/` fixtures whose diagnostic diverges under `--interpret`

> **Update (2026-06-12): the 3 reporting-stage divergences are FIXED; 6 remain.**
> The interpreter now emits the compiled path's diagnostic for an unbound call
> head:
> - `src/turi/eval.c` -- a deferred runtime-dispatch call head (elab_call.c UCH1)
>   that resolves to an unbound variable is reworded from `unbound variable: NAME`
>   to `unknown function or operator 'NAME'`, with the stdlib load-hint appended
>   for known helpers (`tur_stdlib_load_hint`, now shared via `expr.h` so the
>   compiler and interpreter use one curated table).
> - `src/main.c` (`cmd_eval`) -- a runtime error raised while running `main` is
>   now printed to stderr (and an uncaught throw reported) instead of being
>   swallowed (non-zero exit, empty stderr).
>
> `unbound-call-head`, `unknown-helper-load-hint`, and `tce3-map-heterogeneous-val`
> (the last already recovered once `map.tur` joined the elaborator's view) now
> emit their `expected.diag` under `--interpret` and were removed from
> `TURI_ERRORS_DENY`. The **6 remaining** divergences are the 4 missing-check
> cases (`lifetime-cyclic`, `reader-macros-strict-collision`, `lang-unknown`,
> `lang-not-implemented` -- each needs the corresponding elaborator pass enabled
> under `--interpret`) and the 2 TI3.2 serial-shift carve-outs (blocked on
> [turi-capturing-shift-unimplemented.md](turi-capturing-shift-unimplemented.md)).
> This report stays open until those land.

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
| `unbound-call-head` | `unknown function or operator 'foo'` | ~~exits 1, **empty stderr**~~ **FIXED** -- now reports the diag at runtime | reporting-stage |
| `unknown-helper-load-hint` | `unknown function or operator 'float->int'` | ~~no elab diag / load hint~~ **FIXED** -- diag + load-hint emitted | reporting-stage |
| `tce3-map-heterogeneous-val` | `TUR-E0001` | ~~exits 1, empty stderr~~ **FIXED** -- emits `TUR-E0001` | reporting-stage |
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
