# 25 filed inline-C fixtures (20 still silently miscompile) under `--interpret`

> **RESOLVED (W4, 2026-06-12).** All 20 remaining silent miscompiles now flip to
> a clean `rc=1` "inline-C not supported" error -- no more rc=0 wrong answers.
> The `ic_exec_*` matchers (`src/turi/eval.c`) were tightened to
> refuse-rather-than-guess on any shape they cannot evaluate faithfully:
>
> - **`ic_exec_constructor`** (11: the `backtrack-*` cluster, `arrow-instance-
>   loop-nonrecursive`, `workstealing-*`) -- declines a body with a loop
>   (`while`/`for`), a second allocation, `__atomic`/`TUR_APPLY`, or that chases a
>   pointer other than the alloc target (`r->e1 = s->e1 + 100`). A single early
>   `return 0;` OOM guard is still allowed, so flat stdlib constructors
>   (`future-cell-new`, `fs.tur` mkstemp) keep working.
> - **`ic_exec_snprintf_fmt`** (4: `show-float/-list/-pair`, `exg5-rc-in-exists`)
>   -- declines float conversions (`%g`/`%f`/... -- the formatter passes every arg
>   as `(long long)`), loops, pointer-chasing args (`->`), and concatenation
>   (`snprintf(buf + off, ...)`). The guarded if/else `snprintf` pair
>   (`range-bound-show-ord`) is still resolved by `ic_snprintf_cond_branch`.
> - **`ic_exec_accessor`** (2: `arrow-instance-apply` via `TUR_APPLY`,
>   `panic-catch-unwind-caught` + the `ls-get` half of `stdlib-slice-runtime`)
>   -- declines function application (`TUR_APPLY`), `fprintf`, and `if (...)
>   return ...;` early-return branching (`>1` return).
> - **`ic_exec` simple-return** (2: `closure-capture-byptr-struct-param`,
>   `inline-c-cname-splice`) -- declines `printf` side effects, `__TUR_CNAME_`
>   sibling-call splices, and function-pointer calls (`)(`).
>
> Validated: each of the 20 flips to a clean error; the correctly-claimed
> regression sets (20 constructor / 2 snprintf / 5 accessor / 4 simple-return
> allowlisted fixtures) all still pass; full interpreter harness 983/0, compiled
> suite 1599/0. The fixtures now carve cleanly as ordinary inline-C under the
> flip. The clean error itself was also made actionable (prereq 2c): it points
> the user at `tur build`/`tur run`.

> **Update (2026-06-12, claim-trace + recount):** `TUR_IC_TRACE=1` now logs
> which `ic_exec_*` matcher claims each body (`ic_claim`, `src/turi/eval.c`) --
> diagnostic groundwork for the tightening. Re-measured against it, **the true
> remaining silent-miscompile set is 20, not 22**: `instance-head-hole-pair`,
> `stdlib-test-runner-registry`, `weak-dangling`, and `weak-upgrade-option` now
> produce a **clean error (rc=1)**, not a silent wrong answer -- the matcher
> already declines them. The 20 funnel through just two matchers:
> **`constructor` (12: the `backtrack-*` cluster + `arrow-instance-*` +
> `workstealing-*`)** and **`snprintf` (4: `show-float/-list/-pair`,
> `exg5-rc-in-exists`)**, plus `accessor`/`simple-return`/mixed (4). Tightening
> `ic_exec_constructor` first is the highest-leverage slice. Full matcher->fixture
> map and the prereq decomposition:
> [docs/archive/history/turi-open-reports-prereqs.md](turi-open-reports-prereqs.md).
>
> **Update (2026-06-12, conditional-snprintf):** the `ic_exec_snprintf_fmt`
> matcher no longer blindly takes the *first* `snprintf` when two are guarded by
> an `if (COND) snprintf(...); else snprintf(...);`. It now evaluates `COND` and
> formats only the live branch (`ic_snprintf_cond_branch` /
> `ic_format_snprintf_call`, `src/turi/eval.c`). This closed the stdlib
> `bound-fmt` divergence behind `range-bound-show-ord` (tracked in
> [turi-pure-turi-silent-miscompiles.md](../archive/turi-pure-turi-silent-miscompiles.md));
> resolution archived at
> [../archive/turi-inline-c-conditional-snprintf-branch.md](../archive/turi-inline-c-conditional-snprintf-branch.md).
> The other snprintf-bucket cases below still miscompile via *different* paths
> (e.g. `%s`/string args, multi-statement bodies) and remain open.
>
> **Update (TI8.b/W4):** the `ic_exec_accessor` boolean-return guard (see
> [turi-inline-c-accessor-miscompiles-boolean-returns.md](../archive/history/turi-inline-c-accessor-miscompiles-boolean-returns.md))
> converted **3** of these (the accessor-path cases incl. `result-basic`) from
> silent-wrong to clean-error. ~~**22 remain**~~ -- *superseded: the claim-trace
> recount above lands the true figure at **20** (the `22` predates the four
> additional matcher-declines listed in the recount update).* The 20 miscompile
> via the *other* `ic_exec_*` matchers (constructor / snprintf / accessor /
> simple-return / mixed); the per-matcher map is tabulated below. Each needs the
> same refuse-rather-than-guess tightening applied to its matcher. They are
> inline-C carve-outs for the flip, so they do not block W5.

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

### Matcher -> fixture map for the 20 still-silent miscompiles

Re-measured 2026-06-12 via `TUR_IC_TRACE=1 ./build/tur --interpret <fixture>`
(rc + stdout-vs-`expected.stdout` classification). Five of the 25 now produce a
**clean error (rc=1)** -- `instance-head-hole-pair`, `result-basic`,
`stdlib-test-runner-registry`, `weak-dangling`, `weak-upgrade-option` -- the
matcher already declines them, so they are no longer silent miscompiles. The
remaining **20** (rc=0, wrong stdout) funnel through these matchers:

| Matcher (`ic_exec_*`) | Count | Fixtures |
| --- | --- | --- |
| `constructor` | 12 | `backtrack-{bind,depth,depth-exceeded,do-macro,guard,interleave,nested,once}` (8), `arrow-instance-apply`, `arrow-instance-loop-nonrecursive`, `workstealing-metrics`, `workstealing-steal` |
| `snprintf` | 4 | `show-float`, `show-list`, `show-pair`, `exg5-rc-in-exists` |
| `accessor` | 2 | `panic-catch-unwind-caught`; `arrow-instance-apply` also hits accessor |
| `simple-return` | 2 | `closure-capture-byptr-struct-param`, `inline-c-cname-splice` |
| mixed (free+constructor+accessor) | 1 | `stdlib-slice-runtime` |

(The `backtrack-*` cluster also fires `linked-list-print` once each -- a
secondary claim on a printf helper -- but the wrong *answer* comes from the
`constructor` claims.) **Two matchers cover 16 of 20**: tightening
`ic_exec_constructor` (12) then `ic_exec_snprintf_fmt` (4) is the
highest-leverage first slice. Full prereq decomposition:
[../upcoming/v1/turi-open-reports-prereqs.md](turi-open-reports-prereqs.md).

Examples (rc=0, wrong stdout):

- `arrow-instance-apply` -> prints raw pointer addresses
  (`88235808160464 ...`) instead of `42 / 42 / 1007`.
- `backtrack-bind` -> prints `0` instead of `2 / 4 / 6`.
- `result-basic` -> predicate inverted (see
  [turi-inline-c-accessor-miscompiles-boolean-returns.md](../archive/history/turi-inline-c-accessor-miscompiles-boolean-returns.md)).
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
[docs/upcoming/turi-interpreter-gap-closure-plan.md](../upcoming/turi-interpreter-gap-closure-plan.md).
