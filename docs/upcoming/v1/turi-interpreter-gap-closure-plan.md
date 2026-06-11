# turi Interpreter Gap-Closure Plan (TI8.b execution detail)

> **Status:** Draft Plan
> **Last Updated:** 2026-06-11
> **Type:** Interpreter / Test Infra
> **Scope:** post-v1 -- the remaining work to flip `tests/run-turi.sh` from a
> curated allowlist to a denylist (every fixture runs under `tur --interpret`
> minus documented carve-outs).
> **Parent:** [turi-parity-post-v1-plan.md](turi-parity-post-v1-plan.md) Phase TI8.b
> **Companion report:** [docs/reported/turi-harness-flip-reconciliation.md](../../reported/turi-harness-flip-reconciliation.md)

---

## Overview

TI8.a (CI ratchet + harness genuinely interprets + honest 145-fixture allowlist)
and the TI8.b `defmodule` concatenation fix have landed. What remains is the
*body* of TI8.b: drive the ~910 interpreter failures to zero -- either by fixing
the interpreter, or by carving the fixture out with a marker and a reason -- so
the allowlist can be deleted and the harness defaulted to "run everything minus
markers."

This plan exists because that work is **multi-session and spans several distinct
root causes** (some are missing-feature gaps, some are semantic divergences, a
few are silent miscompiles). Doing it as one blind flip turns CI red; doing it
without a map risks fixing low-value fixtures while the scary ones (silent
wrong-output) sit unexamined. The plan below sequences the work by leverage and
by severity, and pins a hard fix-vs-carve decision rule so progress is
mechanical, not improvised.

---

## Current state (measured 2026-06-11)

Full probe: run every `tests/fixtures/*` under `tur --interpret`, minus those
already carrying `requires.{compiled,tur-only,dedicated-runner,spices,tsan}`:

```
post-defmodule-fix:  pass = 660   fail = 910   skip = 92
post-W1 (current):   pass = 695   fail = 875   skip = 92   (+35)
```

> The bucket tables below describe the **pre-W1 (910-failure)** snapshot, which
> is still the right map for the remaining work: W1's +35 came out of the
> typed-stdlib-prelude bucket, but that bucket is only partly closed (the
> native-shim-conflicted modules -- result/map/set/hamt -- are still pending in
> W1b). The harness allowlist now stands at **181 passed, 0 failed**.

EX_* expression-kind parity (the `check_turi_parity.py` ratchet):
**109 / 115 handled, 6 carved out, 0 gaps.** So the remaining failures are
*not* about unhandled expression kinds -- they are stdlib/native/semantic gaps.

The 910 failures split cleanly along the single most important axis:

| Class | Count | Disposition |
| --- | ---: | --- |
| **inline-C-bound** (fixture body has a ` ```c ` block) | **377** | carve-out candidates (`requires.tur-only`) -- the interpreter never runs user inline-C (TI7) |
| **pure-turi** (no inline-C in the fixture) | **533** | genuinely fixable interpreter gaps |

The 533 pure-turi failures bucket (by first diagnostic) into:

| Bucket | ~Count | Root cause |
| --- | ---: | --- |
| Missing typed-stdlib prelude | ~93+ | interpreter does not preload `list/map/vec/option/result/...` + typeclass stubs, so `Cons`/`Option`/`map-new`/`Eq`/`Clone`/... are unbound (see W1) |
| Empty-stderr (silent) | ~155 | mix of silent miscompiles (rc=0, wrong stdout), aborts, and errors printed to stdout; **must be triaged individually** (see W4) |
| Move / linearity divergence | ~33 | interpreter runs the affine/linear checker and disagrees with the compiled path (`linear value dropped`, `use-after-move`, `linear used after consume`) |
| `if condition must be bool, got int` | ~5-13 | a predicate native's elaborated return type defaults to `:int` because its module/stub is not loaded (overlaps W1) |
| Kind mismatch (HKT) | ~6-8 | `cannot apply a type of kind ... as a type` under interpret |
| Existential escape | ~4 | `open: existential type variable escapes its scope` |
| Continuation already resumed | ~4 | one-shot continuation accounting differs from compiled |
| Preload self-conflict | ~2 | `defn 'X' already defined by an auto-loaded stdlib module` (e.g. `math.tur`) -- a W1 ordering bug |
| Misc tail | rest | unbound symbol, variadic rest typing, panic allow-list warnings, json natives |

> Counts are approximate (deduped on the first stderr line, ASan warnings
> filtered). Reproduce with the probe script in the reconciliation report; treat
> them as "shape of the work," not contract.

---

## Goal / definition of done

`tests/run-turi.sh` runs **every** fixture under `tur --interpret` (no
allowlist) and is **green**, with:

- `requires.compiled` / `requires.tur-only` the only meaningful skips,
- zero silent miscompiles hidden behind a carve-out without a tracking report,
- `tools/check_turi_parity.py` still passing (no EX_* regressions),
- the full compiled suite (`tests/run.sh`) still green.

### Non-goals

- Performance parity (the interpreter may stay 10-100x slower).
- Running user inline-C under the interpreter (permanent TI7 carve-out).
- Fixing the *compiled* path for any divergence found (file those separately;
  this plan only closes the interpreter side or carves it).

---

## Strategy: the fix-vs-carve decision rule

For each failing fixture, in order:

1. **Inline-C in the fixture body?** -> carve `requires.tur-only` with reason
   `inline-c` (W2). No investigation needed; the interpreter never runs user
   inline-C.
2. **Pure-turi, fails because a stdlib symbol is unbound?** -> belongs to W1
   (the prelude). Do **not** carve; it will be fixed wholesale.
3. **Pure-turi, runs to completion with WRONG stdout (rc=0) or wrong exit?**
   -> this is a **silent miscompile** (W4). Never carve silently: either fix it,
   or carve with `requires.tur-only` reason `interp-miscompile` **and** a
   one-paragraph `docs/reported/` entry. (CLAUDE.md: a test that surfaces real
   broken behavior is a finding, not a malformed test.)
4. **Pure-turi, hard error from a semantic divergence** (move/linearity, kind,
   existential, continuation)? -> W3: fix in `src/turi/eval.c` /
   `src/compiler/*` if tractable; otherwise carve with the specific reason and a
   report.

Carve reasons are recorded in the marker file body (one line), e.g.
`requires.tur-only` containing `inline-c` or `interp-miscompile: see
docs/reported/<slug>.md`. A short reason string keeps later audits cheap.

---

## Workstreams

### W1 -- Typed-stdlib prelude under `--interpret` -- **LANDED (conflict-free subset)**

**Shipped 2026-06-11.** `cmd_eval` now preloads a conflict-free typed-stdlib
subset via the `(load ...)` mechanism: `safe`, the `typeclass-*` stubs, and
`vec`/`slice`/`option`/`pair`/`tuple`/`list`/`grid`/`zipper`. Result:
`Cons`/`Option`/`Pair`/`Tuple` struct types and the `Eq`/`Clone`/`Functor`
classes resolve under `--interpret`. **+35 net fixtures** recovered (probe
660->695 pass), **zero regressions**, harness green at **181 passed**; full
compiled suite unchanged at 1573.

**Excluded on purpose -- the native-shim conflict.** `result`, `map`, `set`,
`hamt`, `mutmap`, and `contract` are **not** preloaded. The interpreter's
`native_*` shims (`native_ok`/`ok-val`/`result-map`, `native_set_count`, the
hamt invalidation guard, `tur-contract-check`) implement a **different
in-memory layout** than the real modules. Loading the module makes the
elaborator bind to the module defn while the runtime native reads the other
layout -- which regressed `coerce-carrier-to-struct` /
`hamt-transient-invalidated` / the `contract-*` fixtures and **heap-overflowed
`native_set_count`** (`src/main.c:5884`, a latent native bug W1 exposed).
`wk_eval_fixture` already skips `contract.tur` for the same reason. The
`ok?`/`err?`/`none?` elaborator stubs are kept (result.tur not loaded); `some?`
comes from the loaded `option.tur`; `vec-get`/`vec-set!`/`vec-free` stubs are
dropped (vec.tur owns them).

**Follow-up -- reconcile the native-shim layer (was the rest of this
workstream).** To recover `typed/list-basic` / `typed/option-basic` /
`typed/map-basic` / `typed/set-basic` / `result-basic` and friends, the
`native_*` Result/Option/Map/Set/Hamt shims must agree on memory layout with the
real modules (then those modules can join the prelude). That is its own task:
audit each `native_*` against its module's struct layout, or register native
overrides for the modules' inline-C functions so the interpreter never executes
the inline-C body. `native_set_count`'s heap overflow should get its own
`docs/reported/` entry. Track as **W1b**.

---

#### Original W1 design (for reference)

**Problem.** `cmd_eval` preloads only `macros.tur` + benchmark stubs; the
compiled path (`compile_to_c`, `src/main.c:646`) auto-loads the full
typeclass-stub + typed-collection set. So `Cons`/`Option`/`Result`,
`Eq`/`Clone`/`Hash`, and `map-new`/`vec-eq?`/`tcons`/... are unbound under the
interpreter. Recovers the `typed/*`, `clone-*`, `make-struct`, several
`typeclass not defined`, and `unknown function` buckets (~93+ pure-turi, plus it
unblocks many of the ~155 empty-stderr that fail *after* a missing symbol).

**Approach (de-risked -- three issues found in the 2026-06-11 spike):**

1. **Load via `(load "...")`, not `turi_eval_file`.** Several prelude modules
   (`safe.tur`, `contract.tur`) carry their own `defmodule`; only the `(load)`
   preprocessing assigns distinct `file_id`s so the per-file `has_defmodule`
   reset fires. (Same root cause as the landed macros fix, scaled up.) Build one
   `(load A)(load B)...` source in dependency order and `turi_eval` it once.
2. **Drop the benchmark stubs the real modules provide.** `cmd_eval` injects
   no-op `vec-get`/`vec-set!`/`hamt-*`/`ok?`/`some?`/... stubs for benchmark
   scripts that do not load stdlib; the real modules then fail to elaborate with
   `defn 'vec-get' already defined by an auto-loaded stdlib module`. Keep only
   the genuinely benchmark-only stubs (`run-ring`/`run-nbody`/`io-*`/
   `random-access-bench`/`cstr->parse-int`/`bit-*`/...) and re-check the
   benchmark fixtures (most self-provide local inline-C stubs per CLAUDE.md).
3. **Cost.** ~24 modules add ~300ms per interpreter invocation (the interpreter
   re-elaborates all accumulated source each `turi_eval`). Acceptable for
   one-shot script runs; for the REPL, consider a cached/precompiled prelude
   (out of scope for the flip, file as a follow-up).

**Reference.** `wk_eval_fixture` (`src/main.c:6671`) already preloads the typed
set via `turi_eval_file` -- reuse its dependency ordering, but switch it to
`(load)` and unify the two prelude sites so they cannot drift.

**Validation.** `typed/list-basic`, `typed/option-basic`, `typed/map-basic`,
`clone-list` pass under `--interpret`; the 145-fixture allowlist stays green;
re-run the probe and confirm the recovered count.

### W2 -- Carve the inline-C set (mechanical, bulk)

**Problem.** 377 failing fixtures contain user inline-C, a permanent TI7
carve-out. They cannot run under the interpreter by design.

**Approach.** Script a sweep: for each failing fixture whose body has a ` ```c `
block, drop a `requires.tur-only` marker containing the single word `inline-c`.
Do this **after W1** (W1 may turn some inline-C fixtures green via native
overrides -- e.g. the gen/stdlib shapes -- so re-measure first and only carve
the ones that still fail). Spot-check that no carved fixture is *also* a silent
miscompile that we are hiding (rc=0 wrong-output inline-C fixtures get the W4
treatment instead).

**Validation.** `check_turi_parity.py` unaffected; `run-turi.sh` skip count
rises by ~the carved count; no fixture both compiles-clean and is carved without
reason.

### W3 -- Pure-turi semantic divergences

These are real interpreter bugs where the tree-walker disagrees with codegen on
a *hard error*. Tackle by sub-bucket; fix where tractable, else carve +
`docs/reported/`:

- **Move / linearity checker (~33).** The interpreter evidently re-runs (or
  diverges on) the affine/linear analysis that the elaborator already performed.
  Investigate whether the interpreter should trust the elaboration (as it does
  for typeclass dispatch -- see TI0) rather than re-deciding. Likely a single
  root cause covering `linear dropped` / `use-after-move` / `linear used after
  consume` / `linear parameter dropped`.
- **Kind mismatch (~6-8)** and **existential escape (~4).** HKT / existential
  elaboration state not threaded into the interpreter the same way. Pair with
  whoever owns `elab` HKT.
- **Continuation already resumed (~4).** One-shot/affine continuation accounting
  in `eval.c` differs from the compiled `cps_prompt` semantics.
- **`if condition must be bool` (~5 pure-turi).** A predicate native's return
  type defaults to `:int` when its module is not loaded; mostly subsumed by W1,
  re-measure after W1.

### W4 -- Silent miscompiles (highest severity, do NOT bulk-carve)

~30 pure-turi fixtures run to completion (rc=0) with **wrong stdout**, plus ~6
that should exit nonzero but return 0. The compile-based harness hid these
entirely; they are the reason the flip matters. Known examples from the
allowlist reconciliation: `result-basic` (returns 0 where 99 expected),
`weak-dangling` (reports a dangling weak as live), `instance-head-hole-pair`
(0/0 vs 42/7). The W1 empty-stderr bucket will surface more once symbols resolve.

**Approach.** Each gets an individual `docs/reported/<slug>.md` with a minimal
repro and observed-vs-expected. Fix in `src/turi/eval.c` where the root cause is
clear; where it is deep, carve `requires.tur-only` reason `interp-miscompile`
**with the report linked** so it is never forgotten. `weak-dangling` (weak-ref
liveness) and `result-basic` (Result value/ordering) are the priority probes --
they hint at value-representation bugs that may explain other empty-stderr
fixtures.

### W5 -- The flip itself

Once W1-W4 leave only carved fixtures failing:

1. Delete `TURI_FIXTURES_DEFAULT` from `tests/run-turi.sh`; default to
   "run every fixture minus `requires.{compiled,tur-only,dedicated-runner,
   spices}`."
2. Retire the `KB-001` allowlist-gap workaround comment.
3. Flip `tests/run-flags.sh`'s three `tur run` assertions (`:345` try-with-basic,
   `:355` try-with-nested, `:408` effect-export-explicit) to `--interpret`.
4. Confirm `run-turi.sh` is green with near-zero non-marker skips, and the full
   compiled suite is unchanged.

---

## Sequencing

```
W1 (prelude, DONE) ──► W1b (native-shim reconcile) ──► W2 (carve inline-C) ──► W5 (flip)
      │                       ▲                              ▲        ▲
      └──► W3 (semantic fixes) ┘                             │        │
      └──► W4 (silent miscompiles) ──────────────────────────┘        │
                                                                       │
                          (re-measure after each before carving) ──────┘
```

W1 landed (the conflict-free subset). **W1b** -- reconcile the `native_*`
Result/Map/Set/Hamt shims with their real modules so those modules can join the
prelude -- is the natural next step and unblocks the largest remaining cluster
(`typed/list`/`option`/`result`/`map`/`set`, `result-basic`). It also closes the
`native_set_count` overflow
([docs/reported/turi-native-set-count-layout-overflow.md](../../reported/turi-native-set-count-layout-overflow.md)).
W2 is the mechanical bulk that makes the flip reachable. W3/W4 run in parallel
and gate the flip. **Re-measure after each step** before carving anything. W5 is
last and is itself the acceptance test.

Suggested PR slicing (each independently green):
1. W1 prelude (+ allowlist additions for newly-passing typed fixtures).
2. W3 move/linearity root-cause fix (likely one fix, ~33 fixtures).
3. W4 batch 1: `weak-dangling` + `result-basic` root cause (may cascade).
4. W2 carve sweep + remaining W4 carves-with-reports.
5. W5 flip + run-flags wiring.

---

## Risks

1. **Prelude perf / REPL latency (W1).** ~300ms/invocation. Mitigate by
   unifying the two prelude sites and, if needed, a follow-up cached prelude.
   Do not let perf block the flip -- the harness has a stamp cache.
2. **Carving hides a real bug (W2/W4).** The decision rule forbids carving a
   silent miscompile without a report. Enforce by scripting the carve to *skip*
   any rc=0-wrong-output fixture and list them for W4.
3. **Prelude changes ripple to currently-green fixtures (W1).** Some passing
   fixtures may rely on the absence of a stdlib symbol or on a benchmark stub.
   Gate every W1 PR on the 145-fixture allowlist staying green + the full
   compiled suite.
4. **Move/linearity fix changes interpreter semantics broadly (W3).** If the
   interpreter stops enforcing linearity, ensure no fixture *relies* on the
   interpreter catching a linearity error (check the `errors/` subtree handling
   noted in TI0).
5. **Scope creep into the compiled path.** Several divergences are the
   interpreter being *stricter* or *wronger* than codegen; resist "fixing" the
   compiler here -- file separately.

---

## Validation / metrics

Track three numbers per PR (all from the probe + harness):

- **probe pass/fail/skip** -- the headline (660/910/92 today; target fail -> 0
  non-carved).
- **`run-turi.sh` summary** -- must stay green every step (145 passed today).
- **`check_turi_parity.py`** -- must stay `0 gaps`.
- **`tests/run.sh`** -- must stay `1573 passed, 0 failed` (count drifts with
  fixture churn).

Definition of done = W5 merged with `run-turi.sh` green at denylist default.

---

## See Also

- [turi-parity-post-v1-plan.md](turi-parity-post-v1-plan.md) -- parent plan
  (TI8.b lives there; this is its execution detail).
- [docs/reported/turi-harness-flip-reconciliation.md](../../reported/turi-harness-flip-reconciliation.md)
  -- the 31 allowlist reconciliation + the probe + the defmodule fix writeup.
- [docs/reported/turi-harness-compiles-instead-of-interpreting.md](../../reported/turi-harness-compiles-instead-of-interpreting.md)
  -- the resolved root blocker.
- [docs/turi-carve-out.txt](../../turi-carve-out.txt) -- EX_* carve-out list the
  ratchet enforces.
- `src/main.c` `cmd_eval` (`:4801`), `compile_to_c` prelude (`:646`),
  `wk_eval_fixture` (`:6671`) -- the three prelude sites to unify in W1.
