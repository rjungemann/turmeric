# 25 inline-C fixtures silently miscompile under `--interpret`

> **RESOLVED + ARCHIVED 2026-06-15.** Re-verified against a fresh Debug build:
> all 25 fixtures run under `tur --interpret` (`ASAN_OPTIONS=detect_leaks=0`)
> yield **0 silent miscompiles** -- 5 PASS (`backtrack-bind`,
> `backtrack-do-macro`, `backtrack-guard`, `backtrack-nested`, `show-float`) and
> 20 clean-error (rc=1: the "inline-C not supported" decline, or a TUR-W0039
> dispatch warning escalated to a hard error). No fixture exits 0 with wrong
> output. The matcher-tightening goal is met; this report is archived.
>
> **Re-audit 2026-06-15: effectively resolved -- the matchers now decline.**
> Classifying all 25 by `--interpret` outcome (stdout vs `expected.stdout` vs
> exit code) on the current tree: **24 of 25 are no longer silent miscompiles.**
> They now either PASS (`backtrack-bind`, `backtrack-do-macro`,
> `backtrack-guard`, `backtrack-nested`, `show-float`) or CLEAN-ERROR with a
> non-zero exit ("inline-C not supported" / a diagnostic) -- the
> refuse-rather-than-guess outcome this report asked for. The cumulative matcher
> tightenings (the `->` decline in `ic_exec_snprintf_fmt:3462`, the
> `ic_exec_accessor` guards, etc.) did the job; an additional snprintf-arg
> hardening landed this session (`ic_format_snprintf_call` now declines when an
> arg expression fails to evaluate, rather than formatting a guessed `0`).
>
> The **one remaining `exit-0 wrong-output`** case was `exg5-rc-in-exists`
> (`0` instead of `99`), and it was **NOT an inline-C-matcher bug** -- the
> inline-C body is never reached. It was a typeclass-dispatch bug (the
> interpreter prelude hijacked the carrier-fallback `__inst_Show_show_T`
> instance method with a float native), reclassified to its own report and
> **FIXED 2026-06-15**:
> [turi-carrier-fallback-instance-method-silent-miscompile.md](turi-carrier-fallback-instance-method-silent-miscompile.md).
> Net: with exg5 fixed and the matcher-tightening goal met, **all 25 are now
> resolved** (PASS, clean-error, or -- for exg5 -- fixed dispatch). This report
> can be archived.

> **Update (TI8.b/W4):** the `ic_exec_accessor` boolean-return guard (see
> [turi-inline-c-accessor-miscompiles-boolean-returns.md](history/turi-inline-c-accessor-miscompiles-boolean-returns.md))
> converted **3** of these (the accessor-path cases incl. `result-basic`) from
> silent-wrong to clean-error. **22 remain**, miscompiling via the *other*
> `ic_exec_*` matchers (constructor / snprintf / switch-string / linked-list /
> simple-return) -- e.g. the `backtrack-*` (7), `show-*` (3), `arrow-instance-*`
> (2), `workstealing-*` (2), `stdlib-*` (2) clusters. Each needs the same
> refuse-rather-than-guess tightening applied to its matcher. They are inline-C
> carve-outs for the flip, so they do not block W5.

**Summary:** The interpreter's best-effort inline-C evaluator
(`try_exec_simple_inline_c` and its `ic_exec_*` pattern matchers,
`src/turi/eval.c`) **claims to handle** the inline-C in these 25 fixtures but
produces **wrong output with a zero exit** -- a silent miscompile, the worst
failure mode. They are inline-C-bound (TI7 carve-outs for the flip, so they will
be skipped under turi), but the underlying evaluator bug is what makes them
dangerous and is tracked here so it is not lost behind the carve.

**Severity:** High (silent miscompile of positive programs), but bounded to the
`--interpret` path; the compiled path is correct.

## The 25

```
arrow-instance-apply              arrow-instance-loop-nonrecursive
backtrack-bind                    backtrack-depth
backtrack-depth-exceeded          backtrack-do-macro
backtrack-guard                   backtrack-interleave
backtrack-nested                  backtrack-once
closure-capture-byptr-struct-param exg5-rc-in-exists
inline-c-cname-splice             instance-head-hole-pair
panic-catch-unwind-caught         result-basic
show-float                        show-list
show-pair                         stdlib-slice-runtime
stdlib-test-runner-registry       weak-dangling
weak-upgrade-option               workstealing-metrics
workstealing-steal
```

Examples (rc=0, wrong stdout):

- `arrow-instance-apply` -> prints raw pointer addresses
  (`88235808160464 ...`) instead of `42 / 42 / 1007`.
- `backtrack-bind` -> prints `0` instead of `2 / 4 / 6`.
- `result-basic` -> predicate inverted (see
  [turi-inline-c-accessor-miscompiles-boolean-returns.md](history/turi-inline-c-accessor-miscompiles-boolean-returns.md)).
- `weak-dangling`, `instance-head-hole-pair` -> the silent miscompiles first
  found during the TI8 allowlist reconciliation.

## Root cause

`try_exec_simple_inline_c` is a loose pattern matcher: it decides a body "looks
like" an accessor / constructor / string-compare and evaluates it with a
mini-evaluator that does not faithfully implement the C the fixture wrote. When
the body's real semantics exceed what the matcher models (negation/disjunction
in a boolean return, struct-by-pointer field math, multi-statement control flow,
`show`/format helpers, backtracking state), it returns a plausible-but-wrong
value instead of declining. `ic_exec_accessor`'s boolean-return bug is one
concrete instance.

## Fix direction (W4)

Make the evaluator **conservative**: each `ic_exec_*` matcher must return
`turi_nil()` (-> clean "inline-C not supported" error, which then carves cleanly)
for any shape it cannot evaluate with confidence, rather than guessing. Concretely:

- Tighten the pattern guards so a matcher only claims a body whose every token it
  models (reject `!`, `||`, `&&`, `==`/`!=`, pointer arithmetic, `>1` statement,
  `printf`/`snprintf` mixed with returns, etc. unless explicitly supported).
- Re-run these 25 + the existing allowlisted inline-C fixtures (`inline-c-binop`,
  `gen-*`) after tightening: the 25 should flip to a clean error (then they carve
  as ordinary inline-C), and the working ones must stay correct (no regression).

This is delicate -- the same matchers correctly handle the allowlisted inline-C
fixtures, so the tightening must be precise. It is **W4** work, independent of
the W2 carve.

## Interaction with W2 (the carve)

For the allowlist->denylist flip these 25 are skipped as inline-C carve-outs
(they contain a ` ```c ` block). That is correct for the flip, but the evaluator
bug above still ships for any *non-fixture* program that hits the same shapes
under `--interpret`/`tur repl`. So the carve does not close this; the W4 fix
does.

## Status

Filed while executing TI8.b/W2. The carve mechanism skips these under turi; this
report keeps the silent-miscompile evaluator bug visible for W4. See
[docs/archive/turi-interpreter-gap-closure-plan.md](turi-interpreter-gap-closure-plan.md).
