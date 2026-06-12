# 9 `errors/` fixtures whose diagnostic diverges under `--interpret`

> **RESOLVED (2026-06-12, pass 4): all 9 closed; `TURI_ERRORS_DENY` is now
> empty.** The last 2 -- `serial-context-not-capturable` and
> `serial-context-do-not-capturable` -- were recovered when the context-capturing
> shift landed in the interpreter: `ts_capture_and_run` (`src/turi/eval.c`) now
> rejects an uncapturable delimited context with the compiled path's `TUR-E0706`
> ("serial-shift context is not capturable") under `--interpret`, so both emit
> their `expected.diag`. Both removed from `TURI_ERRORS_DENY` (the denylist is
> now empty -- every `errors/` negative fixture's diagnostic matches under the
> interpreter). See
> [turi-capturing-shift-unimplemented.md](turi-capturing-shift-unimplemented.md).
> This report is fully resolved.

> **Update (2026-06-12, pass 3): `reader-macros-strict-collision` FIXED; only
> the 2 TI3.2 serial-shift carve-outs remain (blocked elsewhere).**
> The interpreter's `TuriEnv` reader-macro registry defaults to `strict=false`
> *by design* so the REPL's `src_acc` replay and iterative redefinition stay
> smooth (re-parsing the accumulated history re-runs every prior
> `reader-macros/define`, and the REPL wants last-wins, not a collision). But
> `tur --interpret <file>` is a **batch compile of a single file**, not an
> interactive session -- the user file is parsed in one pass and the preloaded
> stdlib registers no reader macros, so there is no legitimate self-replay to
> protect. The fix sets `env->reader_macros->strict = true` in `cmd_eval`
> (`src/main.c`) only; the REPL (`turi/repl.c`) keeps the default. A reader
> macro registered twice with a differing template now hard-errors under
> `--interpret`, matching the compiled path, while REPL iterative redefinition
> still updates silently (verified: `#sum` redefined then `#sum[3 4]` => 12, no
> error). `reader-macros-strict-collision` removed from `TURI_ERRORS_DENY`.
>
> The reporting-stage and missing-check cases are now ALL closed. **Only 2
> items remain** -- `serial-context-not-capturable` and
> `serial-context-do-not-capturable` -- and both are TI3.2 carve-outs blocked on
> [turi-capturing-shift-unimplemented.md](turi-capturing-shift-unimplemented.md),
> which tracks the underlying `serial-shift`/`cloneable-shift` interpreter
> feature. This report has no remaining *independent* work; it stays open only
> as a tracking pointer until that feature lands and the two carve-outs can be
> un-denylisted. Full turi harness after this pass: 952 passed, 0 failed.
>
> **Update (2026-06-12, pass 2): 3 of the 6 missing-check cases FIXED; 3 remain.**
> Two narrow, parity-only additions to the interpreter's pre-eval pipeline in
> `src/turi/eval.c` (`turi_eval_impl`):
> - **`#lang` not-yet-implemented** (`lang-unknown`, `lang-not-implemented`) --
>   after `detect_lang` finds a directive, the interpreter now rejects an
>   unimplemented reader via `reader_type_is_implemented` (mirroring
>   `detect_and_adjust_lang` in `src/main.c`) instead of silently stripping the
>   directive and running the program under the default reader. It now prints
>   `tur: error: #lang <name> is not yet implemented`.
> - **`TUR-E0106` lifetime-cycle check** (`lifetime-cyclic`) -- the always-on
>   `lifetime_check_program` pass (compiled path: `PASS_BORROW_CHECK`) is now run
>   on the elaborated program after the effect-row pass. It only emits on a
>   genuine outlives cycle (`&'a &'b` / `&'b &'a`), a shape no well-formed
>   program contains, so no positive program is affected. The full move/borrow
>   checker stays with the elaborator (already shared).
>
> All three removed from `TURI_ERRORS_DENY`; `TURI_FILTER=errors/ bash
> tests/run-turi.sh` is green (301 passed, 0 failed) and the full harness is 951
> passed, 0 failed. **3 remain:** `reader-macros-strict-collision` (the
> interpreter's `TuriEnv` reader-macro registry is `strict=false` *by design*
> for REPL `src_acc` replay -- making file-eval strict needs the strict-vs-replay
> reconciliation, not just "enable a pass") and the 2 TI3.2 serial-shift
> carve-outs (blocked on
> [turi-capturing-shift-unimplemented.md](turi-capturing-shift-unimplemented.md)).
>
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
| `lifetime-cyclic` | `TUR-E0106` | ~~exits 0 -- lifetime-cycle check not run~~ **FIXED** -- `lifetime_check_program` now run under interpret | missing-check |
| `reader-macros-strict-collision` | `already registered` | ~~exits 0 -- strict-collision not raised~~ **FIXED** -- file-eval registry now strict (REPL stays non-strict) | missing-check |
| `lang-unknown` | `not yet implemented` | ~~runs the program (prints output)~~ **FIXED** -- rejects unimplemented reader | missing-check |
| `lang-not-implemented` | `not yet implemented` | ~~no diag~~ **FIXED** -- rejects unimplemented reader | missing-check |
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
- **missing-check (4) -- ALL FIXED.** `lifetime-cyclic` (`TUR-E0106`), the
  reader-macro strict-collision, and the `#lang` unknown/not-implemented checks
  previously did not run on the interpret path -- the program elaborated/ran
  clean. Now closed: the `#lang` reject and `lifetime_check_program` run in
  `turi_eval_impl` (`src/turi/eval.c`), and the reader-macro registry is strict
  for file-eval (`cmd_eval`, `src/main.c`) while the REPL stays non-strict. See
  the pass-2 and pass-3 update blocks at the top.
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
