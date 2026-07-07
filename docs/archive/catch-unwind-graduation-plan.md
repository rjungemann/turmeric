---
title: Stackless catch-unwind -- sign-off probes and graduation close-out -- Plan
category: Planning
description: The remaining close-out for the stackless catch-unwind / panic-return-signal experiments after the general lowering landed -- the D4 sign-off probes (cu-rec, cu-catch-deep, atom-rec, fiber-rec, plus a cross-function/mutual-recursion probe) at 1,000,000 flat, a non-catch hot-path neutrality measurement, and the mechanics of graduating both flags to always-on.
---

# Stackless catch-unwind -- sign-off probes and graduation close-out -- Plan

## Why this exists

The general lowering
([archived plan](./compiled-catch-unwind-general-lowering-plan.md),
G1-G7) is complete: the scaffold is retired, the general splitter handles
arbitrary catch-crossing bodies, cross-function/mutual recursion, value
extraction + live err branch, int64/opaque/by-value-aggregate params, and a
native-matching panic precedence with effects/fibers/cancel. What remains before
`stackless-catch-unwind` and `panic-return-signal` can **graduate** (flags
deleted, behavior always-on) is the plan's own graduation gate:

> **Graduation:** G1-G7 complete AND measured neutral-or-better on the non-catch
> hot path across the suite. Until then both experiments stay prototypes behind
> their gates; `expires_at` 0.31.0.

plus the **D4 sign-off** carried from the parent
[compiled-catch-unwind-stackless-plan.md](./compiled-catch-unwind-stackless-plan.md):

> `cu-rec`, `cu-catch-deep`, `atom-rec`, `fiber-rec` at 1,000,000 in the compiled
> backend with no SIGSEGV, matching the interpreter's `eval-tco` probes.

This plan collects the probe run and the graduation mechanics.

## Part A -- sign-off probes

Run each probe **compiled** (`--enable=stackless-catch-unwind`) at 1,000,000
depth, under a reduced stack `ulimit` (e.g. `ulimit -s 256`), and confirm no
SIGSEGV and a value matching native at a small depth (native is the oracle; at
1,000,000 native SIGSEGVs for these shapes, which is the point).

Probe shapes (interpreter side lives in `tests/turi/eval-tco.tur`; compiled
counterparts exist as `tests/fixtures/stackless-catch-unwind-*`):

1. **`cu-rec`** -- nested `catch-unwind`, no panic, non-tail after
   (`stackless-catch-unwind-deep`). Two segments (entry, after) per level.
2. **`cu-catch-deep`** -- a single top-level catch over a deep non-tail body
   (the panic-free depth-wall shape). Add a fixture if not covered.
3. **`atom-rec`** -- a recursion carrying an atom/handle (int64-carrier) param,
   crossing `catch-unwind` (covered in spirit by
   `stackless-catch-unwind-opaque`; add an atom-specific probe if the parent
   plan's `atom-rec` differs).
4. **`fiber-rec`** -- a trampolined deep `catch-unwind` run inside an `async`
   fiber (G7 verified this interoperates; scale it to 1,000,000).
5. **`mutual-rec` (new, G4)** -- ping/pong / a 3-cycle at 1,000,000
   (`stackless-catch-unwind-mutual*` at small depth today; scale the probe).

Deliverable: a short probe script (or a `requires`-tagged, longer-timeout
fixture set) that runs all five at 1,000,000 under a reduced stack and asserts
exit 0 + expected value. Record the results table in this plan when run. Keep the
1,000,000-depth runs OUT of the default `tests/run.sh` fast suite (they are slow
/ memory-heavy) -- gate behind an env flag or a dedicated ctest target, mirroring
how the parent plan treats the deep probes.

## Part B -- non-catch hot-path neutrality

Graduation requires the always-on lowering be **neutral-or-better on the
non-catch hot path** -- i.e. enabling the (soon-mandatory) `panic-return-signal`
A-normalization + the eligibility machinery must not regress ordinary,
non-catch-unwind code.

- Confirm flag-off codegen is byte-identical for every non-catch fixture (it is,
  by construction: `stackless_general_eligible` requires a reachable
  `catch-unwind`, and `panic-return-signal` off is a transparent pass-through).
  Re-assert with a full snapshot diff.
- Measure `bash tests/run.sh` wall-clock and a representative
  `benchmarks/` run with the flags ON vs OFF; require neutral-or-better on the
  non-catch benches. Note any per-call-site A-normalization overhead that
  `panic-return-signal` imposes once it is always-on.

## Part C -- graduation mechanics

When Parts A + B pass (and the remaining aggregate follow-on plans are either
landed or explicitly scoped out of graduation):

1. Delete the two rows from `EXPERIMENTS[]` in
   `src/runtime/experiments.c` (`panic-return-signal`, `stackless-catch-unwind`)
   and their `g_opt_*` globals; make the feature always-on (the elaboration /
   `emit_fns.c` eligibility no longer gated by `g_opt_stackless_catch_unwind`,
   the panic path no longer gated by `g_opt_panic_return_signal`).
2. Drop the `experiment_warn_if_used` calls (TUR-W0060/W0061) and the
   `stackless-catch-unwind` -> `panic-return-signal` implication.
3. Regenerate the `stackless-catch-unwind-*` fixtures without the `flags` file
   (they become always-on), or fold them into the default emission fixtures.
4. Update the release-cut skills' `expires_at` bookkeeping: both currently
   `0.31.0` -- graduation removes the rows before that deadline, satisfying the
   hard `expires_at` contract without a bump.
5. Archive the parent
   [compiled-catch-unwind-stackless-plan.md](./compiled-catch-unwind-stackless-plan.md)
   and this plan alongside the general lowering.

## Sequencing vs the aggregate follow-on plans

Graduation does not strictly depend on the deferred aggregate work
(aggregate returns; by-const-ptr params; aggregate+group; the panic-unwind box
leak) -- those are *widenings* of what trampolines, and any un-widened case
falls back to native (correct, just not flat). Decide at graduation time whether
to block on them or graduate the always-on core and keep the widenings as
post-graduation improvements. Recommendation: **graduate the core** once A+B
pass; land the aggregate widenings independently.

## Validation

- All five probes green at 1,000,000 flat.
- Full suite green; non-catch benches neutral-or-better ON vs OFF.
- Post-graduation: suite green with the flags removed; no `--enable=` needed.

## Close-out (executed 2026-07-07)

**Part A -- sign-off probes.** The five probe inputs live in
`tests/probes/stackless-signoff/`; `tests/stackless-signoff-probes.sh` (dedicated
ctest target `stackless_signoff_probes`, kept OUT of `tests/run.sh`) builds and
runs each at 1,000,000 depth under `ulimit -s 256`. All five green; each matches
the native oracle at depth 1000.

| Probe | Shape | Value @ 1e6 (256KB stack) | Native @ 1e6 (256KB) |
| --- | --- | --- | --- |
| `cu-rec` | nested catch-unwind, non-tail after | 1000000 (exit 0) | SIGSEGV |
| `cu-catch-deep` | single top-level catch over deep panicking body | 1000000 (exit 0) | survives (C compiler loop-optimizes the single-catch body) |
| `atom-rec` | `defopaque` int64-carrier param crossing catch-unwind | 1000000 (exit 0) | SIGSEGV |
| `mutual-rec` | ping/pong cross-function group driver | 1000001 (exit 0) | SIGSEGV |
| `fiber-rec` | deep catch-unwind inside an `async` fiber | 1000000 (exit 0) | SIGSEGV |

**Part B -- non-catch hot-path neutrality.** Enabling the signal transport
A-normalizes every panic-capable call site (hoist into an `__auto_type` temp +
`if (tur_panicking) return {0};`).  Correctness makes this unconditional: signal
transport is a whole-program property (any function may be called under a
catch-unwind in another TU), so the per-call check cannot be safely gated on the
current module.  Measured neutral on non-catch benches (`tur build`, gcc):

| Bench | OFF | ON | Verdict |
| --- | --- | --- | --- |
| collatz sum 1..3e6 (data-dependent, call-dense, ~429M steps) | 0.701s | 0.701s | neutral |
| `bench-parsec-json` | 0.008s | 0.008s | neutral |

The always-not-taken thread-local branch is absorbed by the predictor (and often
eliminated by the C optimizer in single-TU catch-free programs).

**Part C -- graduation mechanics.** Both rows removed from `EXPERIMENTS[]`
(`src/runtime/experiments.c`) along with the `g_opt_*` globals, the
`experiment_warn_if_used` calls, and the stackless->panic-return-signal
implication.  The gates in `emit_expr.c` / `emit_fns.c` / `emit_module.c` are
unconditional; the setjmp/longjmp catch-helper variants are deleted.  The 21
`stackless-catch-unwind-*` / `panic-catch-unwind-signal` fixture `flags` files
are removed (feature is always-on) and all 129 `expected.c` snapshots regenerated
(109 changed).  Full suite green: **1975 passed, 0 failed.**  `expires_at` 0.31.0
is satisfied without a bump (rows deleted before the deadline; `tur experiments
--json` now returns `[]`).  A void/never-typed call is not hoisted (it emits as C
`void`); this guard was widened from `TY_NIL` to also cover `TY_NEVER`.

The deferred aggregate widenings (BR-series, aggregate returns, the composed
by-value van Laarhoven follow-on) are post-graduation improvements per the
"Sequencing" note above and do not block this close-out.
