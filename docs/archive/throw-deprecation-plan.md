---
title: Throw / Try-Catch Deprecation Plan
category: Planning
description: Retire the Phase S4 `throw` / `try` / `catch` forms in favor of `result<T,E>` + `panic`. Two-step retirement -- warning + migration first, deletion of elab/emit/runtime paths second -- so a parallel error channel stops taxing every new pass (borrow, sized, session, effects).
---

# `throw` / `try` / `catch` Deprecation -- Plan

> **Status: Shipped (2026-06-23).** All five steps landed in v0.24.x:
>
> - DEPR-W0 (warning TUR-D0002) -- shipped in 8bda2676 (v0.24.3)
> - DEPR-M0 (migrate `assert-throws!`) -- same commit
> - DEPR-F0 (park the legacy fixture as `_legacy-throw/`) -- same commit
> - DEPR-R0 (interpreter fiber rejection on `TURI_REJECTION` values
>   instead of throws) -- bfbaddec
> - DEPR-D0 (delete the `throw` / `try` / `catch` front end, the legacy
>   fixtures, `turi_native_throw` / `make_throw_val`, and TUR-D0002) --
>   the commit alongside this archival.
>
> Runtime helpers `tur_panic_with` / `tur_catch_unwind` and the
> `setjmp`/`longjmp` plumbing in `emit_module.c` remain in place --
> they are load-bearing for `panic` / `catch-unwind`, which continue
> to ship. `env->throwing` / `env->throw_value` stay as never-set
> scratch slots threaded through the interpreter's signal-propagation
> machinery; no path produces a TURI_THROW value after D0.

## Context

Turmeric inherited a Phase S4 exception channel:

- **Parser / elab.** `throw` and `try` are interned in
  `src/compiler/elab_core.c:1538-1539` and dispatched in
  `src/compiler/elab_call.c:1561-1562` (`elab_throw`, `elab_try_catch`).
- **Codegen.** `src/compiler/emit_expr.c:2027` lowers `try`/`throw`;
  `src/compiler/emit_module.c` emits a global `setjmp`/`longjmp` handler
  (`emit_module.c:5567`, `:5657`, `:5680`, `:5720`, `:5738`) and a
  "global handler first, then fiber" precedence comment at `:5926`.
- **Borrow / passes.** `src/passes/borrow_check.c:714` has Phase S4 cases
  for `throw` / `try-catch`.
- **Runtime.** `src/turi/eval.c:6558` handles `throw`/`try-catch` for the
  interpreter; `src/turi/fiber.c:361` *re-throws* a rejected fiber result
  so `try`/`catch` can intercept it, and `fiber.c:552` throws `"timeout"`
  when a timed task is cancelled.
- **Runtime helpers.** ~15 references to `turi_native_throw` /
  `tur_catch_unwind` / `tur_throw` across `src/`.

### Preliminary research findings (2026-06-23)

Two claims in the earlier draft were either wrong or only half right. The
code points are worth nailing down before DEPR-R0 / DEPR-D0 design choices
ride on them.

**Claim 1 -- "async machinery currently relies on throw as the
cancellation/rejection signal."** *Half right -- interpreter only.*

- **Compiled (`tur build` / `emit-c`) await does NOT throw.**
  `tur_await_future` (`src/compiler/emit_module.c:6719-6756`) handles a
  rejected `FUTURE_REJECTED` by printing
  `await: future rejected: <error>` to stderr and calling `abort()`
  on all three branches (already-done, scheduler-drained, post-resume).
  There is no `tur_panic_with` / `tur_throw` on the rejection path,
  and no `(try ...)` form can catch a rejected await today --
  catching one in compiled code is *already* impossible. The compiled
  `tur_future_reject` helper at `:6687` just sets the status enum and
  stashes the error string.
- **Compiled `with-timeout` and `task-cancel` don't exist as compiled
  natives.** The runtime emit at `emit_module.c:6655+` only ships
  `tur_future_new` / `_fulfill` / `_reject` / `_done` / `_get` /
  `tur_await_future` / `tur_future_free`. There is no compiled
  timeout or cancel helper to migrate. (The interpreter has them; see
  below.)
- **Interpreter (`turi`) await DOES throw.**
  `turi_await_future` (`src/turi/fiber.c:314-373`) takes two paths:
  - `f->state == TURI_FUTURE_REJECTED` *at entry* returns the error as
    a `TuriValue` of `TURI_ERROR` tag (no throw) -- `fiber.c:317-321`.
  - A future that becomes rejected *during* the event loop falls into
    `fiber.c:360-372` and calls `turi_native_throw` (or stashes
    `env->throw_value` directly when the result is already
    `TURI_THROW`-tagged).
- **Interpreter `with-timeout` throws on timeout.** `native_with_timeout`
  at `src/turi/fiber.c:510-556` ends with
  `turi_task_cancel(env, task); turi_native_throw(env, "timeout");`
  when the timer fires first. `turi_task_cancel` itself (`:299-304`)
  rejects the future via `turi_future_reject` -- *not* a throw -- so
  the throw is purely the timeout signal, not the cancellation signal.
- **Net.** Async-to-throw coupling is an interpreter implementation
  detail, not a language-level guarantee. The compiled path already
  reports rejection via `abort()`. So DEPR-D0 on the *compiled* side
  does not need a fiber-rejection refactor at all; DEPR-R0 is purely
  interpreter cleanup.

**Claim 2 -- "fiber rejection has a non-throw path."** *True in both
paths, but for different reasons, and neither is `result`-shaped today.*

- **Compiled:** the non-throw path is already there -- it's `abort()`.
  No language work needed to make compiled await stop throwing; it
  never did. The remaining design question is whether to *change* it
  to return `result<T, FiberError>`, which is a separate improvement
  from `throw` deletion (and not required to land DEPR-D0). The
  internal hooks already exist: `tur_future_reject` records an error
  string; promoting that to a typed `result` is a localized edit at
  `emit_module.c:6719` (compiled await) and its single caller in
  `emit_expr.c:4424-4428` (the `(await fut)` lowering).
- **Interpreter:** the non-throw path also exists -- the
  already-rejected branch at `fiber.c:317-321` returns
  `TURI_ERROR`-tagged values directly. The throw path at
  `fiber.c:360-372` is doing the *same job* (carry a rejection back to
  the awaiter) using a different mechanism, presumably because the
  in-event-loop rejection happens after a `swapcontext`, where simply
  `return`-ing a `TURI_ERROR` from the C frame would skip user code
  that wanted to observe it. The fix is to set
  `env->throwing = false; env->throw_value = (TURI_ERROR)` and let
  the caller observe via the same path the entry-rejection branch
  already uses -- *not* to invent a new error channel.

**Compiled `throw` / `try` are syntactic sugar over `panic` /
`catch-unwind`.**

- `EX_THROW` lowers to `tur_panic_with(type_tag, payload, file, line)`
  -- identical to the `(panic ...)` form (`emit_expr.c:2028-2043`).
- `EX_TRY_CATCH` lowers to a `tur_catch_unwind(thunk, env, &out)` call
  -- identical to `(catch-unwind ...)` from `stdlib/panic.tur` (`emit_expr.c:2044+`).
- `tur_panic_with`, `tur_catch_unwind`, the global `setjmp`/`longjmp`
  plumbing in `emit_module.c:5567-5738`, the `Panic` opaque, and
  `panic-message` / `panic-file` / `panic-line` (`stdlib/panic.tur`)
  are all *load-bearing for `panic` itself* -- they ship in every
  program regardless of whether `try` is ever used.

**Consequence for DEPR-D0:** deleting compiled `throw`/`try`/`catch`
removes *only* the front-end (elab + IR enum tags + emit dispatch +
borrow-check arm + the `sym_throw`/`sym_try` interns). It does **not**
let us simplify the runtime: every `setjmp` site, the `longjmp`, the
global handler precedence at `:5926`, and the `tur_catch_unwind` thunk
ABI all stay, because `panic` and `catch-unwind` keep using them. This
is good news for risk -- the deletion is a small, localized front-end
change -- but it means the v1 motivation has to be framed in terms of
*surface area* (one less form to teach, one less channel for typeclass
/ effect work to model, one less footgun for `result<T,E>`-shaped
code), not *runtime simplification*.

Actual surface usage is small:

- **Stdlib:** one site -- `assert-throws!` in `stdlib/test.tur:88`.
- **Fixtures:** one directory, `tests/fixtures/try-catch-compiled/`
  (three `(try ... (catch [e] ...))` cases, all written to test the
  construct itself).
- **Spices, guides, upcoming/reported plans:** no live consumers.

Meanwhile the rest of the language has moved to typed error reporting:

- `result<T,E>` (`stdlib/result.tur`) is the documented way to surface
  a recoverable failure.
- `panic` + the global `setjmp` handler is the documented way to abort
  on an unrecoverable failure; deferred cleanup is already wired up
  (`emit_module.c:5573`).
- New typeclass / effect work (Session types SS, dynvars DV, sized
  types SZ) does not assume an exception channel.

`throw` is therefore a parallel error path that every new pass has to
keep handling for one stdlib helper and one fixture. The "No Lazy
`:int` Stand-Ins" rule's reasoning applies in spirit: `throw` erases
the error type into a single dynamic channel, which is exactly what
`result<T,E>` exists to replace.

## Goals

1. **DEPR-W0 -- Warn on use.** Elaboration of `throw` / `try` /
   `catch` emits a deprecation diagnostic (`TUR-W0062`?) pointing at
   this plan, with the existing lifecycle-warning machinery
   (`experiment_warn_if_used`-shaped) as the model. The forms keep
   working; downstream code gets one release of notice.
2. **DEPR-M0 -- Migrate the stdlib site.** Rewrite
   `assert-throws!` (`stdlib/test.tur:88`) to catch a `panic` rather
   than a `throw`, or to take a thunk returning
   `result<unit, PanicError>`. Whichever shape lands, it must not
   leak `try`/`catch` into user-facing test code.
3. **DEPR-F0 -- Retire the fixture.** `tests/fixtures/try-catch-
   compiled/` is the only compiled coverage of the construct. Move it
   to `tests/fixtures/_legacy-throw/` and gate behind a `requires.*`
   marker for the deprecation window so the deletion PR can drop the
   marker file with the rest of the implementation.
4. **DEPR-R0 -- Internalize the interpreter's fiber-rejection throw.**
   (Compiled-path note: no work; compiled await already doesn't throw
   -- see *Preliminary research findings*.) The interpreter's
   `turi_await_future` mid-event-loop branch (`src/turi/fiber.c:360-
   372`) reaches for `turi_native_throw` while the entry-rejection
   branch (`:317-321`) just returns a `TURI_ERROR`-tagged value. Make
   the two branches consistent: set `env->throw_value` to the
   `TURI_ERROR` value and clear `env->throwing`, so the caller
   observes the rejection through the same path either way.
   `with-timeout`'s timeout signal (`fiber.c:554`) becomes a
   `TURI_ERROR("timeout")` return -- the cancel half (`fiber.c:303`)
   is already non-throw.
5. **DEPR-D0 -- Delete the front-end paths.** Once DEPR-M0/F0/R0 have
   shipped for one release, remove `elab_throw` / `elab_try_catch`,
   the `EX_THROW` / `EX_TRY_CATCH` arms in `emit_expr.c:2027-2099+`,
   `expr.c:505`, `expr.h:884`, `emit_stmt.c:172`, and
   `borrow_check.c:714`; drop the IR enum tags; drop the `sym_throw`
   / `sym_try` interns. In `src/turi/eval.c`, delete the
   `EX_THROW` / `EX_TRY_CATCH` case at `:6558` and the
   `turi_native_throw` / `tur_throw` natives that only the
   user-level forms call (the interpreter's *internal* uses of
   `turi_native_throw` have already been converted by DEPR-R0). The
   runtime helpers `tur_panic_with` / `tur_catch_unwind` and the
   global `setjmp` / `longjmp` plumbing in
   `emit_module.c:5567-5738` **stay** -- they're load-bearing for
   `panic` / `catch-unwind`.

## Non-goals

- **Replacing `panic`.** `panic` + global `setjmp` handler is the
  documented unrecoverable-failure mechanism and stays. The deletion
  in DEPR-D0 only removes the *user-callable* `throw`/`try`/`catch`
  layer; the `panic` plumbing it shares lives on independently.
- **Building new error-handling sugar.** This plan is a retirement,
  not a redesign. If anything new is wanted (e.g. a `?`-style early-
  return on `result<T,E>`), that is a separate plan.
- **Changing the fiber API surface.** DEPR-R0 changes the *signal*
  fibers use to report rejection, not the public
  `await`/`select`/`timeout` shapes.

## Steps

### DEPR-W0 -- Deprecation warning (~1 day)

1. Add `TUR-W0062` (or next free warning code) "use of deprecated
   exception forms `throw`/`try`/`catch`; migrate to `result<T,E>` /
   `panic`; see `docs/upcoming/throw-deprecation-plan.md`".
2. Fire it once per source location from `elab_throw` and
   `elab_try_catch` (deduplicate per `(file, line, col)` like other
   lifecycle warnings).
3. Add a fixture under `tests/fixtures/throw-deprecation-warning/`
   that asserts the warning fires.

### DEPR-M0 -- Migrate `assert-throws!` (~half day)

1. Decide the replacement shape -- either:
   - `assert-panics! [thunk]` catching a `panic` via whatever
     stdlib-side helper sits over the existing global handler, or
   - `assert-err! [thunk : (fn [] result<T,E>)]` for the
     result-shaped path.
2. Update `stdlib/test.tur` (current site: `:88`) and the
   `assert-throws!` entry in `stdlib/docstrings.tur` accordingly.
3. Regenerate docstring snapshots (`tur run docs --emit-tur
   stdlib/docstrings.tur`).
4. Sweep callers (`grep -rn "assert-throws!"` -- expected: none
   outside the stdlib file itself).

### DEPR-F0 -- Retire the fixture (~half day)

1. Move `tests/fixtures/try-catch-compiled/` to
   `tests/fixtures/_legacy-throw/` (leading underscore keeps it last
   in the directory listing while it survives).
2. Add a one-line `expected.note` recording that this fixture exists
   to keep the deprecation warning surface tested and will be deleted
   alongside DEPR-D0.

### DEPR-R0 -- Internalize interpreter fiber-rejection throws (~1-2 days)

> **Hidden dependency (2026-06-23 follow-up).** An attempt at this
> step revealed two interpreter fixtures whose contract relies on
> throw-through-await semantics that the plan did not account for:
>
> - `tests/turi/eval-async-error.tur` -- catches an async-body
>   `(throw "boom")` with a top-level `(try (await ...) (catch [e
>   :cstr] ...))`.
> - `tests/turi/eval-async-timeout.tur` -- catches the `with-timeout`
>   throw via the same `(try ... (catch))` shape.
>
> Both fixtures `(catch [e :cstr])` and inspect the bound message.
> The interpreter exposes no `error?` / `error-message` native that
> would let the new TURI_ERROR-returning shape carry the message
> through to user code, so the fixture migration is larger than the
> "harmonize two branches" framing in this section. Doing R0 cleanly
> needs:
>
> 1. an interpreter-level predicate + accessor for TURI_ERROR values
>    (e.g. `(error? v)` / `(error-message v)`), then
> 2. the fixtures rewritten to `(let [r (await ...)] (if (error? r)
>    ...))`, then
> 3. the fiber.c branch changes below.
>
> Until that surface lands, R0 stays deferred. W0/M0/F0 above are
> independent and can ship now.

The research collapsed this from "redesign the awaiter contract"
into "harmonize two branches of one function." The compiled path
needs no work.

1. In `turi_await_future` (`src/turi/fiber.c:360-372`), replace
   `turi_native_throw(env, turi_error_message(f->result))` and the
   bare `turi_native_throw(env, "async task rejected")` with the
   same shape the entry branch at `:317-321` uses: return a
   `TURI_ERROR`-tagged `TuriValue` directly. Where a non-fatal
   `(try ...)` in interpreter source code previously caught it, the
   caller already handles `TURI_ERROR` values -- that's how the
   `:317` branch has been working.
2. In `native_with_timeout` (`fiber.c:510-556`), swap the trailing
   `turi_native_throw(env, "timeout")` for `return
   turi_errorf("timeout")`. `turi_task_cancel` at `:299-304` is
   already non-throw and needs no change.
3. Run the interpreter fixture suite; specifically exercise:
   - rejected-future-via-await
   - `with-timeout` timing out
   - `with-timeout` task resolving before the timer.
4. Re-grep `turi_native_throw` -- once DEPR-R0 lands, the only
   remaining call sites should be the user-level `(throw ...)`
   form's elab and any direct test natives. Anything else is a
   missed conversion.

*Out of scope for DEPR-R0:* changing compiled-await rejection from
`abort()` to a `result<T, FiberError>` return. That's a worthwhile
follow-up (the abort behaviour is harsh for recoverable async errors)
but it's a separate plan; nothing about removing `throw` depends on
it.

### DEPR-D0 -- Delete the front-end (~1-2 days, after one release)

Scope narrowed by the research: this is a front-end deletion only.
Runtime helpers (`tur_panic_with`, `tur_catch_unwind`, the global
`setjmp`/`longjmp` plumbing) all stay because `panic` /
`catch-unwind` use them.

1. Drop `elab_throw` / `elab_try_catch` and the dispatch lines at
   `src/compiler/elab_call.c:1561-1562`.
2. Drop the `EX_THROW` / `EX_TRY_CATCH` arms in
   `src/compiler/expr.c:505`, `src/compiler/expr.h:884`,
   `src/compiler/emit_expr.c:2027-2099+`, `src/compiler/emit_stmt.c:172`,
   and `src/passes/borrow_check.c:714`. Remove the corresponding IR
   enum tags.
3. **Do not touch** the global-handler emit at
   `src/compiler/emit_module.c:5567-5738` or the precedence comment at
   `:5926`. Those exist for `panic`, not for `try`/`catch`. (Original
   draft had a "simplify the global handler" step here -- that was
   wrong; see *Compiled `throw` / `try` are syntactic sugar over
   `panic` / `catch-unwind`* above.)
4. Drop the `EX_THROW` / `EX_TRY_CATCH` case in `src/turi/eval.c:6558`
   and the `turi_native_throw` / `tur_throw` natives (DEPR-R0 has
   already converted the interpreter's internal callers). Keep
   `env->throw_value` if any non-throw caller still uses it as a
   scratch slot; remove `env->throwing` if not.
5. Drop `e->sym_throw` / `e->sym_try` interns
   (`src/compiler/elab_core.c:1538-1539`) and their fields in
   `elab_internal.h:370-371`.
6. Delete `tests/fixtures/_legacy-throw/` and the
   `throw-deprecation-warning` fixture.
7. Regenerate fixture snapshots; run the full suite
   (`bash tests/run.sh`, 10-minute timeout); confirm zero `FAIL`.

## Test plan

- **DEPR-W0:** new fixture asserts `TUR-W0062` fires on `throw` /
  `try`. Existing `try-catch-compiled` fixture goes from PASS to
  PASS-with-warning -- update expected stderr.
- **DEPR-M0:** `assert-throws!` (or its replacement) keeps a
  fixture that exercises the panic path.
- **DEPR-R0:** the fiber-rejection / timeout fixtures swap their
  `try`/`catch` shape for the new `result`-returning awaiter. The
  property under test (rejected fiber surfaces an error to the
  awaiter, timeout cancels and reports) is unchanged; the *shape*
  is.
- **DEPR-D0:** full `bash tests/run.sh` clean; spot-check that no
  external spice in `../turmeric-spices/` references the removed
  natives or forms.

## Open questions

1. **Replacement for `assert-throws!`.** Catch a `panic`, or take a
   `result`-returning thunk? Existing stdlib tests will pick one --
   probably `assert-panics!`, since panic is the closest mechanical
   analogue.
2. **Does any spice rely on `throw`?** A `grep` in
   `../turmeric-spices/` before DEPR-W0 lands would close this; the
   in-repo search came back clean.
3. **Warning code allocation.** Pick the next free TUR-W code; the
   experiments machinery already uses TUR-W0060 / W0061.
4. **Single release vs. two releases.** The deprecation policy in
   CLAUDE.md is written for in-flight features behind
   `--enable=<name>`. This is the reverse direction (graduated
   feature being shelved) and not formally covered. One release of
   warning seems right given the tiny surface; revisit if the
   external-spice grep turns up anything.

## Dependencies / coordination

- Should land *after* any in-flight work that already touches
  `borrow_check.c` Phase-S4 cases or the fiber rejection path, so
  the deletion is one focused PR rather than a series of conflict
  resolutions.
- No coordination needed with the experiments registry (this is not
  a new `--enable` flag; it's a deprecation of shipped surface).
- Fixture regeneration is local to the touched fixtures -- no
  codebase-wide snapshot regen is implied.
