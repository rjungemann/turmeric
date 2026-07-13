---
title: Shallow effect handlers -- from the DK substrate to source (F2)
category: Planning
status: COMPLETE + ARCHIVED 2026-07-12 -- `handle-shallow` graduated to always-on (cps-effects experiment deleted; a lingering --enable=cps-effects is an accept-and-warn no-op via GRADUATED[]). Surface + CPS emit + interpreter AND compiled-fiber full shallow + multishot combo + reachability diagnostic all landed; compiled == interp on every shallow shape.
description: F2 of the first-class-continuations work (DONE). Drove shallow effect handlers all the way to source -- a handle-shallow surface form, correct outer-propagation in the runtime, CPS-IR + compiled emit, parity on the fiber fallback and tree-walking interpreter, and the cps-effects experiment (now graduated). Multi-shot effect resume rides the existing ^multishot annotation. Split out of compiled-first-class-continuations-followups-plan.md; its async sibling is compiled-async-heap-continuations-plan.md (F3, still active).
---

> **COMPLETE + ARCHIVED 2026-07-12.** Everything below landed and `handle-shallow`
> graduated to an always-on language feature (the `cps-effects` experiment was
> deleted; see `src/runtime/experiments.c` `GRADUATED[]`). Retained as the
> historical record. The independent async sibling F3 is
> [compiled-async-heap-continuations-plan.md](compiled-async-heap-continuations-plan.md).

# Shallow effect handlers -- from the DK substrate to source (F2)

## Context

The scoping and substrate rationale live in the archived parent,
[compiled-first-class-continuations-plan.md](compiled-first-class-continuations-plan.md),
and the combined F2/F3 follow-up it was split from,
[compiled-first-class-continuations-followups-plan.md](compiled-first-class-continuations-followups-plan.md).
Where things stand:

- **F1 -- landed, always-on.** `handle` / `perform` / `resume` lower onto the DK
  heap-continuation substrate as `CT_HANDLE` / `CT_PERFORM` / `CT_RESUME`
  (`src/compiler/emit_cps_ir.c`, built from the CTerm handle node in
  `src/passes/cps_ir.c:1285`). Shipped as the CPS-IR-to-C backend's Phase C4;
  graduated with the `cps-backend` flag (commit `d50dd2b89`). Fixture
  `tests/fixtures/cps-backend-effect/`.
- **F2 runtime substrate -- landed 2026-07-12.** `dk_perform`
  (`src/runtime/cps_prompt.c`) reads a `shallow` bit on the matched handler
  marker: `dk_handler` (deep) re-installs on the captured sub-continuation
  (analogue of `shift`); `dk_handler_shallow` (new) does not (analogue of
  `shift0`). `dk_has_handler` was added to assert the reinstall bit
  structurally, and `tests/cps_prompt_unit.c` gained the
  `cps-effect-deep-reinstall` / `cps-effect-shallow-no-reinstall` probes. The
  reify is currently single-level; F2.1 below makes it correct for a shallow
  re-perform reaching an enclosing handler.

The fiber runtime stays the default `async` scheduler; nothing here touches it.

## Goal

A program can write a shallow handler; deep and shallow share the one DK
substrate end-to-end (surface -> elaboration -> CPS IR -> compiled runtime ->
tree-walking interpreter); multi-shot effect resume is permitted where the source
opts in. Ships behind `--enable=cps-effects` while the surface semantics soak.

## F2.1 -- Runtime: correct shallow outer-propagation -- LANDED 2026-07-12

The landed substrate did the reinstall/no-reinstall split, but the reify in
`dk_perform` (`src/runtime/cps_prompt.c`) was single-level: a shallow resume's
captured sub ended at `dk_done()`, so a subsequent same-effect `perform` inside
that resume reached the chain end (unhandled) instead of an enclosing handler.
Fixed:

- On a SHALLOW handler match, `dk_perform` now builds the captured sub as
  `dk_copy_range(k, H)` followed by `dk_copy_enclosing_handlers(H->next)` -- a
  copy of the enclosing `DKK_HANDLER` markers (frames and prompts skipped),
  terminated by `dk_done()` -- instead of a bare `dk_done()`. Handler markers are
  transparent to a returning value (`dk_run_impl`'s `DKK_HANDLER` case), so a
  normal resume still surfaces `E[w]`'s value into the case; a re-`perform`
  during the resume finds the copied enclosing handler for its tag
  (`dk_perform`'s linear search, which already walks through prompts). If no
  enclosing handler carries the tag, it hits `dk_done()` and aborts as
  "unhandled" -- correct. Prompts are skipped in the copy to stay consistent with
  `dk_perform`'s own search, which walks through them to the next handler/root.
- Regression added: `tests/cps_prompt_unit.c` `cps-effect-shallow-outer-propagation`
  -- an inner shallow handler nested under an outer deep handler of the same tag;
  the inner case re-performs and is caught by the OUTER handler (delivered value
  `106` = `5 + 100` resumed through `add1`), asserting both the structural
  presence of the enclosing marker and the behavioral outcome. The existing
  `cps-effect-shallow-no-reinstall` probe still holds (no enclosing handler ->
  copied tail is just `dk_done()`, identical to before), so a shallow handler
  with nothing enclosing is unchanged.
- Deep behavior is byte-identical (the deep tail still re-installs this handler);
  `effect-deep-handler` and the full suite stay green (2108 passed, 0 failed),
  and the unit target passes 8/8 leak-clean under `detect_leaks=1`.

## F2.2 -- Surface: a `handle-shallow` form -- LANDED 2026-07-12

Mirrors the `shift` / `shift0` split on the effect side with a `handle` (deep) /
`handle-shallow` (shallow) split -- a distinct form, not an ad-hoc annotation, so
it reads the same way the delimited-control surface already does. Landed:

- `e->sym_handle_shallow` interned in `src/compiler/elab_core.c` beside
  `sym_handle`; field declared in `src/compiler/elab_internal.h`.
- `bool shallow;` added to `HandleExpr` (`src/compiler/expr.h`). `elab_handle`
  was refactored into a shared `elab_handle_impl(e, call, shallow)` with two thin
  wrappers -- `elab_handle` (deep) and `elab_handle_shallow` (shallow) -- so
  `handle-shallow` shares `handle`'s entire pre-scan / parse / typecheck / case
  path and differs only in the stamped bit. `try-with`, `with-handler`, the async
  T25 sugar, and the `handler` value literal all stay deep (`shallow = false`).
  Dispatched in `src/compiler/elab_call.c` next to `sym_handle`.
- Formatter: `handle-shallow` maps to `SF_HANDLE` in `src/compiler/fmt.c`
  (`fmt_handle` re-emits the head token generically, so the layout is identical
  and the head is preserved -- no separate `SF_` value needed). Verified
  idempotent.
- Verified: a `handle-shallow` program parses, type-checks, `emit-c`s, and runs
  on both the compiled and `--interpret` paths; the formatter round-trips it. The
  full suite stays at 2108 passed, 0 failed.

**Intermediate state (closed by F2.3/F2.4).** The `shallow` bit is carried on
`HandleExpr` but not yet consumed: the CPS-IR + compiled emit (F2.3) and the
tree-walking interpreter (F2.4) still lower `handle-shallow` as **deep**. F2.3
also adds the `--enable=cps-effects` gate and the CPS-IR `shallow` field, and
F2.4 the interpreter re-entry gate; only then does `handle-shallow` behave
shallowly. Until then it is a deep synonym -- correct for the deep==shallow smoke
above, and the reason no distinguishing fixture is committed yet (that lands with
the gate in F2.3/F2.6, so the fixture can carry `--enable=cps-effects`).

## F2.3 -- CPS IR + compiled emit + gate + experiment -- LANDED 2026-07-12

- `bool shallow;` added to the CTerm handle struct (`src/passes/cps_ir.h`) and set
  from `HandleExpr` in `build_handle` (`src/passes/cps_ir.c`).
- `emit_handle` (`src/compiler/emit_cps_ir.c`) selects `dk_handler_shallow` vs
  `dk_handler` per case from `t->as.handle.shallow`; nothing else in the lowering
  changes.
- **DK runtime preamble mirrored.** The compiled backend emits its own copy of
  the DK runtime into the generated C (`src/compiler/emit_dk_runtime.c`), not the
  in-tree `cps_prompt.c`. That copy was updated to match: the `shallow` struct
  field, `dk_handler_shallow`, `dk_copy_enclosing_handlers`, and the F2.1
  `dk_perform` shallow branch. All 139 `expected.c` snapshots that embed the DK
  preamble were regenerated in this commit (the diff is preamble-only).
- Surface gated on `g_opt_cps_effects` in `elab_handle_impl`
  (`src/compiler/elab_effects.c`): a `handle-shallow` form with the flag off is a
  hard error pointing at `--enable=cps-effects`; with the flag on it warns
  (TUR-W0060) via `experiment_warn_if_used("cps-effects")`. The `cps-effects`
  experiment row is registered in `src/runtime/experiments.c` (`g_opt_cps_effects`
  in `globals.{c,h}`, introduced 0.28.2, expires 0.30.0, prototype).
- Verified: gate error without the flag; warning + compile with it;
  `dk_handler_shallow` emitted on the DK path; a single-perform shallow handler
  runs correctly (deep-equivalent); full suite 2108 passed, 0 failed.

**Discovered limit -- routing, now owned by F2.4.** Shallow-vs-deep is not yet
observable end-to-end, and the reason is *routing*, not the emit above. The DK
effect path (`dk_perform` call sites) is only taken for a **single perform per
continuation**; a body that performs the same effect twice -- which is exactly
what a distinguishing shallow test needs (a re-perform after resume, or a nested
handler reached on escape) -- falls out of the CPS-emittable subset and lowers
through the **fiber fallback** (`src/compiler/emit_effects.c`), which does not
consult the `shallow` bit. So a multi-perform / nested `handle-shallow` currently
compiles **silently as deep**. That is precisely the silent-miscompile F2.4 is
scoped to close ("do not silently miscompile it as deep"). The DK-path shallow
semantics themselves are correct and unit-proven (F2.1 `dk_perform` probes); what
remains is making the paths that distinguishing programs actually take honor the
bit.

## F2.4 -- Fallback + interpreter parity -- LANDED 2026-07-12 (interp full; compiled hard-error floor)

This is where shallow becomes **observable**: F2.3 confirmed that every
distinguishing program (multi-perform / nested) routes to a non-DK path, so
honoring `shallow` here is what makes `handle-shallow` actually shallow.

- **Tree-walking interpreter (`src/turi/eval.c`) -- FULL shallow, landed.** The
  interpreter has two effect engines and both were gated on `shallow`:
  - The primary **work-stack driver**: at `DK_RESUME` the captured prompt was
    re-installed *active* (`index = 1`), catching a re-perform (deep). For a
    shallow handler it is now re-installed **inactive** (`index = 0`): still a
    return delimiter that restores the env boundary and delivers the slice value,
    but skipped by the perform scan (which ignores `index == 0`), so a re-perform
    reaches the nearest enclosing active prompt (or the fiber path / unhandled if
    none) -- the exact mirror of `dk_perform`'s no-reinstall tail.
  - The **fiber fallback** (`eval_resume_cont`): the deep "re-install handler
    frame around the body re-entry" is now gated on `!shallow` (shallow pushes no
    frame, so a re-perform reaches the enclosing `handler_stack` entry).
  - Verified end-to-end via `tur interpret`: nested inner-shallow/outer-deep
    gives `105` (first caught by inner -> 5, second escapes to outer -> 100) vs
    `10` deep; a single shallow handler whose body performs twice with no outer
    handler correctly reports `unhandled effect` on the second perform. Deep is
    byte-identical (gated on `shallow`); full suite 2108/0 and the turi harness
    1578/0 unchanged.
- **Compiled fiber fallback (`src/compiler/emit_effects.c`) -- FULL shallow,
  landed (F2.4 tail).** The compiled fiber runtime keeps the intercept
  `EffectHandlerFrame` installed for the whole body lifetime, so it cannot drop
  the frame per-resume. Instead the per-handle dispatch DECLINES its own effect
  once its case has run: a `shallow_consumed` flag on `TurEffectCaptureCtx` (init
  false at the handle site) guards each case-match (`!__dcap->shallow_consumed &&
  strcmp(...)`) and is set true when a case fires. After the shallow case runs, a
  later same-effect perform matches no case and falls through to the existing
  **bubble-up** branch, which forwards it to the enclosing handler and
  re-dispatches the inner body with that handler's resume value -- exactly the
  interpreter's shallow semantics. All of this is gated on `h->shallow`, so deep
  codegen is byte-identical (only the `.shallow_consumed = false;` cap init is
  added for every handle, which is why the effect-fixture snapshots regenerated).
  The earlier hard-error floor is removed.
  - Verified compiled == interpreter across shapes: nested inner-shallow/outer-deep
    `-> 105` (vs deep `10`), triple-perform shallow `-> 201`, two-effect shallow
    `-> 65`, `^multishot` shallow `-> 30`, and a shallow re-perform with no outer
    handler aborts "unhandled effect" on both paths. The distinguishing case is now
    an ordinary fixture that runs on BOTH harnesses (see F2.6).

### F2.4 tail -- LANDED 2026-07-12

- **Full shallow on the compiled fiber runtime.** Done (see the fiber-fallback
  bullet above): the `shallow_consumed` bubble-up gives multi-perform / nested
  `handle-shallow` correct shallow semantics on the compiled path, matching the
  interpreter. The hard-error is gone.

### TUR-W0033 reachability refinement -- LANDED 2026-07-12

The effect-reachability analysis flagged an outer handler clause as "unreachable"
for a nested shallow case, because `collect_effects_in_expr` (`src/passes/
effect_check.c`) absorbed a shallow handle's effect from the row exactly like a
deep one -- so an inner `handle-shallow` for `Op` made `Op` vanish from the
enclosing function's inferred row, and the outer `Op` clause looked dead. Fixed:
a shallow handle no longer removes its effect from the row (gated on `h->shallow`;
deep is unchanged), so the effect stays in the enclosing fn's inferred row and an
outer handler for it is correctly seen as reachable. This is also the accurate
row -- a shallow resume can re-perform the effect, so it genuinely can escape.

Verified: the nested inner-shallow/outer-deep program compiles with **no**
TUR-W0033; it did not introduce any spurious unhandled-effect error (a shallow
program whose effect reaches `main` still compiles -- `effect-shallow-basic`
`-> 420`, `effect-shallow-handler` `-> 105`); no codegen change (diagnostic pass
only, so no snapshot churn); suites stay 2112/0 and 1582/0. Pinned by probe 6
(`no-false-unreachable-warning-on-shallow-outer`) in
`tests/shallow-handler-probes.sh`.

### Still remaining

- **Graduate `cps-effects`** once it has soaked: delete the row + `g_opt_cps_effects`,
  make `handle-shallow` always-on, and add `cps-effects` to `GRADUATED[]` for one
  minor line. (Everything the flag gates -- surface, CPS emit, interpreter, and
  compiled fiber path -- is implemented and `compiled == interp` on all shapes.)

## F2.5 -- Multi-shot effect resume (confirm + combine) -- LANDED 2026-07-12

Multi-shot already works: the DK substrate copies per resume (`dk_invoke`), and
`cont_check_double_use` (`src/compiler/elab_effects.c`) already exempts
`CK_MULTISHOT`, so a `^multishot` handler clause resumes any number of times.
This phase confirmed the two opt-ins compose and added a fixture:

- **Combined fixture -- `tests/fixtures/effect-shallow-multishot/`.** A
  `^multishot` **shallow** handler whose clause resumes twice
  (`(+ (resume k 10) (resume k 20))` -> `30`). Single perform, so it stays on the
  CPS path (`dk_handler_shallow`) where deep==shallow; the point is that
  multi-shot resume + `handle-shallow` compose and compile. Passes both harnesses
  (compiled `run.sh` and interpreter `run-turi.sh`), carrying
  `flags = --enable=cps-effects`.
- **Routing confirmed consistent with F2.3/F2.4.** A single-perform
  multishot+shallow handler compiles via CPS (`dk_handler_shallow`, verified). A
  *multi-perform* multishot+shallow handler leaves the CPS subset and hits the
  F2.4 fiber-path hard-error -- no silent deep -- while the interpreter runs it
  shallowly (verified: nested inner-multishot-shallow / outer-deep -> `105`).
- **O3 owning-capture hazard still guarded -- verified.**
  `TUR_E0500_MULTISHOT_UNIQUE_CAPTURE` fires identically for `handle-shallow` as
  for `handle` (it lives in the elaborator on the shared `HandleExpr`, so the
  shallow bit does not touch it), and the `g_cap_single_shot` zero-capture cut
  (`src/compiler/emit_cps_ir.c`) is unchanged. A multi-shot resume that captures
  an owning value is still refused (TUR-E0500); nothing here loosened it.

## F2.6 -- Fixtures + acceptance -- LANDED 2026-07-12

With the F2.4 tail landed (compiled fiber path does full shallow), the
distinguishing shallow program now runs on BOTH harnesses, so it is an ordinary
fixture -- no interpret-only carve-out needed.

- **`tests/fixtures/effect-shallow-handler/` -> `105`** -- THE distinguishing
  fixture: inner shallow / outer deep, body performs Op twice; the inner catches
  the first (resume 5), the re-perform escapes to the outer (resume 100) for
  `5 + 100 = 105` (a deep inner would give `10`). Runs compiled (`run.sh`) and
  interpreted (`run-turi.sh`), both `105`.
- **Compile-able single-perform fixtures (both harnesses)**, where deep ==
  shallow: `tests/fixtures/effect-shallow-basic/` -> `420`;
  `tests/fixtures/effect-shallow-multishot/` -> `30` (F2.5; `^multishot` +
  shallow compose).
- **Probe -- `tests/shallow-handler-probes.sh`** (ctest
  `tur_shallow_handler_probes`). Six checks, all green: interpreter nested
  inner-shallow/outer-deep -> `105` (vs deep `10`); a shallow resume that
  re-performs with no enclosing handler -> `unhandled effect` (both paths);
  compiled nested shallow AGREES with the interpreter (`105`); `^multishot` +
  shallow compose -> `30`; and no false TUR-W0033 on a deep outer clause over a
  shallow inner handler.

(A gate negative fixture existed while `cps-effects` was live -- it verified
`handle-shallow` was rejected without `--enable=cps-effects`. It was removed at
graduation, since there is no gate to reject anymore.)

**Acceptance met.** `effect-deep-handler` and `multishot-handler` stay green
unchanged; **compiled == interp** for every shallow fixture, including the
distinguishing `105` case; the full suites stay at `0 failed` (`run.sh` 2111/0,
`run-turi.sh` 1581/0, plus the probe and the F2.1 `cps_prompt_unit` substrate
probes).

## Experiment registration -- `cps-effects` -- GRADUATED 2026-07-12

`cps-effects` gated F2's shallow-handler **source surface** while it soaked. It
was registered (introduced 0.28.2, expires 0.30.0, `XF_LIFECYCLE_PROTOTYPE`,
`opt_global -> &g_opt_cps_effects`) with the surface (F2.3), and **graduated on
2026-07-12** once every path agreed (`compiled == interp`) and hot-path
neutrality held (all shallow codegen is gated on `h->shallow`, so deep is
byte-identical). At graduation:

- the `EXPERIMENTS[]` row and `g_opt_cps_effects` (`src/runtime/globals.{c,h}`)
  were deleted, and the `elab_handle` gate + `experiment_warn_if_used` call were
  removed -- `handle-shallow` is now unconditionally accepted;
- `cps-effects` was added to `GRADUATED[]` in `src/runtime/experiments.c`, so a
  lingering `--enable=cps-effects` (CLI / `build.tur` / user config) is an
  accept-and-warn no-op (TUR-W0063) for one minor line rather than a hard
  TUR-E0310;
- the shallow fixtures dropped their `flags` files and the gate negative fixture
  was removed.

Graduation was independent of F3 (`cps-async`), which is a separate,
not-yet-registered experiment in the sibling plan.

## Depends on / reuses

- DK multi-prompt substrate (`src/runtime/cps_prompt.{h,c}`, now carrying the
  deep/shallow bit) and the CPS-IR-to-C backend (`src/compiler/emit_cps_ir.c`,
  `src/passes/cps_ir.{h,c}`, always-on).
- The `^multishot` annotation and its elaborator gate
  (`src/compiler/elab_effects.c`), reused unchanged for multi-shot effect resume.
- Tree-walking interpreter effect machinery (`src/turi/eval.c`,
  `eval_handle_inner`) -- extended for shallow parity.

## Out of scope

- Source-level surface changes to `defeffect`, effect rows, or effect
  polymorphism. `handle-shallow` is a new *control* form, not a change to the
  effect *declaration* surface.
- `async` / `await` on heap continuations -- that is F3, in the sibling plan
  [compiled-async-heap-continuations-plan.md](compiled-async-heap-continuations-plan.md).
