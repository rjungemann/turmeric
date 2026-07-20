---
title: CPS backend -- refcounted continuation-env teardown (E3), the owning-capture substrate
category: Planning
status: GRADUATED 2026-07-20 -- owning capture into a multi-shot cloneable continuation is now unconditional (the `owning-cloneable-capture` experiment retired; `g_opt_owning_cloneable_capture` defaults true, name in GRADUATED[]). Every channel (borrow / owner-drop via explicit + auto-defer) and every owning shape whose deep clone is leak-clean in the base language is covered and LeakSanitizer-clean: rc, `:heap` carrier handles (flat / rc-weak-fielded / ref<scalar>-fielded), and multi-word by-value aggregates -- both borrow and consuming. The only rejections left (a ref<owning> or nested-aggregate field, consumed) are bounded by the base language's SHALLOW drop_glue, not by this feature -- making them clean requires first deepening the base drop. See the landed-history sections below.
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

## E3b auto-defer lowering -- design sketch (2026-07-19, IMPLEMENTED)

> Landed via the P5b trailing-defer extension in `cps_tail`'s `do`-lowering
> (`cps_ir.c`), gated on the experiment; fixture
> `cloneable-owning-autodrop-crossing`. The sketch below is the rationale.

Scoped after landing the explicit-drop channel. The remaining ergonomic gap is
narrow and tractable -- it is NOT the full "arbitrary `defer` in a CPS body"
feature. The target is exactly one shape: the **auto-inserted scope-exit drop**
of an owning local captured `^borrow` across a cloneable-reset,
`(defer (rc/drop r))` / `(defer (drop! r))`, appended to the body `do` by the
elaborator (`elab_forms.c:1163` / `:1050` / `:1353`).

Why it is small (from the EX_DEFER map):

- A **plain** `(rc/drop r)` already lowers fine on the CPS path -- `safe_to_delegate`
  admits `EX_RC_DROP` (`cps_ir.c:1714`) so it delegates to a `CT_LETRAW` the
  direct emitter drops. The SAME drop wrapped in `EX_DEFER` evicts *only* because
  `safe_to_delegate`'s `EX_DEFER` arm (`cps_ir.c:1760`) gates on
  `g_whole_body_delegate`. The explicit-drop channel already proves the unwrapped
  `CT_LETRAW`-in-the-continuation shape is admissible and leak-clean.
- The elaborator's auto-drop defer is already recognized structurally:
  `autodrop_defer_owning` (`cps_ir.c:2112-2131`) matches `(defer (rc/drop X))` /
  `(defer (drop! X))` and returns the discharged root local. It already backs the
  BEFORE-control hoist (`cps_emit_hoisted_drops`, spliced at `cps_ir.c:3401`).

**Approach: unwrap the auto-drop defer into a straight-line drop after the
cloneable-reset**, under the experiment gate, for exactly this shape:

- In `cps_bind`/`cps_tail`, when the tail/bound form is a `(defer D)` whose body
  is an `autodrop_defer_owning` root that is captured `^borrow` into an enclosing
  gated cloneable-reset, lower `D` as its inner drop (the same `CT_LETRAW` an
  explicit `(rc/drop r)` produces) threaded into the continuation body -- i.e.
  treat the auto-defer identically to the explicit drop that already works.
- Equivalently at emit level, the agent found a clean region-completion seam in
  `emit_cloneable` at `emit_cps_ir.c:6389-6391`: after `xn = dk_run(dv,0);
  dk_free(dv);` and before `emit_term(body)`, `xn` is a live local and `dv` is
  freed -- a straight-line `ce_line` drop there is exactly the defer semantics
  (region completes -> drop -> deliver). Prefer the CT-IR unwrap (reuses the
  proven explicit-drop path); the emit seam is the fallback if CT-IR unwrap is
  awkward.

Soundness is identical to the landed explicit-drop channel: the frame only
BORROWS the rc (never drops it), the body between reset and scope exit is
straight-line, and the drop runs once after a normally-completing reset. General
`defer` (multiple defers, user `(defer ...)`, LIFO ordering, abortive-exit
firing, non-cloneable regions) stays out of scope -- that is the genuinely large
"CPS defer runtime" feature and is not needed for E3's owning-capture goal.

Not applicable to `emit_reset` (plain CT_RESET): its continuation is lifted
behind the DK chain (`emit_cps_ir.c:5860/5881`) with no inline post-completion
seam, so a defer there must thread at the CT-IR level (as the existing
`cps_ir.c:3409-3462` do-with-control-op defer path already does). The cloneable
path is the one E3 needs.

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
- **The TUR-E0710 owning-autodrop-crossing-a-cloneable-reset case.** An `rc` the
  enclosing fn OWNS, captured across a `cloneable-reset` and dropped after it.
  This splits into two tiers (both pinned 2026-07-19):
  - **Explicit drop -- LANDED.** With an explicit `(rc/drop r)` on the
    straight-line path after the reset, the drop lowers as a plain `CT_LETRAW`;
    granting `CT_CLONEABLE` the "no longer has to precede the control op" pass in
    `owning_dropped_before_control` admits it. Sound because the E3a admission
    only lets the frame BORROW the rc (never dropped inside the multi-shot
    continuation) -- the owner drops it once, after a reset that completes
    normally. Fixture `cloneable-owning-explicit-drop-crossing` (leak-clean).
  - **Auto-inserted drop -- LANDED.** The elaborator auto-inserts the scope-exit
    drop as `(defer (rc/drop r))`, APPENDED after the real body of the enclosing
    `do`. The `cps_tail` `do`-lowering's P5b defer-threading already threads a
    control-free defer body into the block continuation (fires after the value,
    before delivery, LIFO) -- it only bailed when the value item was not the last
    item (a defer tail). Extended (under the gate) to use the last NON-defer item
    as the value and thread the trailing defer, so the auto-drop lowers to the
    same straight-line `CT_LETRAW` drop the explicit channel emits -- no full
    "CPS defer runtime" needed. Fixture `cloneable-owning-autodrop-crossing`
    (leak-clean, no hand-written drop). Off-gate a defer tail still bails, so
    every non-experiment snapshot is byte-identical. (The `cps_bind` `do`-path
    uses the O1-b hoist instead of P5b; a bind-position owning-autodrop-crossing
    is a contrived shape, not yet wired -- a small follow-on if it surfaces.)
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

**Codegen -- first channel landed (borrow-capture).** An owning `rc` **borrowed**
across a genuinely multi-shot cloneable continuation now compiles, native-lowers,
and runs leak-clean under the gate. Fixture: `cloneable-owning-borrow-capture`
(a `^borrow rc` captured into `(read-combine [] r)`, resumed twice via clone +
original -> `32`, LeakSanitizer-clean). It took relaxing three gates, all under
`g_opt_owning_cloneable_capture`:

1. **TUR-E0014** (`check_cloneable_capture_precise`, elab_effects.c) -- admit an
   owning `rc` capture (no Clone instance) instead of rejecting; emits W0060.
2. **The 2-arg-frame scalar-env gate** (`build_marshal_reset`, cps_ir.c ~1176) --
   admit an owning `rc` env operand when the callee takes it `^borrow`
   (`FN_ARG_FLAG(..., FA_BORROW)`, gated on the operand type since an rc param's
   kind is reliable only on the operand). TUR-E0710 otherwise.
3. **The `n_live_captures` Shape-2 gate** (cps_ir.c ~1384) -- a blunt "any live
   local" reject; dropped under the gate because the frame loop having succeeded
   means every continuation-referenced local is a validated carried frame operand.

Why no new clone/drop glue was needed for this channel: `^borrow` guarantees the
frame never drops the rc, so the existing shallow-shared frame env is
read-only-correct across resumes and the owner (the caller) drops it exactly
once. The `dk_frame_owning` substrate is reserved for the *consuming* case.

**Remaining scope.** An owning value the **enclosing fn owns** and drops after the
cloneable-reset now works both ways: with an explicit `(rc/drop r)` and -- via the
P5b trailing-defer extension -- with the auto-inserted scope-exit drop (no
hand-written drop). The owning KIND is also widened beyond `rc` to EVERY owning shape a ^borrow
capture can carry:
- **One-word handles** -- `rc<T>` and a `:heap` ADT / struct carrier handle ride
  the frame env by a bare pointer copy (fixture
  `cloneable-owning-carrier-handle-capture`).
- **Multi-word by-value aggregates** -- a by-value struct/ADT product with an
  owning field (`needs_drop_glue`) does not fit the one-word env by value, so it
  rides by ADDRESS: the emit carries `&o` (o is the owner's stable by-value
  local; the non-serial reset runs `dk_run` synchronously in the owner's frame,
  so the pointer outlives every resume) and the frame fn derefs it
  (`*(tur_adt_X *)env`, or `(const tur_adt_X *)env` for a >16B pass-by-ptr param).
  No clone glue: the ^borrow frame only reads it, and the owner's existing
  per-field scope-exit drop -- threaded by P5b -- releases the fields once
  (fixture `cloneable-owning-aggregate-capture`). Admission requires an ATOMIC
  operand (a bare local, so `&o` is well-formed); a computed aggregate is
  rejected. Non-serial only -- a serialized continuation resumed after the frame
  returns would dangle the address.

Because the borrow + owner-drop channels never drop the value inside the frame,
NONE of them need per-frame clone glue.

- **CONSUMING case (rc) -- LANDED.** A frame that DROPS its captured rc once per
  call (a non-^borrow rc param) is the shape the `dk_frame_owning` substrate was
  built for. It rides `dk_frame_owning` with an emitted per-frame `env_clone`
  (`rc_strong_increment`) and NO `env_drop`: each `dk_copy_node` resume copy
  increfs its own +1, which the frame's own drop balances; the owner's base +1 is
  released once by its P5b-threaded scope-exit drop. Accounting nets to freed
  exactly once (fixture `cloneable-owning-consuming-capture`, LSan-clean).
  build_marshal_reset admits it at both the param and operand env checks.
- **CONSUMING carrier handle -- LANDED.** A frame that consumes a FLAT `:heap`
  carrier handle (no owning fields) rides `dk_frame_owning` with a DEEP-COPY
  `env_clone`: `malloc` a fresh header + shallow field copy, so each resume frees
  its OWN allocation (a heap handle is not refcounted -- a shared free would
  double-free). The bare header type is `emit_type_c_name` minus the trailing
  ` *`, so `sizeof`/copy cover the whole header, not just the first word (fixture
  `cloneable-owning-consuming-carrier`, two fields, LSan-clean).
- **CONSUMING carrier handle with owning fields (recursive clone) -- LANDED.** A
  heap handle whose fields are plain values and increfable owning handles (rc /
  weak) is consume-cloneable: the `env_clone` deep-copies the header AND increfs
  each owning field -- the mirror of `drop_glue`'s decref, so the copy owns its
  own +1 that its free (drop_glue) balances. The incref loop walks the ctor
  fields (`adt_field_member_path`); a flat handle gets an empty loop. Restricted
  to rc/weak owning fields: a ref/lref or nested aggregate / heap-handle field
  needs a genuinely recursive VALUE clone (not an incref) and is rejected, never
  miscompiled (fixture `cloneable-owning-consuming-recursive`, an rc-fielded
  `:heap` struct, LSan-clean).

- **CONSUMING by-value aggregate -- LANDED.** A multi-word owning by-value
  aggregate consumed by a non-^borrow callee. It already rides the env by ADDRESS
  (`&o`) and is passed BY VALUE to the callee, which drops its OWN copy each call.
  So the `env_clone` increfs the aggregate's owning (rc/weak) fields via `&o` per
  resume -- balancing the callee-copy's drop -- and returns the SAME `&o` (no
  allocation: the C by-value copy the call makes IS the per-resume copy; the
  owner's `o.r` is freed once by its P5b-threaded scope-exit drop). Same rc/weak
  field restriction as the recursive carrier (fixture
  `cloneable-owning-consuming-aggregate`, LSan-clean).

- **CONSUMING heap handle with a `ref<scalar>` field (recursive clone) -- LANDED.**
  A ref field is an owning box (`drop_glue` `free`s it), so a multi-shot resume
  freeing a shared box double-frees.  The `env_clone` deep-copies the box -- malloc
  a fresh box + scalar copy, then store it back (the field is `void *`, so cast to
  the inner pointer type to deref and to `void *` to store).  This is exactly as
  deep as `drop_glue`'s `free` -- the clone MIRRORS the drop.  Fixture
  `cloneable-owning-consuming-ref-field`, LSan-clean.

**Where the recursion stops -- and why it is the right boundary.** A clone must
mirror `drop_glue` exactly, and `drop_glue` is SHALLOW: for a `ref`/`lref` field
it just `free`s the box (no recursion into the pointed-to value), and it has NO
case for a nested `TY_ADT`/`TY_STRUCT` field at all.  So the cleanly-synthesizable
clone goes exactly one level for a `ref<scalar>` (fresh box) and no deeper:
- a `ref<OWNING>` field -- the base drop frees the box but leaks the inner owning,
  so a matching clone would only reproduce that base leak, not a clean deep copy;
- a nested aggregate / heap-handle field -- the base drop does not drop its owning
  fields at all (leaks), so again a matching clone cannot be leak-clean.
Both stay cleanly rejected (E0710), never miscompiled.  Making them leak-clean
would require first deepening the base language's `drop_glue` to recurse -- a
separate base-language change, out of scope for E3.  With this, E3's
owning-capture goal is complete for every shape that is leak-clean in the base
language.  Remaining: GRADUATION (retire the experiment gate at the 0.31.0
`expires_at`).
(A.2/A.3 of the capture-channel map -- those kinds still evict). Note from that
map: serial can't carry owning values and the shift-receiver env is single-shot,
so the non-serial `CloneFrame` env is the only multi-shot owning channel.

The `rc<int>` annotation footgun found during this work (angle brackets become a
tyvar; use bare `rc`) is filed separately:
[docs/reported/rc-angle-bracket-annotation-becomes-tyvar.md](../reported/rc-angle-bracket-annotation-becomes-tyvar.md).

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
