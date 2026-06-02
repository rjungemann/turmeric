# Error Handling -- Deferred Features Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-01
> **Type:** Language / Compiler / stdlib / Docs

---

## Overview

The Error Handling Guide ([docs/guides/error-handling-guide.md](../guides/error-handling-guide.md))
ends with a "Deferred" table listing five named features and two behavioural
sections (effect handlers, async tasks) that are flagged as planned-but-not-yet
implemented. This plan finishes those features, sequences them by dependency,
and folds the guide updates into the same phase that ships each piece -- so
the guide never drifts ahead of, or behind, the runtime.

The deferred surface is:

| # | Feature | Guide phase tag | Current state |
|---|---|---|---|
| 1 | `?` query operator | R1 | **Mostly implemented** in compiler (`elab_question` in `src/compiler/elab_forms.c:2292`) + stdlib helpers (`__tur-q-is-err?`, `__tur-q-ok-val` in `stdlib/result.tur:194`). Passing fixtures: `tests/fixtures/result-question-op{,-chain}`. **Undocumented** in the guide. |
| 2 | `catch-unwind` | R2 | **Partial.** Runtime has `tur_panic_payload`, `tur_panic_with`, `EX_CATCH_UNWIND` AST node, interpreter `catch_jmp` setjmp boundary in `src/turi/env.h:154`. Stub fixture `tests/fixtures/panic-catch-unwind` only verifies it compiles. No stdlib surface, no end-to-end test, no docs. |
| 3 | `--no-contracts` flag | C2 | Not started. `stdlib/contract.tur` already calls `contract-enabled?` which is hard-coded to `true`; the flag flips a compiler constant and a stdlib function. |
| 4 | `--warn-unused-result` | R6 | Not started. Requires `ignore!`-aware lint pass over `result`-typed call expressions in statement position. |
| 5 | `--lint-panic` | R6 | Not started. Requires a call-site walker plus a per-source-file allow-list. |
| 6 | Panic in effect handlers / continuations | R6 | Behavioural; depends on (2). Runtime already unwinds defer thunks; needs spec + tests for `reset` boundary clearing the panic state and for continuation-resumption semantics. |
| 7 | Panic in async tasks | R6 (v2) | Behavioural; depends on (2) and on fiber-based async (out of scope of this plan; tracked in `docs/upcoming/cps-transform-plan.md`). This plan only specifies the boundary behaviour and writes synchronous-mode docs. |

---

## Goals / Non-Goals

### Goals

- Land `?` and `catch-unwind` as user-visible, documented features with full
  test coverage and stdlib surface.
- Ship `--no-contracts` as a real release-build optimisation: the contract
  macros expand to nothing when the flag is on.
- Ship `--warn-unused-result` and `--lint-panic` as opt-in `diag_warn` passes
  with single-source-file allow-list comments and CI-friendly diagnostics.
- Specify and test panic behaviour inside effect handlers, continuations, and
  defer chains.
- Rewrite the **Deferred** section of `error-handling-guide.md` after each
  phase so the guide always reflects what actually ships.

### Non-Goals

- Typed exceptions / `try` / `catch` syntax (covered in
  `docs/design/error-handling-rationale.md` §"Typed Exceptions (v2)" -- separate
  v2 roadmap item).
- Fiber-based async runtime (tracked in
  `docs/upcoming/cps-transform-plan.md`). This plan only writes the eventual
  panic-at-task-boundary semantics into the guide as a forward-looking note.
- Replacing `panic`/`abort` with stack traces (out of scope; existing
  `tur_panic` message format is preserved).

---

## Phase R1 -- Document the `?` operator (already implemented)

### Audit before writing

- Confirm `(? expr)` lowers via `__tur-q-is-err?` / `__tur-q-ok-val` at
  `src/compiler/elab_forms.c:2292`.
- Confirm `e->fn_body_depth == 0` guard rejects `?` at the top level.
- Confirm `result-question-op` and `result-question-op-chain` fixtures pass
  under `bash tests/run.sh`.

### Stdlib

- No new functions. The existing helpers `__tur-q-is-err?` and `__tur-q-ok-val`
  are already correctly marked `#{}` (safe).

### Compiler

- Add a sweet-exp form for `?` so it composes with neoteric
  (`?(foo(x))` and `? foo(x)` already parse via the existing reader_macros
  path; add a fixture pinning both spellings).
- Improve the diagnostic when `?` is used on a non-result type. Today it
  silently lowers; we want `TUR-E0xxx: ? operator requires a Result value,
  got <type>` from elaboration after the helper resolves.

### Tests

- `tests/fixtures/result-question-op-typed/` -- typed `Result<int, cstr>`
  through `?`, asserting the err branch carries through.
- `tests/fixtures/result-question-op-bad-type/` -- a `?` on an `:int` value
  to lock in the new diagnostic.
- `tests/fixtures/result-question-op-toplevel/` -- a `?` at the REPL / top
  level to lock in the `fn_body_depth` rejection.

### Guide updates

Add a new `## Query operator: ?` section between `## Result` and `## Option`:

- Syntax: `(? expr)` in both s-exp and sweet-exp; show neoteric `?(expr)`.
- Lowering: `(let [q expr] (if (err? q) (return q) (ok-val q)))`.
- Scope rule: only valid inside a function body whose return type is `result`.
- Worked example: a `parse-config` function that threads three `?` calls.
- Cross-link to `result-or-else` for the "transform-then-propagate" use case.

Move row 1 ("`?` operator") of the **Deferred** table into the new section.

---

## Phase R2 -- `catch-unwind`

### Audit before writing

- Existing scaffolding in `src/runtime/runtime.h:238` (`tur_panic_payload`),
  `src/turi/env.h:154` (`catch_jmp`), `src/compiler/expr.h:192`
  (`EX_CATCH_UNWIND`).
- The compiled path (codegen) currently emits a `Phase R2:
  catch-unwind/catch-panic-of` comment but no real lowering -- confirm in
  any `actual.c` (e.g. `tests/fixtures/panic-catch-unwind/actual.c`).
- Stub fixture `tests/fixtures/panic-catch-unwind/input.tur` only contains
  `(defn main [] :int 0)`; replace with real coverage.

### Runtime

- Finish `tur_panic_with` so it routes through the innermost active
  `catch_jmp` (interpreter) or unwind-table boundary (codegen).
- Implement `tur_panic_payload_downcast(p, target_type)` -- already declared
  in `runtime.h:298`; needs body + a type-tag check against
  `TypeKindInt`.
- Document the double-panic-during-catch case: if a `defer` thunk fires
  inside `catch-unwind` and itself panics, the **inner** panic replaces the
  caught one (matches Rust `catch_unwind` + `Drop` panic semantics).
- Ensure `catch_jmp` is saved/restored across nested `catch-unwind` calls
  (currently a single field; needs a linked-stack push/pop guarded by a
  local `jmp_buf`).

### Compiler

- Codegen for `EX_CATCH_UNWIND`: emit `setjmp` around the thunk call, push
  `catch_jmp` on entry, pop on exit, lower the panic-propagation path to
  populate a `tur_panic_result` struct.
- Reject `catch-unwind` outside a function body (same fn_body_depth guard
  as `?`).
- Refuse to compile `catch-unwind` on the WASM target without
  `-fexceptions` until a Wasm story exists; emit
  `TUR-E0xxx: catch-unwind requires -fexceptions on wasm32`.

### Stdlib

Add `stdlib/panic.tur`:

```turmeric
;;; catch-unwind -- run thunk; return ok(thunk-result) or err(panic-payload).
;;;
;;; Parameters:
;;;   thunk -- (fn [] :int) -- the body to protect
;;;
;;; Returns:
;;;   Result; ok = thunk's return value, err = tur_panic_payload pointer
;;;
;;; Example:
;;;   (let [r (catch-unwind (fn [] (panic "boom")))]
;;;     (if (err? r) (println "caught") (println "ok")))
;;;
;;; Since: Phase R2
(defn catch-unwind [thunk :int] :ptr<void> ...)

;;; catch-panic-of -- like catch-unwind, but only catches panics whose
;;; payload downcasts to the given type tag; re-raises other panic types.
;;;
;;; Parameters:
;;;   type-tag -- TypeKindInt of the expected payload
;;;   thunk    -- (fn [] :int)
;;;
;;; Since: Phase R2
(defn catch-panic-of [type-tag :int thunk :int] :ptr<void> ...)

;;; panic-payload-message -- read the message string out of a caught payload.
;;;
;;; Since: Phase R2
(defn panic-payload-message [p :ptr<void>] :cstr ...)

;;; panic-payload-file -- source file of the panic call site.
(defn panic-payload-file [p :ptr<void>] :cstr ...)

;;; panic-payload-line -- source line of the panic call site.
(defn panic-payload-line [p :ptr<void>] :int ...)
```

### Tests

- `tests/fixtures/panic-catch-unwind-basic/` -- `(catch-unwind (fn [] 42))`
  returns `ok(42)`.
- `tests/fixtures/panic-catch-unwind-caught/` -- catches an explicit
  `(panic "boom")` and asserts message round-trip.
- `tests/fixtures/panic-catch-unwind-defer/` -- defer thunks fire before
  the boundary returns.
- `tests/fixtures/panic-catch-unwind-nested/` -- inner boundary catches,
  outer boundary observes no panic.
- `tests/fixtures/panic-catch-unwind-double/` -- defer panics inside the
  boundary; outer caller sees the inner panic, not the original.
- `tests/fixtures/panic-catch-panic-of/` -- typed payload downcast paths.
- Interpreter equivalent under `tests/run-turi.sh` (the `catch_jmp` field
  in `src/turi/env.h` is interpreter-side).

### Guide updates

Add a new `## Catching a panic at a boundary: catch-unwind` section after
`## panic`. Cover:

- Use cases: FFI boundaries, test harness, supervisor loops -- **not** as a
  substitute for `result`.
- The double-panic-during-catch rule above.
- The interaction with `defer`: defer thunks run before `catch-unwind`
  returns its `err` result.
- Cross-reference `docs/design/error-handling-rationale.md` for the
  exception-vs-panic boundary.

Remove row 2 of the **Deferred** table.

---

## Phase C2 -- `--no-contracts` release flag

### Compiler

- Add `--no-contracts` to the global flags parser (sibling of `--no-data-literals`,
  `--no-auto-spice`).
- Define a single compile-time constant `TUR_CONTRACTS_ENABLED` written into
  the codegen preamble. When the flag is set, the constant is `0`.
- `tur-contract-check` and `tur-contract-check-inv` (`stdlib/contract.tur:45-49`)
  become no-ops when the constant is `0`. The cleanest implementation is to
  expand the macros (`assert!`, `require!`, `ensure!`, `invariant!`) to
  `(do)` rather than to the helper call, so the predicate expression itself
  is never evaluated.
- `contract-enabled?` returns `false` under the flag (today: hard-coded `true`).

### Tests

- `tests/fixtures/contracts-stripped/` -- compile a fixture with `--no-contracts`
  that contains `(assert! (= 1 2))` and verify it runs to completion.
- `tests/fixtures/contracts-stripped-side-effect/` -- the predicate's side
  effects are also stripped (pins the "expand to `(do)`" behaviour).
- Negative: without the flag the same fixture panics with `Assertion failed`.

### Guide updates

Update the "In v1, contracts are always enabled..." paragraph in
`## Contract macros` to describe `--no-contracts`. Remove row 3 of the
**Deferred** table.

### Justfile / build

- Add `release-stripped` recipe: `tur build --no-contracts -O2 ...`.
- Reference it from `docs/guides/compiler-flags-guide.md`.

---

## Phase R6a -- `--warn-unused-result`

### Compiler

- Add a new diagnostic class `TUR-W0xxx: result discarded without ignore!`.
- Walker hooks into the statement-position pass in `src/compiler/elab_forms.c`
  (the same pass that drops `do` block intermediate values).
- A call expression whose static return type is `result<_, _>` and whose
  enclosing parent is a `do`/`progn`/function body (not bound, not the tail
  expression) triggers the warning.
- `ignore!` (already in `stdlib/macros.tur`) silences the warning because it
  expands to `(do expr nil)` -- the wrapping `do` shifts the discarded value
  to a non-result type.
- Opt-in via `--warn-unused-result`; not on by default.

### Tests

- `tests/fixtures/lint-unused-result/` -- expects warning.
- `tests/fixtures/lint-unused-result-ignored/` -- silenced by `ignore!`.
- `tests/fixtures/lint-unused-result-bound/` -- a `let [_ ...]` binding
  does **not** warn (explicit-discard convention).
- `tests/fixtures/lint-unused-result-off/` -- flag not passed; no warning.

### Guide updates

Add a `### Linting unused results` subsection under `## ignore!`. Remove
row 4 of the **Deferred** table.

---

## Phase R6b -- `--lint-panic`

### Compiler

- Add a new diagnostic class `TUR-W0xxx: panic call site outside allow-list`.
- A call to `panic`, `tur_panic`, `assert!`, `require!`, `ensure!`,
  `invariant!`, `result-unwrap`, or `option-unwrap` is a panic site.
- Allow-list source comment: a top-of-file `;; #lint-panic-allow` directive
  silences the warning for the whole file.
- Per-call escape: an immediately-preceding `;; #lint-panic-allow` comment
  silences a single call.
- Off by default; `--lint-panic` opts in.

### Tests

- `tests/fixtures/lint-panic-warn/` -- expects warning on `(panic ...)`.
- `tests/fixtures/lint-panic-file-allow/` -- file-level allow.
- `tests/fixtures/lint-panic-call-allow/` -- per-call allow.
- `tests/fixtures/lint-panic-asserts/` -- contract macros warn.
- `tests/fixtures/lint-panic-off/` -- no flag, no warning.

### Guide updates

Add a `### Auditing panic call sites` subsection at the bottom of
`## panic`. Remove row 5 of the **Deferred** table.

---

## Phase R6c -- Panic in effect handlers / continuations

### Specification

Depends on R2. The semantics already described in the guide
(effect handlers, continuations, defer) become **normative** -- the spec
text moves out of the *Deferred* section into the body of the guide.

The four rules to lock in:

1. **Effect handlers**: a panic inside `(perform ...)` propagates through
   the handler chain and is only intercepted by a `catch-unwind`.
2. **Continuations**: a panic between `shift` and `reset` unwinds to the
   `reset` boundary, which **clears** the panic state. Resuming a captured
   continuation after a panic does not re-panic.
3. **Defer**: defer thunks fire in reverse registration order during
   unwinding. A defer thunk that itself panics triggers the double-panic
   guard (`abort()`).
4. **Cleanup ordering**: defer thunks fire *before* `catch-unwind` populates
   its `err` payload, so a defer thunk can observe panic state via
   `tur_panic_in_progress`.

### Compiler / runtime

- Runtime change: `tur_reset_enter` and `tur_reset_exit`
  (existing entry points in `src/runtime/effects.c`, walk the codebase to
  confirm exact names) must save/restore `tur_panic_in_progress` to
  implement rule 2.
- No new user surface beyond what R2 ships.

### Tests

- `tests/fixtures/panic-in-handler/` -- panic propagates past the handler.
- `tests/fixtures/panic-in-handler-caught/` -- `catch-unwind` wraps a
  handler and catches.
- `tests/fixtures/panic-reset-clears/` -- continuation after `reset`
  proceeds normally after a caught panic.
- `tests/fixtures/panic-defer-double/` -- double-panic guard fires.

### Guide updates

Promote the "Panic inside effect handlers and continuations (Phase R6)"
section out of *Deferred* into the body, immediately after `## panic`'s
existing `### defer during panic` subsection.

---

## Phase R6d -- Async panic semantics (forward-looking note)

Fiber-based async is out of scope (`docs/upcoming/cps-transform-plan.md`),
but the guide should document what *currently* happens and what the v2
target is, in one place.

### Spec text only -- no code

Rewrite the "Panic inside async tasks (Phase R6)" section as:

- A `## Panic inside async tasks` body section.
- A clear "**Today (synchronous async):**" callout that says: "`async`
  inlines the function call; a panic propagates through the caller. There
  is no task boundary."
- A "**v2 (fiber async):**" callout that quotes the four-point list from
  the guide today (panic → rejected future, cancellation precedence,
  uncaught panic in async main, WASM `unreachable`).
- A `> See [docs/upcoming/cps-transform-plan.md](cps-transform-plan.md)`
  cross-link.

Remove the Deferred entry; it lives in the spec now.

---

## Documentation phase -- guide rewrite checklist

The guide updates above land **in each phase**, but a final sweep is needed
to clean up the table and cross-links:

1. The **Deferred** section shrinks to zero rows. Replace the table with a
   short paragraph: "All previously-deferred features have shipped; see the
   table of contents above for the current surface."
2. The guide's opening "It describes what is implemented today" line stays
   accurate.
3. Add a `## See Also` section linking to:
   - `docs/design/error-handling-rationale.md`
   - `docs/guides/effects-system-guide.md`
   - `docs/guides/compiler-flags-guide.md` (for the three new flags)
   - `docs/upcoming/cps-transform-plan.md` (for the v2 async semantics)
4. Regenerate HTML: `python3 tools/genguides.py` (or whichever recipe
   `just docs` invokes for the guides tree -- confirm before running).
5. Refresh `docs/guides/error-handling-guide.html` and check it into the
   same commit as the `.md` changes (the existing guides all carry both
   variants).

---

## Sequencing & dependencies

```
R1 (?)  ──┐
          ├──► R6a (warn-unused-result; benefits from ? being documented)
          │
R2 (catch-unwind) ──► R6c (effect/continuation panic rules)
                  │
                  └─► R6d (async semantics note; spec only)

C2 (--no-contracts)  ── independent
R6b (--lint-panic)   ── independent (but ergonomic after C2 so release builds
                       are not flagged for `assert!` calls)
```

Recommended landing order: **R1 → C2 → R2 → R6c → R6a → R6b → R6d**.

R1 is mostly docs + small compiler tightening, so it ships fastest and
gives `?` to users immediately. C2 is the highest user-visible value for
release builds. R2 is the largest compiler change and gates the two R6
phases that depend on it.

---

## Risks

1. **`catch-unwind` in codegen vs interpreter divergence.** The interpreter
   uses `setjmp`/`longjmp` through `env->catch_jmp`; the C codegen needs a
   compatible scheme. Pinning both with the same fixture set under
   `tests/run.sh` and `tests/run-turi.sh` (per CLAUDE.md harness notes) is
   essential.
2. **WASM target.** `setjmp`/`longjmp` on WASM requires `-fexceptions` or
   asyncify; the guide currently does not call this out for the WASM
   build. The plan errs on the side of refusing `catch-unwind` on WASM
   without `-fexceptions` to keep the failure mode loud.
3. **Contract stripping subtlety.** Side effects inside contract predicates
   silently disappear under `--no-contracts`. This is the documented Rust /
   C `assert` convention but surprises Python/Clojure users. The new tests
   pin the behaviour and the guide calls it out explicitly.
4. **`--lint-panic` allow-list bikeshed.** The chosen `;; #lint-panic-allow`
   comment syntax is the simplest path; if the project grows other
   per-call lint silencers we may want a unified `;; #lint-allow: panic`
   form. Leave a TODO in the implementation; do not block the phase on it.

---

## Open Questions

1. Should `result-unwrap` and `option-unwrap` be deprecated once
   `result-must` / `option-must` exist with consistent panic semantics? The
   current guide already steers callers toward `*-must`. A `--lint-panic`
   warning on `*-unwrap` specifically could be the soft-deprecation lever.
2. `catch-unwind` payload representation: today the runtime stores a
   `tur_panic_payload` pointer in the `err` slot of a `result`. Should the
   stdlib surface expose this directly, or wrap it in a `Panic` opaque type
   to prevent accidental dereferencing? The plan above assumes opaque
   wrapping via `panic-payload-*` accessors.
3. `--warn-unused-result` semantics for the `?` lowering: the lowered
   `(let [__q expr] ...)` consumes `expr`, so the warning should never fire
   on a `?`-wrapped call. Confirm during R6a implementation; add a fixture
   if not.

---

## See Also

- [error-handling-guide.md](../guides/error-handling-guide.md) -- the guide
  this plan updates
- [error-handling-rationale.md](../design/error-handling-rationale.md) --
  exceptions vs. panic design rationale
- [compiler-flags-guide.md](../guides/compiler-flags-guide.md) -- where the
  three new flags will be documented
- [cps-transform-plan.md](cps-transform-plan.md) -- fiber-based async, on
  which the v2 panic-at-task-boundary semantics depend
- [effects-system-guide.md](../guides/effects-system-guide.md) -- effect
  handler semantics referenced by R6c
