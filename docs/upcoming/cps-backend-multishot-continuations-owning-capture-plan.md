---
title: CPS backend -- multi-shot continuations and owning-value env capture (Tracks A + B)
category: Planning
status: Track A COMPLETE (A1-A3). Track B: E-borrow landed (leak-clean owning captures via bare aliasing -- E1 is now leak-clean); E2 (aggregate/carrier captures) BLOCKED on the scope-exit-auto-drop hole (rides E3/O1-b); E3/E4 open. Supersedes cps-backend-env-capture-owning-values-plan.md (E1 landed).
description: Two orthogonal CPS-backend coverage features, split out and detailed after landing E1 of the env-capture story. Track A -- a lifted continuation body may contain a NESTED control op (perform/handle/shift), not only straight-line code; this is what a two-perform body ("resumed twice") needs, and it has a proven template in F3's async/await gap-2 (lift the continuation as LH_RESET_CONT so a nested suspension threads the enclosing k). Track B -- finish the env-capture story so an owning value captured into a genuinely multi-shot continuation is cloned/dropped correctly and leak-clean (E2 aggregates, E3 the Option B refcounted-env teardown, E4 reset/shift), and resolve the consuming-case EX_DEFER interaction. Neither is a correctness gap today (the whole-function fallback is sound); both are missed coverage that N6.5 (fallback deletion) needs covered.
---

# CPS backend -- multi-shot continuations and owning-value env capture

## Why this document exists / what it supersedes

This plan replaces
[cps-backend-env-capture-owning-values-plan.md](../archive/cps-backend-env-capture-owning-values-plan.md)
(archived), whose **E1 landed**: a borrow-style `rc` capture into a multi-shot
handler case now CPS-emits via Option A clone-on-read-out
(`cap_owning_ok`, `CapSet.owning[]`, `rc_strong_increment` in `emit_lifted`;
fixture `cps-backend-owning-capture-handler-case`, `requires.no-leak-check`).

Landing and validating E1 surfaced two facts that reshape the remaining work:

1. **A true runtime "resumed twice" program is not CPS-emittable** -- not for a
   capture reason, but because a lifted continuation body may not contain a
   NESTED control op. A two-`perform` body (`(let [a (perform E)] (let [b
   (perform E)] (+ a b)))`) evicts. This is a *subset-shape* gap, orthogonal to
   owning captures, and it turns out F3 already shipped the exact machinery to
   close it (for `await`). **Track A.**

2. **The env-capture story's remaining phases (E2-E4)** -- aggregate/carrier-ADT
   captures, the leak-clean Option B refcounted-env teardown, and reset/shift
   multi-shot -- are still open, and the "case *consumes* the capture" shape the
   original E1 narrative described is blocked upstream by an `EX_DEFER`
   interaction that Option B (E3) is the right place to resolve. **Track B.**

The two tracks are **orthogonal but complementary**: Track A produces the
runtime double-invocation of a handler case; Track B makes that invocation
memory-safe when the case captures an owning value. A genuine
"handler case captures an owning value and runs twice" capstone fixture needs
both.

**Nothing here is a correctness gap today.** Every shape both tracks reject is
handled correctly by the general whole-function fallback. This is coverage, and
it is what N6.5 (the general fallback deletion,
[cps-backend-n6-fallback-removal-followups-plan.md](cps-backend-n6-fallback-removal-followups-plan.md))
needs covered before it can delete the fallback without hard-erroring these
shapes.

## Substrate that already exists (verified)

- **Multi-suspension continuation lowering (F3 gap-2).**
  `await_cont_reset_ok` (`src/compiler/emit_cps_ir.c:1079`) admits a *bounded*
  full-CPS `await` continuation (a branch, or a further sequential `await`), and
  `emit_await`'s gap-2 branch (`:3877-3900`) lifts it as **`LH_RESET_CONT`**:
  the frame carries the enclosing continuation `k` in its env (via
  `emit_cont_env`, `k_expr = cur_k`), its `next` is `dk_done()`, and inside the
  helper `cur_k` is `"__kont"` (read from `env->__k`). A nested `await` then
  threads `__kont` (`dk_shift(..., dk_frame(inner, env, __kont))`) and a
  `KK_RET` appcont emits `dk_run(__kont, v)`. Proven by
  `tests/fixtures/async-await-cps-two` (`(+ (await A) (await B))`) and
  `async-await-cps-repark` (`(let [x (await A)] (let [y (await B)] (+ x y)))`),
  both CPS-emitted. The bound: a `cps->cps` tail call (recursive/unbounded
  suspensions) is rejected (`docs/reported/cps-async-recursive-await-eviction.md`).

- **Refcounted env with clone/drop callbacks.**
  `tur_cloneable_cont_alloc(fn, cap, clone, drop)` (used at
  `emit_cps_ir.c:3401` and `:3599`) is a refcounted continuation with per-clone
  / per-drop callbacks. The cloneable-shift path already threads an owning
  capture through `tur_cloneable_cont_alloc(__dk_cont_fn, __cap, __dk_env_clone,
  __dk_env_drop)`. The callbacks (`emit_dk_runtime.c:54-60`) currently clone/free
  the DK **spine only** (`dk_copy_range` / `dk_free`), not the owning payload in
  a frame env -- that is exactly the gap Option B fills.

- **DK runtime.** `dk_copy_node` (`src/runtime/cps_prompt.c:81`) shallow-copies
  the `env` pointer, so every multi-shot resume shares one env. `dk_free`
  (`:147`) frees nodes with **no env teardown**. Option B adds the teardown.

- **Owning clone/drop glue.** `rc_strong_increment` (incref) /
  `rc_strong_decrement` (decref), and the generated struct/ADT clone+drop glue
  keyed by `type_uses_carrier_abi` (carrier ADT) and `adt_is_byvalue_product`
  (by-value product with owning fields) that O2 already emits.

---

## Track A -- multi-suspension continuations (nested control op in a lifted body)

### The gap (verified)

Empirically the boundary is sharp (`TUR_TRACE_EVICT=1`):

| shape | result |
| --- | --- |
| `(+ (perform E) 1)` -- one perform, straight-line continuation | CPS-emits |
| `(let [a (perform E)] (perform E))` -- perform whose continuation performs | evicts |
| `(let [a (perform E)] (let [b (perform E)] (+ a b)))` | evicts |
| `(+ (handle ...) (perform E))` -- nested effect under a k-carrying frame | evicts |

The invariant enforced today: **a lifted continuation body must be straight-line
-- no nested control op.** Gated by `perform_body_ok`
(`emit_cps_ir.c:1034`), `handle_case_ok` (`:1111`), and `shift_body_ok`
(`:997`), each of which falls through to `default: return false` on a nested
`CT_PERFORM` / `CT_HANDLE` / `CT_SHIFT`. The `CT_PERFORM` classification
(`:1377`) and `emit_perform` (`:3753`) lift the continuation only as
`LH_PERFORM_CONT` -- a value-transform frame `(env, xval)` that returns a value
`dk_run` delivers to `next`. That frame cannot host a suspension: it has no
downstream continuation to thread and the wrong role (return-a-value, not
re-enter the prompt machinery). This is why the reset-style lift (which *does*
carry `k`) is the vehicle.

### Design -- generalize F3 gap-2 from `await` to `perform`/`handle`/`shift`

The change mirrors, almost line-for-line, what `CT_AWAIT` (`:1390`) and
`emit_await` (`:3877`) already do:

1. **A bounded full-CPS continuation predicate.** Generalize
   `await_cont_reset_ok` into a shared `cont_reset_bounded_ok(t)` that recurses
   through the straight-line arms (`CT_LETVAL/LETPRIM/LETCALL/LETRAW/CT_IF`) and
   through the nested control arms `CT_PERFORM` / `CT_HANDLE` / `CT_SHIFT` /
   `CT_AWAIT`, requiring each operand slot-representable and rejecting a
   `cps->cps` tail call (`CT_TAILCALL` to a colored callee) -- the same bound
   `await_cont_reset_ok` already imposes to keep suspensions statically bounded
   (no O(N) resume stack).

2. **Relax the classification gates.** In the `CT_PERFORM` (`:1377`), `CT_HANDLE`
   case-body (`handle_case_ok`), and `CT_SHIFT` (`:1341`) admissions, admit a
   continuation that is `perform_body_ok(body) || cont_reset_bounded_ok(body)`
   -- exactly the disjunction `CT_AWAIT` uses at `:1407`.

3. **Lift as `LH_RESET_CONT` on the reset path.** In `emit_perform` /
   `emit_handle` / `emit_shift`, when the continuation is not the straight-line
   value-transform shape but is the bounded full-CPS shape, lift it as
   `LH_RESET_CONT` (carrying `k` via `emit_cont_env(..., cur_k)`, `next =
   dk_done()`) instead of `LH_PERFORM_CONT` -- the `else` branch of `emit_await`
   copied to the perform/handle/shift emitters. A nested control op inside the
   lifted helper then threads `__kont` through its own `dk_perform` /
   `dk_handler` / `dk_shift`, and a `KK_RET`/`KK_PROMPT` appcont delivers to
   `__kont`.

4. **Captures.** The continuation's free locals (including an earlier perform
   result such as `a`) ride the `LH_RESET_CONT` env via the existing
   `collect_caps` (scalar-cvar capable, `cap_add_cvar`). Owning captures on this
   path stay single-shot per invocation (each frame runs at most once per
   resume); Track B governs the genuinely multi-shot handler-case env.

### Handler-reachability check (the one real risk)

When a nested `perform` inside an `LH_RESET_CONT`-lifted continuation runs, it
searches its continuation argument's chain for a handler of its tag. That
argument is `dk_frame(inner, env, __kont)`, and `__kont` is the enclosing
continuation captured at the *outer* control-op site -- which for a handled
body is the handler chain (`__h<id>`, `emit_handle:3675`). So the handler stays
reachable. **Verify** this holds across the lift for: a nested `perform` handled
by an *enclosing* handler (not the one being installed), a nested `perform`
under a nested `handle`, and the shallow-handler case (`dk_handler_shallow`,
F2). `dk_copy_enclosing_handlers` (`cps_prompt.c:124`) is the F2 mechanism that
keeps enclosing handlers reachable across a shallow resume -- confirm the reset
lift composes with it.

### Phases

- **A1 -- perform continuation containing a further perform. LANDED.** The
  two-perform body. Fixture `cps-backend-two-perform`
  (`g = (let [a (perform E)] (let [b (perform E)] (+ a b)))`, handled by a
  resuming case, output 82, `direct == cps`). Also verified: three sequential
  performs (nested resume-frames compose), an effect with an argument, a branch
  whose one arm re-performs, and no regression to single-perform.

  **Design correction (important -- the plan's "mirror `emit_await` gap-2" was
  too optimistic).** `await` gap-2 works with `next = dk_done()` because it
  shifts to the always-present root prompt. `perform` instead dispatches to a
  *handler* and delivers the handler-case result via `dk_run(H->next, r)`, so a
  nested perform threading `cur_k` would deliver to the post-handle continuation
  *twice*. The correct lowering needed a new runtime capability: a **resume-frame**
  (`DKK_RESUME_FRAME` / `dk_frame_resume`, added to the emitted DK prelude
  `emit_dk_runtime.c` and the reference machine `cps_prompt.{c,h}`) whose fn
  receives its *run-time* downstream chain (`k->next`, which `dk_perform` splices
  to the reinstalled-handler tail) and consumes it (`dk_run_impl` returns its
  result rather than continuing the loop). The perform continuation is lifted as
  `LH_RESUME_CONT` (signature `(env, xval, DK *__kont)`); a plain `KK_RET`
  delivers `dk_run(__kont, v)`, a nested perform threads `__kont`, so the value is
  delivered exactly once. Gate: `perform_cont_reset_ok` + the relaxed `CT_PERFORM`
  classification + a `CT_PERFORM` arm in `collect_caps_rec`; bounded-only (a
  cps->cps tail call still evicts).

  **Shallow-handler fix (A1 exposed a latent bug).** Making a two-perform body
  CPS-eligible cascaded `effect-shallow-handler` into the all-DK path, which
  revealed that shallow outer-propagation was broken: the CT-IR handle lifts the
  enclosing continuation into the continuation frame's *env*, so
  `dk_copy_enclosing_handlers` could not reach an enclosing handler and a
  re-performed effect aborted as "unhandled". Fixed in `emit_handle` by splicing
  `dk_copy_enclosing_handlers(cur_k)` as the shallow handler's continuation-frame
  `next` (transparent on the normal path; reachable for a re-perform). All
  `effect-shallow-*` fixtures pass.

  **Owning-capture capstone is NOT here.** A1 makes a handler case that captures
  an owning `rc` run *twice at runtime* (memory-safe under E1's Option A), but the
  observed strong count then *accumulates* across runs (2 then 3, sum 5) because
  E1 leaks the env clone per read-out. The clean count (4) needs Track B / E3's
  refcounted env. The runtime multi-shot owning-capture fixture rides E3, not A1.
- **A2 -- handle continuation containing a nested effect. LANDED.**
  (`(+ (handle ...) (perform E))`, the p4 shape.) Fixture
  `cps-backend-handle-cont-perform` (output 15, `direct == cps`). Turned out to
  need **only** a `CT_PERFORM` arm in `joins_closed_rec`: the handle continuation
  is already lifted as a k-carrying reset frame (`LH_RESET_CONT`) at the base of
  the handler chain (`next = dk_done()`), so a nested perform inside it threads
  the enclosing `__kont` and dispatches with no double delivery -- the resume-frame
  machinery from A1 was not needed here (it kicks in only for the *value-transform*
  perform-continuation position). `term_core_ok` already admitted the nested
  perform (A1); the reset/handle continuation just had to pass the joins-closed
  structural check. Verified: the p4 shape, a same-effect re-perform in the
  continuation, a branch after the perform, and the A1+A2 combination (a handle
  continuation that performs *twice*, output 20).
- **A3 -- reset/shift continuations meet effects. LANDED.** Probing found most
  shapes already worked after A1+A2: a reset continuation that performs, a handle
  continuation containing a reset/shift, a reset value bound then performed --
  all CPS-emit correctly. A3's concrete contribution was fixing a **pre-existing
  miscompile** (present before A1): an effect *performed inside a reset delimited
  body* and handled by a handler *enclosing* the reset aborted "unhandled
  effect" -- the reset installs only a prompt, and the enclosing handler was
  buried in the reset-continuation frame's env with the frame's `next` set to
  dk_done(). Fixed in `emit_reset` by splicing `dk_copy_enclosing_handlers(cur_k)`
  as that frame's `next` -- the same buried-enclosing-handler fix as the
  shallow-handler case in A1, now applied across all delimiter forms. dk_perform
  walks through the reset's prompt to the enclosing handler; the reified sub still
  contains the prompt (a shift in the resumed computation stays delimited); with
  no enclosing handler the splice is `[done]`, unchanged. Fixture
  `cps-backend-reset-body-perform` (output 6, `direct == cps`). Full suite 2148
  passed, 0 failed (2 reset snapshots regenerated).

  Still evicting (correct fallback, out of A3 scope -- genuine interleaving of
  delimited capture and effect dispatch in one delimited body): a shift whose
  *value computation* performs (`(reset (shift f (+ 1 (perform E))))`), and a
  reset body containing *both* a perform and a shift
  (`(reset (+ (perform E) (shift f 7)))`).
- **Bounded only.** A `cps->cps` tail call in a continuation (recursive /
  unbounded suspensions) stays evicted, matching `await`; note the O(N)-resume
  concern in the fixture comment and keep the report link.

### Track A fixtures & tests

- `cps-backend-two-perform` (A1), `cps-backend-nested-effect-cont` (A2),
  `cps-backend-shift-nested-effect` (A3). Each asserts `direct == cps` and runs
  LeakSanitizer-clean (Track A introduces no owning capture; the reset-lift env
  is Copy-only, same leaked-DK-node regime as today's reset continuations).

---

## Track B -- owning-value env capture, completed (E2-E4)

Carries forward E2-E4 from the archived env-capture plan, with the Option B
design detailed and the consuming-case interaction resolved.

### E-borrow -- leak-clean owning captures via bare aliasing (LANDED)

A sharper realization than the Option A / Option B split for the shapes that are
actually **reachable**. An owning value captured into a multi-shot handler case
is only ever reachable in a **borrow-only** shape: a case that *consumes*
(drops/moves) the capture without the enclosing fn also consuming it evicts
upstream (the fn's scope-exit auto-drop becomes an unlowered `EX_DEFER` --
`docs/reported/cps-handler-case-consumes-owning-capture-evicts.md`). For a
borrow-only capture the **owner is the enclosing fn**, which drops the value
exactly once on its straight-line path -- so the env needs neither clone nor
drop: it rides by a **bare shallow alias**. That is leak-clean (no env clone to
reclaim), has no double-free, and the observed value is exact (no incref
inflation) -- **the leak-clean bar without a DK-node teardown**.

Landed: `owning_cap_borrow_only` (a conservative detector -- every reference to
the capture in the handler-case body must be a pure borrow read: `rc/strong-count`,
`rc->ptr`, `(weak ..)`; any consuming op / move / delivery keeps the incref path)
downgrades a borrow-only owning capture from incref-on-read-out (E1's Option A)
to a bare alias in `collect_caps_case`. The E1 fixture
`cps-backend-owning-capture-handler-case` is now leak-clean (dropped its
`requires.no-leak-check`; observed count is 1, not the inflated 2). A *provably
consuming* rc capture (the rare double-consume that still reaches CPS) keeps the
memory-safe incref path unchanged.

**What this leaves for E3 (true teardown):** only the genuinely-consuming
multi-shot case, where the env must *own* a reference and drop it once per
continuation lifetime. Those shapes currently evict (`EX_DEFER`), so E3 is no
longer on the critical path for a leak-clean *reachable* story -- it becomes the
enabler for *admitting* consuming multi-shot captures (and O1-b P2/P3's abortive
ref teardown), not a prerequisite for leak-cleanliness of what already emits.

### E2 -- aggregate + carrier-ADT owning captures (BLOCKED -- no reachable shape today)

E2 was prototyped (extend `cap_owning_ok` beyond `TY_RC` to a carrier ADT
(`carrier_handle_ok`) and an owning-carrying by-value aggregate
(`owning_byvalue_aggregate`); a borrow-only such capture is a bare shallow copy
like an rc handle -- **no clone glue needed** -- and a consuming one evicts).
The change is sound and non-regressing, **but has no reachable fixture and was
reverted** rather than land untestable code. The blocker, found empirically:

`rc<int>` is essentially the *only* owning type that reaches a multi-shot case,
because reaching CPS requires the capture's ownership to be discharged by an
**explicit** straight-line consume that `is_binding_consumed` recognizes (else a
scope-exit auto-drop is injected as an unlowered `EX_DEFER` and the fn evicts).
For an rc that consume is `(rc/drop r)`. But:

- a **by-value struct** with an owning field has no user-facing explicit drop
  (`drop!` requires `ref<T>`, not the aggregate -- `TUR-Exxxx: drop! requires
  ref<T>, got Own`), so its owning field can only auto-drop -> `EX_DEFER` ->
  evict (this is the separate `byvalue-struct-local-owning-field-leak`);
- a **heap ADT / carrier handle** likewise auto-drops via `EX_DEFER`;
- a **`ref<T>`** captured *across* a control op is exactly O1-b P3, which O1-b
  rejects (crossing ref) pending the same teardown.

So every owning aggregate / carrier / ref capture into a multi-shot case is
blocked upstream by the scope-exit-auto-drop (`EX_DEFER`) hole. **E2 rides the
non-ref auto-drop lowering, NOT E3** -- see
[cps-backend-owning-autodrop-lowering-plan.md](cps-backend-owning-autodrop-lowering-plan.md).
That plan's **P2** lowers the injected `(defer (rc/drop (.f o)))` /
`(defer (drop! (.f o)))` into the SINGLE-SHOT post-handle continuation (exactly
how E1's explicit `(rc/drop r)` after a `handle` already works), which is sound
without any DK teardown. Combined with the reverted `cap_owning_ok` extension +
the consuming-non-rc evict guard (the E2 prototype), that makes the borrow-only
aggregate capture emit leak-clean. Only a genuinely *consuming* / abortive
crossing rides E3. (Earlier drafts of this section said "E2 rides E3" -- that was
wrong; E2's borrow captures are single-shot at the drop and need no teardown.
Original Option A interim design retained below for reference.)

#### E2 (original Option A interim -- superseded by E-borrow for the reachable case)

Extend `cap_owning_ok` (`emit_cps_ir.c:670`, today `ty == TY_RC` only) to a
**carrier ADT** and an **owning-carrying by-value aggregate**, reusing O2's
struct/ADT clone glue for the read-out clone. `cap_ctype` / `emit_lifted`
already declare aggregate env fields; the clone-on-read-out emit needs the
type's clone-glue function name (resolved via `type_uses_carrier_abi` /
`adt_is_byvalue_product`, the same keys O2's drop glue uses). Fixture: a
captured `defstruct` holding an `rc` field into a handler case, `requires.no-leak-check`
(still Option A -- the env's clone is leaked until E3).

### E3 -- refcounted env with clone/drop (Option B), leak-clean -- THE substrate

> **Detailed plan:**
> [cps-backend-owning-env-teardown-e3-plan.md](cps-backend-owning-env-teardown-e3-plan.md).
> That doc supersedes the sketch below, and reframes E3 post-E-borrow: it is no
> longer needed for leak-cleanliness of what emits today (E-borrow did that) --
> its job is to ADMIT the currently-evicting consuming / aggregate / carrier / ref
> captures, in two phases (E3a per-frame env clone/drop hooks; E3b delimited-region
> teardown, which also retires the DK-node leak). It is the shared substrate O1-b
> P2/P3 land on.

Stop leaking the owning-carrying env; give the lifted continuation frame a real
clone/drop pair, emitted **only when `caps` contains an owning field** (Copy-only
envs keep today's leaked fast path untouched):

- **clone** (run when `dk_copy_node`/`dk_copy_range` copies the spine for a
  multi-shot resume): deep-copy the env struct and run each owning field's clone
  glue (`rc_strong_increment` for an rc handle; the O2 struct/ADT clone glue for
  an aggregate). Copy fields stay a shallow copy.
- **drop** (run when the DK node / cont is freed): run each owning field's drop
  glue, then `free` the env.

Two wiring options, pick during A1/E3 co-design:

1. **Reuse `tur_cloneable_cont`.** Route the owning-carrying frame through
   `tur_cloneable_cont_alloc(fn, cap, <env_clone>, <env_drop>)` (as the
   cloneable-shift path already does at `:3599`), emitting a per-continuation
   `<hname>_env_clone` / `<hname>_env_drop` that the callbacks dispatch to.
2. **Give `dk_frame` env clone/drop hooks + a `dk_free` teardown.** Add optional
   `env_clone` / `env_drop` fn pointers to the `DK` node (default NULL = today's
   leaked behavior); `dk_copy_node` runs `env_clone`, `dk_free` runs `env_drop`.
   This is the more invasive but more uniform option and is what
   `__dk_env_clone` / `__dk_env_drop` (`emit_dk_runtime.c:57-60`) would grow into.

Extend `__dk_env_clone` / `__dk_env_drop` (which today only `dk_copy_range` /
`dk_free` the spine) to also clone/drop the owning payload inside each frame's
env. Drop `requires.no-leak-check` from the E1/E2 fixtures once E3 lands -- they
then run under normal leak detection. **This is the graduation-quality landing**
and the substrate O1-b P2/P3 also depend on (see below).

### Resolve the consuming-case `EX_DEFER` interaction (owned by E3)

The archived plan's literal E1 shape -- "the case *drops* the rc once per
resume, the enclosing fn does not" -- evicts today
(`docs/reported/cps-handler-case-consumes-owning-capture-evicts.md`):
`is_binding_consumed` (`src/compiler/elab_core.c:1195`) does not traverse
`handle` expressions, so a consume that lives only inside a case is invisible to
it; the rc auto-drop injection (`src/compiler/elab_forms.c:1117-1178`) then sees
`is_binding_consumed(f_body, r) == false`, injects `(defer (rc/drop r))`, and
that `EX_DEFER` has no CT-IR lowering -> `f` evicts.

**Resolution (E3):** the owning value captured by a multi-shot continuation is
owned by the **env** and dropped exactly **once per continuation lifetime** (the
env `drop` callback), NOT by a user-written drop in the case body. A case-body
drop is not a sound ownership discharge anyway -- the case may run 0 times
(leak) or N times (N drops). So E3:

- redirects the captured owning local's scope-exit ownership discharge to the
  env teardown, and
- **suppresses the elaborator's scope-exit auto-drop** for a local that is
  captured (by value, owning) into a resumable continuation -- coordinate with
  the auto-drop injection site (`elab_forms.c:1117`) so no `EX_DEFER` is emitted
  for such a local.

Explicitly **do not** "fix" this by teaching `is_binding_consumed` to traverse
`handle` cases -- that would suppress the auto-drop on the strength of a
case-body drop that does not reliably run, which is unsound on both the direct
and CPS paths.

### E4 -- reset/shift multi-shot owning capture

A reset/shift whose continuation is *resumed more than once* (multi-shot resume,
distinct from Track A's multi-*suspension*) and whose body captures an owning
value. Today a second resume in the CT-IR subset is a hard error (TUR-E0201);
multi-shot resume rides the cloneable / DK-copy path. E4 admits an owning
capture on that path via E3's env clone/drop (the `dk_copy_node` clone fires the
env clone per resume). Fixture: a generator/step shift resumed twice capturing
an `rc`. Land after E3.

---

## How the tracks combine -- the capstone

A genuine **"handler case captures an owning `rc` and runs twice at runtime"**
fixture needs both tracks:

- **Track A / A1** makes the two-`perform` `g` CPS-emit, so the handler case
  actually runs twice.
- **Track B / E3** makes the shared owning capture memory-safe: the env owns one
  reference, the clone callback fires per resume so each case invocation has its
  own +1, and the env `drop` fires once at teardown -- **no leak**, so this
  capstone fixture runs under normal leak detection (unlike E1's Option A
  interim). E1's clone-on-read-out is the Option A stand-in until E3.

## Recommended order

1. **A1** (two-perform) -- widens the CPS subset, independently valuable, unblocks
   runtime multi-shot. Lowest risk (proven F3 template).
2. **E3** (Option B env teardown) -- the shared substrate; also unblocks O1-b
   P2/P3; makes E1/E2 leak-clean.
3. **E2**, **A2/A3**, **E4** -- round out the shapes.
4. **Capstone** (A1 + E3): runtime multi-shot owning-capture fixture, no
   `requires.no-leak-check`.

## Interaction with other plans

- **Supersedes** `cps-backend-env-capture-owning-values-plan.md` (archived). E1
  landed there; E2-E4 continue here as Track B.
- **O1-b** ([cps-backend-ref-scope-exit-drop-plan.md](cps-backend-ref-scope-exit-drop-plan.md)):
  P2 (abortive-unwind ref drop) rides E3's DK teardown; P3 (resumable-crossing
  ref) is the `ref`-flavored instance of Track B's owning capture. Its state
  line points at the env-capture plan -- redirect it here.
- **N6.5** ([cps-backend-n6-fallback-removal-followups-plan.md](cps-backend-n6-fallback-removal-followups-plan.md)):
  Track A shrinks the nested-control-in-continuation fallback surface; Track B
  shrinks the owning-capture fallback surface. Both are residual-fallback line
  items; list them when N6.5 audits residuals, with the bounded-only /
  consuming-case carve-outs named.
- **F2 / F3 (landed):** Track A generalizes F3's async/await gap-2; F2's shallow
  handlers already ride the DK heap-continuation substrate. Confirm Track A
  composes with `dk_handler_shallow` / `dk_copy_enclosing_handlers`.

## Depends on / reuses

- Track A: `await_cont_reset_ok` (`emit_cps_ir.c:1079`), `emit_await` gap-2
  (`:3877-3900`), `perform_body_ok` (`:1034`), `handle_case_ok` (`:1111`),
  `shift_body_ok` (`:997`), the `CT_PERFORM`/`CT_HANDLE`/`CT_SHIFT` classification
  (`:1377`, ...) and emit (`emit_perform:3753`, `emit_handle:3675`,
  `emit_shift:3651`), `LH_RESET_CONT` / `emit_cont_env`.
- Track B: `cap_owning_ok` (`:670`), `CapSet.owning[]`, the clone-on-read-out in
  `emit_lifted`, `tur_cloneable_cont_alloc` (`:3401`, `:3599`),
  `__dk_env_clone` / `__dk_env_drop` (`emit_dk_runtime.c:54-60`), `dk_copy_node`
  / `dk_free` (`cps_prompt.c:81`, `:147`), `rc_strong_increment` /
  `rc_strong_decrement`, O2 struct/ADT clone+drop glue (`type_uses_carrier_abi`,
  `adt_is_byvalue_product`).
- Consuming-case interaction: `is_binding_consumed` (`elab_core.c:1195`), the rc
  auto-drop injection (`elab_forms.c:1117-1178`).

## Out of scope

- **Recursive / unbounded suspension continuations** (a `cps->cps` tail call in a
  continuation body) -- stays evicted on both tracks; closing it needs a
  trampolined heap continuation to avoid an O(N) resume stack, a separate plan.
  Tracked for `await` in `docs/reported/cps-async-recursive-await-eviction.md`.
- **General user `defer` in a colored function** -- a broader defer-semantics
  question; the auto-drop handling here is gated to the elaborator-injected
  owning auto-drop shape only.
- **`ref<T>` scope-exit auto-drop** -- O1-b owns it; P2/P3 there ride E3's
  teardown rather than reinvent it.
- **Adding owning pointers to `slot_ty`** -- Findings 1-2 of the owning-pointers
  parent stand: owning pointers never cross the slot bare, so there is nothing to
  hook.
