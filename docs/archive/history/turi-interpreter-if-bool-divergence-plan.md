---
title: Turi interpreter if-bool type divergence -- Plan
category: Planning
description: Fix the two remaining `if condition must be bool, got int` divergences under `tur --interpret` (contract-release, result-question-op-sweet), where the shared elaborator types a predicate's return as int under interpretation but bool when compiled. Untyped native overrides and the `?`-operator's ok-ness carrier are the root causes.
---

# Turi interpreter `if`-bool type divergence -- Plan

> **RESOLVED 2026-06-14 -- archived.** The divergence is gone:
> `contract-release` now PASSES under `--interpret` (prints `contracts-enabled`,
> rc=0) because `contract.tur` is loaded up front in the `cmd_eval`/`--interpret`
> path ([src/main.c](../../../src/main.c), the `contract.tur` preload at the
> top of `cmd_eval`, *before* the typed-stdlib prelude array), so its
> `(defn contract-enabled? [] :bool true)` types the `if` condition `:bool`
> before the elaborator's bool check runs. The native override at
> `turi_env_register_native(env, "contract-enabled?", ...)` supplies the runtime
> impl (also `bool`), so there is no int/bool mismatch. Its `requires.tur-only`
> marker has been removed; it runs genuinely interpreted under the post-W5
> denylist harness. `result-question-op-sweet` remains carved, but for the
> *correct* reason: its `u-ok`/`u-ok-val` helpers are genuine inline-C
> (`malloc` + struct deref) the tree-walker cannot run, so it auto-skips via the
> inline-C carve (its redundant `requires.tur-only` was removed too -- it was the
> only inline-C fixture carrying one). The `?`-operator desugar's int-carrier
> `if` would only resurface *if* those helpers were ever reimplemented as natives;
> that is a hypothetical, not a live blocker. Neither root cause from the plan
> below required a code change -- the prelude graduation of `contract.tur` (an
> independent landing) closed #1, and the inline-C carve covers #2.

## Status and scope

Two fixtures remain carved `requires.tur-only` with reason "if-bool type-check
divergence": `contract-release` and `result-question-op-sweet`. Both elaborate
fine on the compiled path but, under `tur --interpret`, fail at elaboration with

```
if condition must be bool, got int
```

This is **not** a separate interpreter type checker -- the diagnostic is raised
by the *shared* elaborator at
[src/compiler/elab_forms.c:1762](../../../src/compiler/elab_forms.c)
(`if (!type_eq(cond->type, TYPE_BOOL))`). The divergence is therefore entirely
about what *type* the condition expression resolves to before that check runs,
and that differs between the compiled and interpreted preludes. Fixing it lets
both fixtures drop their markers and join the `run-turi.sh` denylist default
(post-W5) as genuinely-interpreted coverage.

Severity: **low / correctness-smell**. It is not a miscompile (the program never
runs -- it is rejected at elaboration), and it affects only 2 fixtures today.
But it is a real interpreter-vs-compiler divergence: the interpreter is
*stricter* than the compiler for the same source, which is the wrong direction
and a latent ergonomics hazard for any user predicate that resolves to a native
under `--interpret`.

## Root cause, per fixture

### 1. `contract-release` -- untyped native override shadows a `:bool` defn

`stdlib/contract.tur:44` declares the predicate with an explicit bool return:

```turmeric
(defn contract-enabled? [] : bool true)
```

On the compiled path the elaborator sees that `:bool` annotation, so
`(if (contract-enabled?) ...)` type-checks. Under `--interpret`, `cmd_eval`
registers a **native** override
([src/main.c:5127](../../../src/main.c)):

```c
turi_env_register_native(env, "contract-enabled?", native_contract_enabled, NULL);
```

`turi_env_register_native`
([src/turi/eval.h:99](../../../src/turi/eval.h)) carries **no type signature** --
natives are untyped, so a bare call to a native binding resolves its return type
to the default (`:int`). That `:int` shadows the pure-turi defn's `:bool`, so the
condition is typed int and the `if` is rejected.

Note the override is also **redundant**: `contract-enabled?`'s body is the
pure-turi literal `true`, which the tree-walker can evaluate directly -- there is
no inline-C to shim here (unlike `tur-contract-check`, which calls `tur_panic`).

### 2. `result-question-op-sweet` -- the `?` operator desugars to an int-carrier `if`

The `?` postfix operator desugars (in elaboration) to an `if` that branches on
the Result's ok-ness. Under `--interpret` that ok-ness is read off the int64
carrier and typed `:int`, so the desugared `if` trips the same check. This
fixture *additionally* uses inline-C `u-ok` / `u-ok-val` helpers the interpreter
does not run, so even after the type fix it needs native shims (or a pure-turi
rewrite of those helpers) to actually produce `42/22`. The compiled path prints
`42/22`.

## Proposed fix directions

Sequenced cheapest-first; re-measure after each.

1. **Drop the redundant `contract-enabled?` native** (fixes #1 alone).
   `contract.tur` is already preloaded under `--interpret`, and its
   `(defn contract-enabled? [] :bool true)` is interpretable as-is. Removing the
   `turi_env_register_native(env, "contract-enabled?", ...)` line lets the
   `:bool`-typed defn stand, so the elaborator types the condition bool. Verify
   no other path depends on the native (the always-true worker-mode variant at
   `src/main.c:10137` is a separate registration site -- check both).
   - Risk: if some flag-stripping mode *replaces* the defn body, the native may
     have been the only definition in that mode. Confirm `contract-release` is
     the only consumer and that the defn is always present under `--interpret`.

2. **Give native predicates a bool return type** (general fix, supports #2 and
   future cases). Either:
   - add a typed registration entrypoint (e.g. `turi_env_register_native_typed`
     carrying a `TypeKind` / full `Type` for the return), and register boolean
     natives (`contract-enabled?`, ok-ness predicates, `is?`-style tests) with
     `:bool`; or
   - teach the elaborator that a native call head whose name is a known
     bool-returning predicate yields `:bool`.
   The typed-registration route is cleaner and removes the "natives default to
   int" foot-gun globally.

3. **Type the `?`-operator desugar's condition as a bool test** (fixes #2's type
   half). The desugar should branch on a bool ok-ness *predicate*, not the raw
   int carrier -- mirror however the compiled path keeps this bool. Then provide
   native (or pure-turi) `u-ok` / `u-ok-val` so the fixture runs end-to-end.

A minimal landing is **step 1** (unblocks `contract-release`); steps 2-3 are the
principled fix that also unblocks `result-question-op-sweet` and prevents
recurrence.

## Validation

- After each step, run the affected fixture directly:
  `ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/contract-release/input.tur`
  (expect `contracts-enabled`, rc=0), and likewise
  `result-question-op-sweet` (expect `42/22`).
- Remove the `requires.tur-only` marker from each unblocked fixture; it then
  runs automatically under the post-W5 denylist harness.
- `bash tests/run-turi.sh` stays green (pass count rises by the number
  unblocked); `bash tests/run.sh` unchanged (1615 passed); `tools/check_turi_parity.py` 0-gaps.
- Guard against the interpreter-stricter regression class: add a tiny positive
  fixture exercising a bool-returning native in `if` position so the divergence
  cannot silently return.

## See also

- [docs/archive/history/turi-open-reports-prereqs.md](turi-open-reports-prereqs.md)
  -- Prereq 3d, which carved these two with `requires.tur-only`.
- [docs/archive/history/turi-harness-flip-reconciliation.md](turi-harness-flip-reconciliation.md)
  -- the if-bool bucket of the original blast-radius measurement.
- [docs/archive/history/turi-interpret-flip-residual-plan.md](turi-interpret-flip-residual-plan.md)
  -- W5 flip (landed); these fixtures stay carved until this plan lands.
