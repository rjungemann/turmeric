# Quickstart tutorial stack teaches an Option/Result/for/struct API that does not exist

**Severity: high** -- the interactive `:tutorial quickstart` failed at the real
REPL; quickstart.md and repl-tutorial.md walked new users into
unknown-function errors on their first session. Found in the 2026-08-20 docs
audit.
**Status: RESOLVED.**

## Repro

Replaying what a learner types, through one REPL session, produced **9
diagnostics** before the fix and **0** after.

## What was wrong, and one correction to the filing

| taught | reality |
|---|---|
| `option-some` / `option-none` / `option-some?` | `some` / `none` / `some?` |
| `option-unwrap` / `option-unwrap-or` | `unwrap` / `unwrap-or` |
| `(for i 0 5 ...)` counted loop | `for` is the monadic comprehension |
| `(Point-x p)` accessors | field access is `(.x p)` |

The report said the real names include `option-unwrap` and `option-unwrap-or`.
They do not -- both are unknown; the working spellings are `unwrap` and
`unwrap-or`. Checked at the REPL rather than inferred.

## `none?` existed as a name with nothing behind it

The report said there is "**no `none?` predicate at all**". It is worse than
that, and this was the one real code fix here:

- **Compiled path:** `(none? (none))` -> `error: unknown function or operator`.
- **REPL:** `(none? (none))` -> `false`. And `(none? (some 1))` -> `false`.
  **No warning**, because `none?` appears in the compiler's pure-builtin list
  (src/compiler/emit_fns.c:1300, 2555), so the name is "known" and
  runtime-dispatches to a default.

A name that hard-errors compiled and silently answers `false` interpreted is
worse than a missing one: the tutorial could not use it, and anyone who found
it in the builtin list would get a wrong answer with no signal.

`none?` is now implemented in stdlib/option.tur as the complement of `some?`,
and both paths agree.

## Regenerated snapshots

`option.tur` is auto-loaded, so one new function shifted the gensym counter in
**142** `expected.c` files. Regenerated in the same change per CLAUDE.md.
The diff is 24902 insertions against 24902 deletions -- pure renumbering
(`h_787` -> `h_788`), no structural movement.

## Content rewritten

- `tutorials/quickstart.yaml` -- the Option steps, the counted-loop step (now
  `while` + `^mut` + `set!`, with the step retitled, since teaching a counted
  `for` that does not exist was the defect), and the struct step.
- `docs/guides/quickstart.md`, `docs/guides/repl-tutorial.md`,
  `docs/guides/quickstart-tutorial-plan.md` -- the same four corrections, in
  both the s-expr and sweet-exp halves. All 78 sweet-exp pairs still check.

Every replacement was **run** before being written down: the Option examples
produce exactly the values the guides claim (`1 1 0 99 -1 5 -1`), and the
rewritten loop prints `0 1 2 3 4` then `1 4 9 16 25`.

## A second broken tutorial, found by the new harness

`tutorials/eavt.yaml`'s `print-value` matched two of the `Value` ADT's **three**
constructors, so the step failed with a non-exhaustive-match error. Fixed by
adding the `EntityVal` arm, and the step now says why: `match` is checked for
exhaustiveness, so the omission is an error rather than a silent
fall-through -- which makes the fix teach something instead of just compiling.

## Tests

`tests/run-tutorial-quickstart.sh` (ctest `tur_tutorial_steps`) replays **every**
`tutorials/*.yaml` -- the code lines from each step's `instruction` block, then
its `expected` answer, in order, through one REPL session, because later steps
use names earlier steps define. Any error or `TUR-W0040` unknown-name warning
fails it.

Stated in the harness: it proves every step **runs**, not that a step produces
the answer its `success_message` claims -- the format records expected input,
not expected output. That is still the distinction the report was about: a new
user's first session walking into unknown-function errors.

Nothing checked this content before, which is how it drifted: the YAML is data,
so no compiler ever read it.

Suites: run.sh 2676 passed / 0 failed; run-turi.sh 1843 / 0;
run-stdlib-checks.sh 35 / 0; run-flags.sh 86 / 0; tutorial 3 / 0.
