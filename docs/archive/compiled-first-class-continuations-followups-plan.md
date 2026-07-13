---
title: First-class continuations -- F2/F3 follow-ups (SPLIT + ARCHIVED)
category: Planning
status: archived 2026-07-12 -- split into two active plans (see banner)
description: Superseded. This combined F2/F3 follow-up was split into two independently-expirable active plans -- compiled-shallow-handlers-plan.md (F2, shallow effect handlers to source) and compiled-async-heap-continuations-plan.md (F3, async/await on heap continuations). Kept here as the shared history; the live work is in the two split plans. F2's runtime substrate + gate decision landed 2026-07-12 before the split.
---

> **ARCHIVED 2026-07-12 -- this plan was split, not shelved.** The two phases are
> independently expirable, so they now live in their own active plans under
> `docs/upcoming/v1/`:
>
> - **F2 -- shallow effect handlers to source:**
>   [compiled-shallow-handlers-plan.md](compiled-shallow-handlers-plan.md)
> - **F3 -- async/await on heap continuations:**
>   [compiled-async-heap-continuations-plan.md](compiled-async-heap-continuations-plan.md)
>
> Both are in implementation. The F2 runtime substrate (`dk_handler_shallow`) and
> the experiment-gate decision landed 2026-07-12, before the split. The text
> below is retained verbatim as the shared history the two plans were carved
> from; the authoritative, current subtask breakdown lives in the two plans above.

# First-class continuations -- shallow handlers to source + async on heap continuations (F2 / F3)

## Context

The scoping, the substrate rationale, and the landed F1/F4 work live in the
archived parent,
[compiled-first-class-continuations-plan.md](compiled-first-class-continuations-plan.md).
Where things stand:

- **F1 -- landed, always-on.** `handle` / `perform` / `resume` lower onto the DK
  heap-continuation substrate as `CT_HANDLE` / `CT_PERFORM` / `CT_RESUME`
  (`src/compiler/emit_cps_ir.c`, built from the CTerm handle node in
  `src/passes/cps_ir.c:1285`). A `perform` captures a real sub-continuation up to
  the enclosing `handle` and `resume` invokes it via the DK machine. Shipped as
  the CPS-IR-to-C backend's Phase C4; graduated with the `cps-backend` flag
  (commit `d50dd2b89`). Fixture `tests/fixtures/cps-backend-effect/`.
- **F2 runtime substrate -- landed 2026-07-12.** `dk_perform`
  (`src/runtime/cps_prompt.c`) reads a `shallow` bit on the matched handler
  marker: `dk_handler` (deep) re-installs on the captured sub-continuation
  (analogue of `shift`); `dk_handler_shallow` (new) does not (analogue of
  `shift0`). `dk_has_handler` was added to assert the reinstall bit structurally,
  and `tests/cps_prompt_unit.c` gained the `cps-effect-deep-reinstall` /
  `cps-effect-shallow-no-reinstall` probes. The reify is currently single-level;
  F2 below makes it correct for a shallow re-perform reaching an enclosing
  handler.
- **F4 effect-rec probe -- landed.** `tests/probes/stackless-signoff/effect-rec.tur`
  wired into `tests/stackless-signoff-probes.sh`.

The fiber runtime stays the default `async` scheduler through F2 and into F3; F3
adds an *alternate* heap-continuation representation behind a flag, it does not
remove `ucontext`.

## Phase F2 -- shallow handlers, all the way to source

**Goal.** A program can write a shallow handler; deep and shallow share the one
DK substrate end-to-end (surface -> elaboration -> CPS IR -> compiled runtime ->
tree-walking interpreter); multi-shot effect resume is permitted where the source
opts in. Ships behind `--enable=cps-effects` while the surface semantics soak.

### F2.1 -- Runtime: correct shallow outer-propagation

The landed substrate does the reinstall/no-reinstall split, but the reify in
`dk_perform` (`src/runtime/cps_prompt.c`) is single-level: a shallow resume's
captured sub ends at `dk_done()`, so a subsequent same-effect `perform` inside
that resume reaches the chain end (unhandled) instead of an enclosing handler.
Fix it so shallow is correct:

- On a SHALLOW handler match, build the captured sub as
  `dk_copy_range(k, H)` followed by a **copy of the enclosing handler markers**
  reachable from `H->next` (the `DKK_HANDLER` nodes between `H` and the next
  delimiter/root, terminated by `dk_done()`), instead of a bare `dk_done()`.
  Handler markers are transparent to a returning value (`dk_run_impl`'s
  `DKK_HANDLER` case), so a normal return still surfaces `E[w]`'s value into the
  case; but a re-`perform` during the resume now finds the copied enclosing
  handler for its tag (`dk_perform`'s linear search), matching shallow semantics.
  If no enclosing handler carries the tag, it hits `dk_done()` and aborts as
  "unhandled" -- correct.
- Add a `dk_perform`-level regression to `tests/cps_prompt_unit.c`: a shallow
  handler nested under an outer deep handler of the *same* tag, where the shallow
  resume performs the tag a second time and it is caught by the OUTER handler
  (assert the delivered value distinguishes shallow-then-outer from deep). This
  is the behavioral counterpart to the structural probe already landed.
- Keep deep behavior byte-identical (`effect-deep-handler` fixture must stay
  green): the deep branch is untouched.

### F2.2 -- Surface: a `handle-shallow` form

Mirror the `shift` / `shift0` split on the effect side with a `handle` (deep) /
`handle-shallow` (shallow) split -- a distinct form, not an ad-hoc annotation, so
it reads the same way the delimited-control surface already does.

- Intern the keyword next to the others in `src/compiler/elab_core.c` (beside
  `sym_handle` at `:1433` and `sym_shift0` at `:1420`): add `e->sym_handle_shallow`.
- Add `bool shallow;` to `HandleExpr` (`src/compiler/expr.h:692`) and set it in
  `elab_handle` (`src/compiler/elab_effects.c:1165` region). `handle-shallow`
  shares `handle`'s entire parse/typecheck path -- deep vs shallow is
  type-transparent, so only the flag differs. `try-with` (`:490`) delegates to
  the same code and stays deep.
- Format support: add `SF_HANDLE_SHALLOW` alongside `SF_HANDLE`
  (`src/compiler/fmt.c:357`) so the formatter round-trips the new form.

### F2.3 -- CPS IR + compiled emit

- Add `bool shallow;` to the CTerm handle struct (`src/passes/cps_ir.h:178`) and
  set it where the term is built from `HandleExpr` (`src/passes/cps_ir.c:1285`).
- In `emit_handle` (`src/compiler/emit_cps_ir.c:3362`) emit
  `dk_handler_shallow(...)` instead of `dk_handler(...)` for each case when
  `t->as.handle.shallow` is set. Nothing else in the lowering changes -- the
  case lifting, env packing, and continuation frame are shared.
- Gate the *surface* elaboration on `g_opt_cps_effects`: reading a
  `handle-shallow` form while the flag is off is a hard diagnostic pointing at
  `--enable=cps-effects` (the deep `handle` path is unaffected and always-on).

### F2.4 -- Fallback + interpreter parity (both non-CPS runtimes)

A shallow handler must behave identically on the two non-CPS paths, or the parity
ratchets (`tools/check_turi_parity.py`, `check_turi_native_parity.py`) and
`--interpret` fixtures diverge:

- **CPS fallback (`src/compiler/emit_effects.c`).** When a colored function
  falls out of the CPS-emittable subset it lowers here (the fiber-lift path, also
  used by `^multishot` today). Thread the `shallow` flag into the handler-frame
  install so a fallback shallow handler drops the deep re-install. If that is too
  invasive for the first cut, make a shallow handler force the CPS path and emit
  a clear diagnostic when it cannot -- but do not silently miscompile it as deep.
- **Tree-walking interpreter (`src/turi/eval.c`).** `eval_handle_inner`
  (`:1041`) re-installs the handler frame around the body re-entry with the
  comment "Re-install handler frame around the body re-entry (deep semantics)"
  (`:1077`). Gate that re-install on the handle's `shallow` flag: shallow pops
  the handler frame before running the resumed body so a re-perform reaches the
  enclosing `handler_stack` entry. Add the shallow flag to the `HandleExpr` the
  interpreter reads (already shared with the compiler front-end).

### F2.5 -- Multi-shot effect resume (confirm + combine)

Multi-shot already works: the DK substrate copies per resume (`dk_invoke`), and
`cont_check_double_use` (`src/compiler/elab_effects.c:1494`) already exempts
`CK_MULTISHOT`, so a `^multishot` handler clause resumes any number of times
(`tests/fixtures/multishot-handler/` -> `30`, on the compiled/DK path). This
phase:

- Adds a combined fixture: a `^multishot` **shallow** handler, proving the two
  opt-ins compose (multi-shot resume + no re-install).
- Keeps the O3 owning-capture hazard guarded -- `TUR_E0500_MULTISHOT_UNIQUE_CAPTURE`
  (elaborator) and the `g_cap_single_shot` zero-capture cut
  (`src/compiler/emit_cps_ir.c`) stay exactly as they are until the
  env-capture / deep-clone story (owning-pointers follow-ups, task O3) lands. A
  multi-shot resume that captures an owning value is still refused; nothing here
  loosens that.

### F2.6 -- Fixtures + acceptance

- `tests/fixtures/effect-shallow-handler/` (compiled) -- a shallow handler whose
  resume re-performs the same effect and is caught by an enclosing handler,
  distinguishing shallow from deep by output. Add the interpreter twin
  (or a shared fixture that both paths run) so `--interpret` parity holds.
- `effect-deep-handler` and `multishot-handler` stay green unchanged.
- Acceptance: `direct == cps == interp` for the shallow fixtures; the full
  suite stays at `0 failed`; the `cps-effects` experiment warns (TUR-W0060) when
  a `handle-shallow` form is compiled with the flag on.

## Phase F3 -- `async` / `await` on heap continuations

**Goal.** An alternate, heap-continuation representation for `async` / `await`
that decouples "how many suspended computations are live" from "how many C stacks
are alive." Ships behind `--enable=cps-async`, per compilation unit /
per-function to start. The fiber scheduler stays the default; this is a second
representation, selected by the flag.

### F3.1 -- Lowering

- Lower `await` under CPS as a `shift` against an implicit `async` prompt
  installed at the fiber body's entry: elaborating an `(async fn)`
  (`src/compiler/elab_concurrent.c:52`) under the flag wraps the body in a
  `reset`-equivalent prompt, and each `await` inside becomes a `shift` capturing
  the sub-continuation up to that prompt. The suspended computation is a `DK*`
  (a `dk_kont`), not a suspended `ucontext` fiber.
- The compiled lowering reuses `emit_cps_ir.c` / `emit_core.c`; the await point
  emits a capture-and-register rather than a `swapcontext`.

### F3.2 -- Reactor integration

- On completion of the awaited I/O, the reactor (`src/async/reactor.c`,
  `scheduler.c`) resumes the suspended `DK*` with `dk_invoke(kont, result)` --
  on whatever thread the completion fires. This requires the DK chain to be
  safe to invoke off the spawning thread: audit `cps_prompt.c` allocation/free
  for thread-locality assumptions and make the kont handoff explicit (ownership
  transfers to the reactor at suspend, back to the resumed computation at
  invoke). No shared mutable DK node is invoked from two threads at once.
- The `atomic_queue` (`src/async/atomic_queue.c`) carries ready konts to the
  scheduler exactly as it carries ready fibers today.

### F3.3 -- Experiment gate

Register `cps-async` as a live experiment (F3's lowering is the "real lowering to
gate" the STRICT RULE requires):

- Row in `EXPERIMENTS[]` (`src/runtime/experiments.c`) with every descriptor
  field, `opt_global` -> `&g_opt_cps_async` (declared in
  `src/runtime/globals.{c,h}`), `plan_path` this doc, `introduced` the current
  release, `expires_at` two minor releases out, `XF_LIFECYCLE_PROTOTYPE`.
- `experiment_warn_if_used("cps-async")` at the async-lowering entry point.
- Independent of `cps-effects` -- separately expirable, never bundled.

### F3.4 -- Bugs addressed at the representation level

The archived reports
[turi-async-fiber-stack-never-reclaimed](history/turi-async-fiber-stack-never-reclaimed.md)
and
[turi-async-await-deep-recursion-garbage](history/turi-async-await-deep-recursion-garbage.md)
were patched on the fiber path; F3 removes the class at the representation level
(a suspended computation costs one heap chain, not one C stack), giving a durable
fix the flag can prove out.

### F3.5 -- Sign-off probes (F4 tail)

- `tests/probes/stackless-signoff/async-rec.tur` -- recursively-spawned suspended
  computations without proportional C-stack growth, under `ulimit -s`, wired into
  `tests/stackless-signoff-probes.sh` beside `effect-rec.tur`.
- Non-effect hot-path neutrality: measured neutral-or-better on the suite (the
  same gate the catch-unwind graduation held itself to).

## Experiment registration

- **`cps-effects`** -- the gate for F2's shallow-handler **source surface**
  (F2.2--F2.4). F1's effect lowering and F2's substrate are always-on; the gated
  in-flight surface is `handle-shallow`. Register the row in
  `src/runtime/experiments.c` with `opt_global` -> `&g_opt_cps_effects` read in
  `elab_handle` when it sees a shallow form, `experiment_warn_if_used("cps-effects")`
  at that entry point, `plan_path` this doc, `expires_at` two minor releases out.
  Graduate on F2 sign-off (all three paths agree) + hot-path neutrality; on
  graduation the flag is deleted, `handle-shallow` goes always-on, and the name
  is added to `GRADUATED[]` for one minor line.
- **`cps-async`** -- see F3.3. Separate, independently-expirable.

Both rows land *with* their feature (once the lowering they gate exists), never
before -- that is the STRICT RULE, and it is why the substrate landing above
carries no row.

## Depends on / reuses

- DK multi-prompt substrate (`src/runtime/cps_prompt.{h,c}`, now carrying the
  deep/shallow bit) and the CPS-IR-to-C backend (`src/compiler/emit_cps_ir.c`,
  `src/passes/cps_ir.{h,c}`, always-on).
- The `^multishot` annotation and its elaborator gate
  (`src/compiler/elab_effects.c`), reused unchanged for multi-shot effect resume.
- Fiber runtime (`src/async/fiber.{h,c}`, `reactor.c`, `scheduler.c`,
  `atomic_queue.c`) -- stays the default `async` scheduler; F3 adds the alternate
  representation beside it.
- Tree-walking interpreter effect machinery (`src/turi/eval.c`,
  `eval_handle_inner`) -- extended for shallow parity.

## Out of scope

- Source-level surface changes to `defeffect`, effect rows, or effect
  polymorphism. `handle-shallow` is a new *control* form, not a change to the
  effect *declaration* surface.
- Removing the `ucontext` fiber code -- F3 is an alternate representation behind
  a flag, selected per unit/function; the fiber scheduler stays default.
- Cross-thread continuation semantics beyond what the DK substrate provides:
  F3.2 hands a kont to exactly one thread at a time (ownership transfer), it does
  not add shared concurrent invocation of a single continuation.
