---
title: CPS backend -- refcounted continuation-env teardown (E3), the owning-capture substrate
category: Planning
status: NOT STARTED -- shelved-by-default. This is the design of record for the day a consuming / abortive owning capture into a MULTI-SHOT continuation must be ADMITTED rather than rejected. It is not on the critical path: every reachable owning capture that compiles today is already leak-clean by cheaper means (E-borrow bare aliasing + E1 incref-on-read-out + the P2 auto-drop lowering), and the one residual shape is a clean compile-time error (TUR-E0107), not a miscompile or a leak. Do not build it to "fix a leak" -- there is no owning-payload leak in the multi-shot env today. Build it only to widen the admitted-shape surface. Split out of cps-backend-multishot-continuations-owning-capture-plan.md (archived) 2026-07-19, which landed everything reachable without this substrate (Tracks A + B E1/E2/E-borrow + E4a).
description: The refcounted-env clone/drop teardown (Option B) for owning values captured into a genuinely multi-shot continuation. Gives a lifted continuation frame's env a real clone (deep-copy + per-owning-field incref/clone-glue on each dk_copy_node resume) and drop (per-owning-field decref/drop-glue + free on dk_free), emitted ONLY when the capture set holds an owning field. Two phases: E3a adds the per-frame hooks (admits consuming multi-shot captures, base +1 still leaks); E3b adds the delimited-region teardown (leak-clean, retires the base leak). This is the substrate E4's genuinely-owning multi-shot capture, the TUR-E0107 consuming-aggregate admission, the TUR-E0710 owning-autodrop-crossing-a-cloneable-reset case, and O1-b P2/P3 all ride.
---

# CPS backend -- refcounted continuation-env teardown (E3)

> **Status: shelved-by-default, do-not-build-to-fix-a-leak.** Verified against
> the tree 2026-07-19: no `dk_frame_owning`, no per-frame `env_clone` /
> `env_drop` hook, no `dk_free_deep`; the only `__dk_env_clone` / `__dk_env_drop`
> in `emit_dk_runtime.c` are the spine-only `dk_copy_range` / `dk_free` pair that
> predate E3. It stays unbuilt on purpose. Leak-cleanliness for every reachable
> owning capture is already achieved without a teardown, and the one residual
> (a consuming multi-field aggregate capture) is a clean hard error, not a
> miscompile. This document is the design of record for the day that residual
> must be **admitted** rather than **rejected**.

## What E3 is

A **real teardown for an owning value captured into a genuinely multi-shot
continuation** (a continuation resumed more than once). Give a lifted
continuation frame's environment a real clone/drop pair, emitted **only when the
capture set contains an owning field** (Copy-only envs keep today's leaked-DK-node
fast path untouched):

- **clone** -- run when `dk_copy_node` / `dk_copy_range` copies the spine for a
  multi-shot resume: deep-copy the env struct and run each owning field's clone
  glue (`rc_strong_increment` for an rc handle; the O2 struct/ADT clone glue for
  an aggregate). Copy fields stay a shallow copy.
- **drop** -- run when the DK node / cont is freed: run each owning field's drop
  glue, then `free` the env.

## Why it is split out (and why it is not urgent)

E3 previously had a standalone plan (`cps-backend-owning-env-teardown-e3-plan.md`),
was folded into the multishot-continuations plan's E3 section, and is now split
back out because that parent plan **landed everything reachable without it** and
has been archived. The parent's own conclusion, re-verified:

- A **borrow-only** owning capture rides a bare shallow alias (E-borrow); the
  enclosing fn owns the value and drops it once. Leak-clean, no teardown.
- A **consuming rc** capture is leak-clean via E1's incref-on-read-out balanced
  by the case's own drop, with the base dropped by the P2-lowered auto-drop.
- An **owning aggregate borrow** capture is single-shot at the drop (P2) and
  needs no teardown (E2, leak-clean, no `requires.no-leak-check`).
- The one shape that genuinely needs the teardown -- a **consuming multi-field
  aggregate** capture (a handler case that DROPS a captured struct's owning
  field, with no per-field incref to balance it) -- is a **hard error,
  TUR-E0107** (`is_field_consumed_in_handler` + `elab_forms.c`), not a silent
  double-drop.

So E3 is the enabler for *admitting* consuming / abortive owning multi-shot
captures, not a prerequisite for leak-cleanliness of anything that compiles
today. **There is no owning-payload leak in the multi-shot env to fix.** (The
separate, still-open `docs/reported/cps-drop-elided-under-delimited-control.md`
-- a returned heap ADT whose scope-exit drop is elided when its fn also contains
delimited control -- is a drop-insertion/CPS-threading bug in a different pass;
E3 does **not** fix it. See Out of scope.)

## What building E3 would unlock (expressiveness)

Multi-shot continuations back **backtracking, nondeterminism, cloneable
generators, and speculative / re-entrant control**. Today those work only over
Copy scalars and borrow-only reads across the capture point. E3 lets each
resumption carry its **own correctly-refcounted copy of a mutable owned heap
resource**:

- backtracking search whose per-branch partial solution is an owned `Vec` /
  board / route each branch mutates independently;
- cloneable generators/iterators that hold an `rc` to the structure they walk
  (each clone increfs);
- speculative / transactional "try this world, else that" over owned working
  state;
- web/workflow continuations (`serial-reset`) resumed multiple times owning
  session state.

Note the honest scoping: **immutable / persistent** structures (HAMT map,
persistent list, `Fix`/`Free`) already cross a multi-shot boundary via
borrow/alias and need no teardown; the owned value can also be **threaded
explicitly** through `resume k v` today. E3 buys **direct-style ergonomics for
implicitly-captured mutable owned state**, not a category of otherwise-impossible
computation. That is why it is expressiveness, not correctness.

## The mechanism today, and why a naive `free(env)` is unsound

On a multi-shot resume `dk_copy_node` (`src/runtime/cps_prompt.c:81`)
**shallow-copies the env pointer** (`c->env = n->env`), so every reified sub
shares one env with the leaked original chain, and `dk_free` (`:147`) runs only
on those reified sub copies -- never on the original chain. So the env is
*shared* (a naive free double-frees) and the original chain is *never freed* (any
owning ref it holds -- the "base" +1 -- leaks without a teardown). A build
therefore splits into two phases.

## Design of record -- Option B (refcounted env)

Two wiring options; pick during E3a:

1. **Reuse `tur_cloneable_cont`.** Route the owning-carrying frame through
   `tur_cloneable_cont_alloc(fn, cap, <env_clone>, <env_drop>)` (as the
   cloneable-shift path already does at `emit_cps_ir.c:3599`), emitting a
   per-continuation `<hname>_env_clone` / `<hname>_env_drop` that the callbacks
   dispatch to.
2. **Give `dk_frame` env clone/drop hooks + a `dk_free` teardown.** Add optional
   `env_clone` / `env_drop` fn pointers to the `DK` node (default NULL = today's
   leaked behavior); `dk_copy_node` runs `env_clone`, `dk_free` runs `env_drop`.
   More invasive but more uniform; this is what `__dk_env_clone` /
   `__dk_env_drop` (`emit_dk_runtime.c:57-60`) would grow into.

Either way, extend `__dk_env_clone` / `__dk_env_drop` (which today only
`dk_copy_range` / `dk_free` the spine) to also clone/drop the owning payload
inside each frame's env, reusing E-borrow's `CapSet.owning[]` and O2's
struct/ADT clone+drop glue (keyed by `type_uses_carrier_abi` /
`adt_is_byvalue_product`).

## Phasing

### E3a -- per-frame env clone/drop hooks (admits consuming captures; still leaks the base)

Give a DK frame an optional `env_clone` / `env_drop` pair (default NULL = today's
leaked, share-on-copy behavior), fired in:

- `dk_copy_node` -- deep-copy the env + clone each owning field, so each reified
  sub gets its own +1;
- `dk_free` -- drop each owning field, then free the env.

Emit an `<hname>_env_clone` / `<hname>_env_drop` per continuation whose caps hold
an owning field (reuse E-borrow's `owning[]` + O2's clone/drop glue) and pass
them to a new `dk_frame_owning(...)` constructor at the `emit_cont_env` site.

This makes a **consuming** multi-shot capture memory-safe (copies no longer
alias), **but the base env's +1 still leaks** (the original chain is never
freed) -- so an E3a-era fixture carries `requires.no-leak-check`, exactly like
E1's original incref path. That is acceptable for E3a: the goal of E3a is
*admission + no double-free*, not leak-cleanliness.

### E3b -- delimited-region teardown (leak-clean; retires the DK-node leak)

Free the original delimited chain (and, via `env_drop`, its base env refs) at
region completion. The hard part: the region is emitted **tail-recursively**, so
it needs one of:

- a **non-tail region** -- bind the `dk_run` result, then `dk_free_deep` the
  original chain, then deliver to `cur_k`; or
- a **per-region arena** freed at region end.

Either also retires the pre-existing DK-node leak (already archived as
`docs/archive/cps-delimited-dk-node-leak.md`) as a bonus. Once E3b lands, drop
`requires.no-leak-check` from the E3a fixtures.

### Correctness obligations (both phases)

- **Fire the base `env_drop` exactly once.** Accounting: base populate = +1;
  each sub copy = +1 clone and -1 `dk_free`; region teardown = -1 base; net zero,
  freed once.
- **Fire the teardown on abortive control too.** An abortive `shift` discards the
  continuation -- the region-end teardown must still run. The arena option makes
  this uniform. (Same obligation O1-b P2's "fire on abandon" carries.)

## What rides E3 (the consumers to re-admit once it lands)

- **E4 proper -- genuinely-owning multi-shot capture.** The `rc` handle (or
  owning aggregate) riding the multi-shot env, incref'd per `dk_copy_node` clone
  and decref'd per `dk_free`. Fixture target: a generator/step `shift` resumed
  twice capturing an `rc`; a handler case that captures an owning `rc` and runs
  twice with a clean strong count (the Track A + E3 capstone). Currently evicts /
  hits the cloneable grammar.
- **The TUR-E0107 consuming-aggregate admission.** The consuming multi-field
  aggregate capture (`is_field_consumed_in_handler`) becomes *admitted* with a
  real env drop instead of the current hard error.
- **The TUR-E0710 owning-autodrop-crossing-a-cloneable-reset case.** An `rc` live
  across a `cloneable-reset` and dropped after it currently evicts (TUR-E0710):
  the owning-autodrop-lowering (P2) covers only the single-shot `handle`
  crossing, not the multi-shot cloneable-reset crossing. The multi-shot crossing
  drop is exactly an env-owned reference dropped at region teardown -- E3b.
- **O1-b P2 / P3** ([cps-backend-ref-scope-exit-drop-plan.md](cps-backend-ref-scope-exit-drop-plan.md)):
  P2 (abortive-unwind ref drop) rides E3b's region teardown; P3
  (resumable-crossing ref) is the `ref`-flavored instance of the owning capture
  E3a admits.

## Experiment-gate note

If E3a is built as an in-flight feature (it *admits shapes that are currently
rejected*, i.e. a surface-semantics widening), follow the repo's experimental-
features rule: add a row to `EXPERIMENTS[]` in `src/runtime/experiments.c` with
every descriptor field populated and a `g_opt_<name>` the admission reads, call
`experiment_warn_if_used` from the admission entry point, and point `plan_path`
at this file. Graduate (delete the row, feature always-on) once E3b lands and the
`requires.no-leak-check` markers are gone. A pure internal-codegen teardown that
does not by itself admit new shapes does not need the gate.

## Out of scope

- **`docs/reported/cps-drop-elided-under-delimited-control.md`** -- a returned
  heap ADT (e.g. a `Vec`) whose scope-exit drop is elided when its owning fn also
  contains delimited control (`handle`/`resume`, presumably `reset`/`shift`).
  That is a **drop-insertion / CPS-threading bug** (the single-owner return drop
  is lost across CPS coloring), NOT the multi-shot env-payload teardown. It is a
  real, reachable leak (keeps `requires.no-leak-check` on
  `cps-backend-heap-adt-return`) and is the higher-value, far cheaper target if
  the goal is stopping a leak -- but it is fixed in the drop-insertion pass, not
  here. Do not conflate it with E3.
- **Recursive / unbounded suspension continuations** (a `cps->cps` tail call in a
  continuation body) -- a separate trampolined-heap-continuation plan; tracked
  for `await` in `docs/reported/cps-async-recursive-await-eviction.md`.

## Implementation status (2026-07-19)

Landed on `claude/cps-backend-multishot-continuations-yidyhq`:

- **Runtime substrate** -- `DKEnvClone`/`DKEnvDrop` hooks + `dk_frame_owning` on
  the standalone DK machine (`cps_prompt.c`), sanitizer-proved by two new
  `cps_prompt_unit.c` tests (per-copy clone, per-free drop, net-zero across a
  multi-shot resume; base drop fires exactly once).
- **Emitted-runtime mirror** -- the same hooks in the DK prelude compiled into
  programs (`emit_dk_runtime.c`); NULL-default, byte-identical behavior, 139
  fixture snapshots regenerated.
- **Experiment gate** -- `owning-cloneable-capture` (`g_opt_owning_cloneable_capture`,
  `EXPERIMENTS[]` row, `--enable=` accepted), default off and inert.

**Codegen is blocked** on a pre-existing type defect:
[docs/reported/rc-param-generalizes-to-tyvar.md](../reported/rc-param-generalizes-to-tyvar.md).
An owning `rc` reaches a cloneable continuation frame only through a user
function with an `rc<T>` parameter, and such a parameter generalizes to a bare
tyvar -- so rc builtins hard-error on it and `build_cloneable` sees `TY_TYVAR`,
not `TY_RC`. The elab admission (relax TUR-E0014 under the gate) and the
`build_cloneable` owning-env relaxation (operand-gated + `^borrow`-gated, the
sound borrow subset) were prototyped and reverted pending that fix. Once a
`rc<T>` parameter resolves to `TY_RC`, re-apply them and wire `dk_frame_owning`
+ the per-frame rc clone/drop glue at the `emit_cloneable` Shape-2 frame push
(`emit_cps_ir.c` ~6349-6363). Note (from the capture-channel map): serial can't
carry owning values, and the shift-receiver env is single-shot -- the only
multi-shot owning channel is this non-serial `CloneFrame` env.

## Depends on / reuses

- `tur_cloneable_cont_alloc(fn, cap, clone, drop)` (`emit_cps_ir.c:3401`, `:3599`).
- `__dk_env_clone` / `__dk_env_drop` (`emit_dk_runtime.c:57-60`), today spine-only.
- `dk_copy_node` / `dk_free` (`cps_prompt.c:81`, `:147`); the shallow env-pointer
  copy is what E3a's clone hook replaces.
- `CapSet.owning[]`, `cap_owning_ok` (`emit_cps_ir.c:954`), `emit_cont_env` --
  the capture-set machinery that already knows which fields are owning.
- `rc_strong_increment` / `rc_strong_decrement`; O2 struct/ADT clone+drop glue
  keyed by `type_uses_carrier_abi` (carrier ADT) and `adt_is_byvalue_product`
  (by-value product with owning fields).
- The TUR-E0107 hard-fail (`is_field_consumed_in_handler` + `elab_forms.c`) --
  the admission point E3 relaxes.
