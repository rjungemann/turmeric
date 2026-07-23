# Closure env drop glue -- freeing captured fat-closure environments

> **Re-verified 2026-07-22: DONE / GRADUATED, plan is accurate.** All phases (S1,
> S2/Model U, Model R, R1, R2, R3a, R4) confirmed in the tree. Graduation to
> always-on is commit `3f4a5c980` (`EXPERIMENTS[]` row -> `GRADUATED[]` as a
> TUR-W0063 no-op; `drop_glue_<env>` walk in `emit_fns.c`, `TUR_CLOSURE_DROP` +
> `env[-1]` header ABI in `emit_module.c`). The two R3a auto-drop increments
> (`Handler` half 1, `ClosureChain` half 2) landed AFTER graduation as shipped
> stdlib, not pending -- commits `8a5c59a71` and `cff818796`. R2b/R3b remain the
> only by-design deferrals (no corpus driver). **Doc-hygiene only:** the three
> `docs/reported/...` report paths cited below (httpd-request-accessor-cstr-leak,
> httpd-mw-rate-limit-state-leak, httpd-new-pool-failure-handler-leak) have all
> been moved to `docs/archive/...`; and `docs/reported/escaping-fat-closure-env-leak.md`
> (the report this plan resolves) is itself now due for archival -- its gate is met.
>
> # ✅ DIRECTIVE SATISFIED (2026-07-22) -- Model R walk-glue LANDED.
> The blocking directive below has been carried out (left intact, unreworded,
> as instructed). Model R shipped behind `--enable=closure-drop-glue`: the
> `drop_glue_env_N` header + walk, capture-time retain/move, the `Drop`
> typeclass owned-opaque release, and 10 httpd fixtures flipped leak-clean --
> see the dated progress notes below (2026-07-21d..2026-07-22) and the updated
> Status. The remaining residuals are documented and out of the original
> blocking scope (variadic-rest uniqueness for `mw-compose-of`; the reactor/CPS
> runtime-API for the async family; and the httpd request-accessor cstr leak,
> now `docs/reported/httpd-request-accessor-cstr-leak.md`).

> # ⛔ ACTIVE / BLOCKING -- BUILD THIS NOW. DO NOT DEFER. DO NOT REWORD.
>
> This is not deferred, not "until later," not "the next slice," not a
> "quick win to bank," and not optional. It is the current front of the
> track and **all other work is blocked until the walk-glue lands and the
> leak-suppressed fixtures below are valgrind-clean.**
>
> **If you open this file, your job is to write CODE, not prose.** Editing,
> re-summarizing, re-scoping, or appending another "progress note" to this
> plan is **explicitly NOT progress** and does not count as picking it up.
> The plan has been analyzed to death; the load-bearing core
> (`drop_glue_env_N` + capture-time clone) is still at ZERO lines in `src/`.
> Verify that yourself: `grep -rn drop_glue_env src/` -> no hits.
>
> **Definition of done (the only thing that counts):**
> 1. Capture-time retain/clone for owning captures **and** the
>    `drop_glue_env_N` walk-glue land together as one atomic unit (see
>    "Implementation findings" -- either alone is unsound).
> 2. `free-lift-bind` and `unsafe-closure-capture` are valgrind/LSan-clean
>    and their `requires.no-leak-check` markers are DELETED.
> 3. Then `hkt-stdlib-parser-instances` and `cps-backend-fn-param` (the
>    escaping/colored cases) via S2 Model U + CPS admission, markers deleted.
>
> "It's too much work" / "it can't be safely chunked" is a description of
> THIS task, not a reason to hand it back. The hazard (double-free) is real
> and is exactly why 1a+1b are atomic -- build them atomic, prove it under
> valgrind, do not split them to dodge the hazard. Do not return until a
> real fixture flipped clean. This directive overrides the general
> "nothing blocks the track" posture in CLAUDE.md **for this work only** --
> by explicit owner instruction (2026-07-21).

**Status: GRADUATED 2026-07-22 -- the drop-glue header ABI is now the DEFAULT and
the `closure-drop-glue` experiment is retired.** The `EXPERIMENTS[]` row is gone
(name moved to `GRADUATED[]` as a TUR-W0063 no-op), the `g_opt_closure_drop_glue`
bit and all 19 codegen gates are removed (each always-taken), every per-fixture
`--enable` flag file dropped, and all 140 `expected.c` + 3 rc-count
`expected.stdout` regenerated to the always-on codegen. Full suite green
(2264/0). Everything below is the historical development record.

**Status (pre-graduation):** S1, S2/Model U, AND **Model R LANDED** (behind
`--enable=closure-drop-glue`; base language byte-for-byte unchanged, suite green).
Every value-closure, HOF-arg, and stored-in-a-generated-holder escaping closure
is freed unconditionally (8 opt-outs dropped); and under the experiment the fat
env now carries an `env[-1]` drop-glue header walked by `drop_glue_env_N`, with
retain/move at capture, a `Drop` typeclass for owned-opaque (String) captures,
and a partial-application-head free. **15 httpd fixtures** are flipped leak-clean
flag-on (mw-log, basic-auth{,-attr,-noncapture}, body-size, json, static, cors,
cors-opts, compose, compose-of, async-mw-compose, h7-middleware, h5-tls,
mw-fold-many), and the whole async/reactor family (the `reactor-*` fixtures,
`async-capturing-closure`, ...) is corruption- and leak-free flag-on. Residuals
(documented in the dated notes + reports):
`mw-cookie`/`-form` are an httpd request-accessor cstr leak, not a closure issue
(`docs/reported/httpd-request-accessor-cstr-leak.md`). Prepared from
`docs/reported/escaping-fat-closure-env-leak.md` and the B2 residuals in
`cps-runtime-finish-plan.md` (Progress-log PD).

## Remaining work -- roadmap to graduation

> This section is the CURRENT forward plan. Everything above the dated progress
> notes (the "ACTIVE / BLOCKING" directive, the original Design / Phasing /
> Implementation-findings sections) is now a HISTORICAL record of the landed
> S1 / S2 / Model-R core -- read it for context, not for what to do next. The
> ownership machinery is functionally complete for the corpus; what follows is
> everything left, in priority order.

**Naming, to avoid confusion.** Two distinct things share the name "Model R":
1. **The header/walk ABI that shipped** (the dated notes call this "Model R"):
   the `env[-1]` drop-glue header + `drop_glue_env_N` walk + capture-time
   retain/move + `Drop` typeclass. This is LANDED behind the experiment.
2. **"Model R (refcount)"** in the Design section below: a per-env `__rc` word
   for genuinely *shared* closures. This is a DEFERRED fallback (R3), never
   built, because the move model (Model U) covers the whole corpus.

### R1 -- Cleanup: flip the last closure-shaped httpd markers -- DONE (2026-07-22)

Two fixtures leaked a *closure* env flag-on only because a teardown site or a
fixture annotation was never updated to the landed machinery. Same shapes as the
already-flipped `compose` / `async-mw-compose`; both are now leak-clean flag-on,
markers dropped, `flags: --enable=closure-drop-glue` added (suite green):

- **`httpd-h7-middleware`** [DONE] -- `mw-tag`'s handler param annotated
  `^fat next`, so the drop-glue walk frees the wrapper onion (identical to
  `mw-compose` / `mw-compose-of` source #1). Was 56 B / 2 allocs from `mw-tag`.
- **`router-free`** (`stdlib/httpd.tur`) [DONE] -- the linked-list teardown's
  bare `free(cur->handler)` rewired to `TUR_CLOSURE_DROP` (walks the onion;
  flag-off it is the identical plain free). Covers any router that stores a fat
  handler (`httpd-h6-routing` is now header-safe flag-on).
- **`httpd-new-tls`** (`stdlib/httpd.tur`) [DONE] -- this was `httpd-h5-tls`'s
  residual 24 B: `httpd-new-tls` takes `^fat handler` but its two EARLY refusal
  paths (`ctx == 0`; TLS ops unregistered) returned NULL without freeing the
  owned box. Both now `TUR_CLOSURE_DROP(handler)` before returning. (The deeper
  `httpd-new-pool` failure-path handler leak is pre-existing, not closure-onion
  shaped, and unexercised -- left for a separate httpd-teardown pass.)

Exit: MET -- `httpd-h7-middleware` and `httpd-h5-tls` leak-clean flag-on, markers
dropped, `bash tests/run.sh` green. No snapshot churn (no `expected.c` embeds the
changed httpd functions).

**Explicitly NOT R1** (the marker text on several is stale). Profiled flag-on and
split by ACTUAL root cause (all now have `docs/reported/` findings where genuine):

- `httpd-mw-cookie` / `-form` -- request-accessor `cstr` leak from
  `httpd-req-cookie` / `-form`
  (`docs/reported/httpd-request-accessor-cstr-leak.md`). Non-closure.
- `httpd-mw-fold-many` -- RESOLVED (R3a, 2026-07-22): the runtime-built onion +
  spine + factory heads, now leak-clean flag-on via the `httpd-mw-drop` /
  `httpd-mw-free-chain` teardown primitives + `^fat next`. Report archived at
  `docs/archive/httpd-mw-fold-many-closure-list-leak.md`. See R3a below.
- `httpd-mw-rate-limit` -- the `RLState` counter table
  (`httpd-mw-ratelimit-new`) is never freed; non-closure httpd-teardown gap.
  `docs/reported/httpd-mw-rate-limit-state-leak.md` (option (b) there folds it
  into the `Drop`-capture path).
- `httpd-req-string-opt` -- the fixture's own inline-C fabricates `HttpdConn` /
  cookie / body graphs with no server to reclaim them; an intentional
  test-scaffold leak (the marker documents it). Not a compiler/stdlib bug.
- `httpd-mw-compress` -- `requires.spices` skip (missing zlib dep).
- Non-httpd markers (`panic-catch-unwind*`, `panic-*`, `cli-*`,
  `cps-backend-heap-adt-return`) -- unrelated to closures entirely.

Also filed while closing R1: `docs/reported/httpd-new-pool-failure-handler-leak.md`
-- `httpd-new-pool` leaks the `^fat handler` on its own construction-failure
paths (deeper than the R1 `httpd-new-tls` early-refusal fix; unexercised).

### R2 -- Walk generality gaps -- FINDINGS (2026-07-22): R2a folds into R3; R2b deferred

A tractability pass (mirroring the S1 "Implementation findings" discipline) tried
to drive each gap with a real fixture. Result: **R2a is not independently
testable and should fold into R3; R2b is a soundness-hardening item with no
current driver.** The gaps and the evidence:

**R2a -- type-honest owning-closure captures.** The drop-glue walk
(`emit_fns.c` ~2903) frees a capture only when it is `TY_RC` (rc decrement),
`cap->is_fat` (moved-in `^fat` handle -> `TUR_CLOSURE_DROP`), or `Drop`-typed
(the `Drop` instance). A capture that is an owned closure handle of type
**boxed `TY_FN`** but did NOT arrive via a `^fat` param has `is_fat == false` and
is skipped -- confirmed: for `(let [inner (fn..k..)] (fn [y] (inner y)))`,
`drop_glue___env_<wrap>` frees only its base and leaves the `inner` field. So the
gap is real in the emitter.

BUT it **cannot be exhibited as an isolated leaking fixture**:
- If the outer closure is scope-local (the only case the scope-exit drop-glue
  governs), `-O2` statically traces the whole closure graph and **elides every
  malloc** -- verified down to a bare unused `malloc(64)` reporting zero leaks;
  and even an *observable* small alloc is hidden by LSan stack-leftover
  false-negatives. No leak is observable, so the walk's coverage of that capture
  is moot.
- The gap only bites when the outer closure **escapes** into a heap structure
  (returned, stored in a struct field, threaded into a chain/list). There the
  captured inner closure's lifetime is governed by the escape machinery --
  **Model U** (the holder struct's fn-field drop glue) or **R3** (an owned
  runtime `list<Closure>`) -- NOT the scope-exit walk. `httpd-mw-fold-many`
  (`docs/reported/...`) is exactly this: its factory heads + spine escape into a
  `build-chain` list consumed by `httpd-mw-fold`.

Conclusion: there is nothing to build in the scope-exit walk. When a real driver
appears it is an *escape* case, so the fix belongs in R3 / the Model U fn-field
drop, keyed on the field type (`TY_FN` boxed / owned `ref`) with the same
move-gate the `is_fat` arm already uses. R2a is therefore **merged into R3**.

**R2b -- cross-function double-ownership -- FINDINGS (2026-07-22): NOT a live
hazard; closed.** The concern was: the capture move closes *same-function*
aliasing (a second capture of a moved `^fat` handle is `TUR-E0005`), but a caller
that also freed a handle it passed into a consumer could double-free across the
call boundary. Probed it directly and it is **not reachable** for an honest
consumer:
- Passing a fat handle to a `^fat` (consuming) param: the escape analysis
  (`binding_escapes_impl`, "only ever greenlights a free") treats the arg as
  ESCAPING, so `let_binding_env_freeable` does NOT free it caller-side -- the
  consumer is the sole owner. Verified: a `(fn ..k..)` heap closure passed to a
  `^fat` consumer that `TUR_CLOSURE_DROP`s it -> main emits no caller free, no
  double-free (ASan clean). The ownership transfer is effectively threaded across
  the boundary already, by conservatism.
- The only theoretical double-free needs a `^borrow`/nonretain consumer that
  VIOLATES its contract by freeing a handle it promised only to borrow. That is
  (a) a consumer bug, not a general hole; (b) present flag-OFF too (a `^borrow`
  callee that frees double-frees regardless of closure-drop-glue); and (c) not
  even reachable in these shapes -- the `:int`-erased consumer params force a
  `(:: h int)` ascription on the arg, which defeats the escape-analysis
  borrow-arg recognition, so the caller does not free the handle anyway.

Conclusion: closure-drop-glue introduces no new cross-function double-free.
Nothing to build. If `^fat`/borrow handles are ever un-erased to honest
owning-closure types (the R2a/R3a type-honesty theme), thread the move across the
call boundary then for defense-in-depth -- but there is no live hazard to guard
today. R2b **closed**.

Neither R2a nor R2b is a prerequisite for graduation (R4) on the current corpus.

### R3 -- Escaping owned-closure captures + shared closures

Closures that ESCAPE their constructor (so the scope-exit walk does not reach
them). The driver is `httpd-mw-fold-many`
(`docs/reported/httpd-mw-fold-many-closure-list-leak.md`) -- a runtime
`build-chain` list of factory closures consumed by `httpd-mw-fold`, whose spine +
factory heads + composed onion all leak.

**R3a -- runtime-built chain teardown -- DONE (2026-07-22), explicit-primitive
increment.** `httpd-mw-fold-many` is now leak-clean flag-on (marker dropped,
`flags: --enable=closure-drop-glue`). Because `httpd-mw-fold` and its cons list
are `:int`-erased (no honest `Closure` / `list<Closure>` type), the compiler
cannot auto-derive the drop the way `mw-compose-of`'s *variadic-rest,
distinct-let-binding* shape allowed caller-side. So R3a shipped as two sound
stdlib teardown primitives the builder of a runtime chain calls explicitly:

- `httpd-mw-drop [handler]` -- `TUR_CLOSURE_DROP` the composed onion; under the
  experiment the drop-glue walk (needs `^fat next` on the middleware, as
  `mw-count` now has) frees every wrapper down to base.
- `httpd-mw-free-chain [mws]` -- drop each head factory + free each spine cell of
  the INPUT list (the fold only borrows the factories). Sound iff each head is a
  distinct fresh closure (documented); it is the "free the heads too" companion
  to the existing `httpd-mw-free-spine`.

This closes the corpus leak. What it does NOT do -- and what a future
*automatic* R3a needs -- is the honest re-typing of `httpd-mw-fold` /
`build-chain` off `:int` onto `Closure` / `list<Closure>` so the holder's
drop-glue drops the field/spine with no explicit call (the Model U fn-field drop
generalized to a captured `TY_FN`-boxed field + a cons spine, move-gated exactly
as the landed `is_fat` arm). That was a larger httpd-fold re-typing; the explicit
primitives are the sound stopgap and are independently useful for any hand-built
dynamic chain. **Update: half of it -- retiring `httpd-mw-drop` via an automatic
onion auto-drop -- landed 2026-07-22 (see below), and the input-spine half
(`httpd-mw-free-chain`) landed the same day via the same affine-opaque mechanism.
The deeper typed-recursive `Cons.tail` de-erasure was found not to be needed.**

**Automatic R3a, half 1 -- `httpd-mw-drop` RETIRED (2026-07-22).** A bound
composed onion now auto-drops at scope exit, no explicit call. The route turned
out cleaner than the first feasibility probe guessed: rather than generalize the
`returns_fresh_closure` escape analysis, it rides the **Drop typeclass** plus an
**`:affine` opaque**.

- *Compiler (`elab_forms.c`).* The let scope-exit auto-drop injection -- which
  previously fired only for `TY_REF` (rc/ref) bindings, emitting `(defer (drop!
  r))` -- now also fires for a binding whose type is a **move-only (`:affine`,
  non-Clone) Drop-instance opaque** initialized by a fresh-producing call. For
  those it injects `(defer (<Drop.drop> b))`, dispatching through the type's Drop
  instance (which routes to `TUR_CLOSURE_DROP`) instead of `drop!`->`free` (a bare
  free of the past-header fat pointer is the interior-free abort). New helper
  `binding_closure_drop_inst`. The existing move/consume filters (`is_moved`,
  `is_binding_consumed`, moved-during-init) are reused unchanged, so a handle
  handed to a consumer (poisoned by the affine arg-move) is not double-dropped.
- *stdlib (`httpd.tur`).* `(defopaque Handler :int :affine)` + a non-Clone `Drop
  [Handler]` instance (-> `httpd-handler-drop` -> `TUR_CLOSURE_DROP`).
  `compose-middleware-of` returns `Handler`. A bound Handler that is neither moved
  nor borrowed auto-drops. Two use-shapes:
  - **borrow** (invoke and keep): `(httpd-call (:: composed :int) conn)` -- the
    ascription is a non-consuming read, so the scope-exit auto-drop still fires.
  - **handoff** (server takes ownership): `httpd-handler-carrier [h : Handler] :
    int` consumes the affine Handler (arg-move) and yields the raw carrier, which
    *suppresses* the auto-drop so the server's `httpd-free`/`httpd-async-free` is
    the sole owner -- no double-free.
  `httpd-mw-drop` is kept only for legacy raw-`:int` onions; the fixtures no
  longer use it.
- *Soundness.* Deterministic (alloc/free counter, LSan-independent) fixture
  `closure-drop-affine-opaque-autodrop` proves all three shapes: unused -> drop
  once; `^borrow` -> drop once; consuming move -> suppressed (ASan-clean, no
  double-free). `httpd-mw-fold-many` (leak-checked, was the driver) now uses the
  Handler auto-drop in place of `httpd-mw-drop` -- byte-equivalent reclaim (same
  `TUR_CLOSURE_DROP`, compiler-injected). `httpd-mw-compose-of` exercises the
  server handoff via `httpd-handler-carrier`. Full suite 2266/0.

Half 2 (below) -- auto-dropping the input spine + heads -- landed the same day.

*Original probe of half 1 (superseded by the landed route above; kept for the
record):* the only pre-existing scope-exit auto-drop for a *returned* closure is
`returns_fresh_closure` (`elab_fns.c` ~3518), restricted to a bare `EX_CLOSURE`
body with scalar-Copy captures/result and a bare `free`; generalizing *that* was
one option, but the Drop-typeclass + `:affine`-opaque route avoided touching the
fresh-closure escape analysis entirely.

**Automatic R3a, half 2 -- `httpd-mw-free-chain` RETIRED (2026-07-22).** The input
spine + factory heads now auto-drop, via the SAME affine-opaque + Drop mechanism
as half 1 -- no compiler change beyond the half-1 `elab_forms.c` injection.

- *stdlib (`httpd.tur`).* `(defopaque ClosureChain :int :affine)` + a non-Clone
  `Drop [ClosureChain]` instance whose method (`httpd-chain-drop`) is the exact
  body of the old `httpd-mw-free-chain`: walk the cons spine, `TUR_CLOSURE_DROP`
  each head, `free` each cell.  Bind the runtime chain as a ClosureChain and it
  auto-drops at scope exit; `httpd-mw-fold` BORROWS it via `(:: chain :int)`.
  `httpd-mw-free-chain` is kept only for legacy raw-`:int` chains (docstring marks
  it superseded).
- *Soundness.* Deterministic fixture `closure-drop-affine-chain-autodrop`
  (alloc/free counters, LSan-independent, ASan-guarded): a bound 4-cell chain,
  borrowed then left to auto-drop, frees all 4 heads + cells exactly once.
  `httpd-mw-fold-many` now binds BOTH `composed` (Handler) and `chain`
  (ClosureChain) as affine Drop types -- defers fire LIFO (onion first, chain
  second), matching the old explicit `httpd-mw-drop` / `httpd-mw-free-chain`
  order, byte-equivalent reclaim, still leak-checked + ASan-clean.

**Why NOT the `Cons.tail` de-erasure.** The original blocker framed half 2 as
needing a typed recursive `list<Closure>` whose *type-driven* drop glue walks a
typed spine -- i.e. de-erasing `Cons.tail` from `:int` to `(Cons A)`.  That was
empirically confirmed to be a large, *absent* compiler capability, and it is NOT
needed to retire `httpd-mw-free-chain`:
- A plain `:heap` recursive owning ADT (`(defdata WList :heap (WNil) (WCons Widget
  WList))`) emits **no** drop glue and leaks its whole spine at scope exit -- even
  its ctor erases the tail to `int64_t`.  `emit_adt_byval_drop_glue`
  (`emit_module.c` ~5411) *does* emit a recursive field-releasing `drop_glue_<T>`,
  but only under `needs_drop_glue`, only for rc-driven teardown of *distinct*
  nested aggregates -- not triggered for a plain owning-ADT let-binding, and not
  built for self-recursion.  So the honest path is: (a) set `needs_drop_glue` +
  recognize affine/closure heads and a self-recursive tail as owning fields, and
  (b) add a scope-exit trigger category for owning-ADT let-bindings in
  `emit_let_value`.  Both are real compiler work with broad blast radius (touches
  owning-ADT semantics generally), benefiting one corpus site.
- The affine-opaque + hand-written spine-walk `Drop` achieves the same retirement
  at the same honesty level as the shipped `Handler` (an affine newtype over the
  int carrier with a Drop instance), soundly and with near-zero blast radius.

The typed-recursive `list<Closure>` drop glue remains available as a *future*,
independently-scoped compiler feature (the honest generalization), but it is no
longer a blocker for anything in the corpus.

**R3b -- "Model R (refcount)"** below remains deferred: a roadmap-sized ABI/compiler
item, not a bounded follow-up, with no driver in the corpus.

**R3b -- "Model R (refcount)"** (build-on-demand). The per-env `__rc` word for
genuinely *shared* closures (Design section below). A heavier ABI change to the
`^fat` layout / HKT thunk recovery / `tur_poly_fn_t` (audited in
`docs/archive/fat-closure-abi-audit-plan.md`). Reserve for the case the move
model cannot express -- a closure legitimately owned by two live owners at once.
No driver in the corpus; no action until one appears.

### R4 -- Graduation off the experiment -- DONE (2026-07-22)

**Graduated at 0.30.2** (early, by owner instruction; the 0.34.0 `expires_at` was
a deadline, not an earliest date). The header ABI is now unconditional and the
experiment is retired. The steps executed, in one coordinated change:
1. Removed all 19 `g_opt_closure_drop_glue` codegen gates (each made
   always-taken), across `elab_fns.c`, `emit_core.c`, `emit_cps_ir.c`,
   `emit_dk_runtime.c`, `emit_expr.c`, `emit_fns.c`, `emit_module.c`.
2. Deleted the `EXPERIMENTS[]` row and the `g_opt_closure_drop_glue` bit
   (`experiments.c`, `globals.c/.h`); added `closure-drop-glue` to `GRADUATED[]`
   so a lingering `--enable` is a TUR-W0063 no-op.
3. Dropped all 21 per-fixture `--enable=closure-drop-glue` flag files.
4. Regenerated all **140** `expected.c` snapshots + the **3** rc-count
   `expected.stdout` (rc-auto-drop-closure-capture 1->2,
   rc-elision-negative-closure-capture 2->3, closure-env-free-with-owning-sibling
   37->39 -- the intended rc-retain semantics).
5. Full suite green: **2264 passed, 0 failed.** The gate removals are faithful (a
   mis-removed gate would have re-introduced a flag-off crash the suite catches).

The remaining open items are the by-design deferrals only: R2b closed (not a live
hazard), R3a shipped (explicit primitives; automatic re-typing still future),
R3b build-on-demand (no driver). Original graduation plan retained below.

#### Original graduation procedure (executed above)

`closure-drop-glue` was `XF_LIFECYCLE_PROTOTYPE`, off by default, with a 0.34.0
`expires_at`. To graduate (feature goes always-on, `EXPERIMENTS[]` row deleted,
flag removed):

1. **Emit the header ABI unconditionally.** Flag-off, `TUR_CLOSURE_DROP` already
   expands to a plain `free`, but the header changes the env allocation layout,
   so making it default CHURNS every closure codegen snapshot -- **measured: all
   140 `expected.c` snapshots change** (the header preamble is emitted into every
   program), a single coordinated regen in the graduation PR (fixture-churn
   policy in CLAUDE.md).
2. **Prove the whole corpus clean flag-on.** **NOT YET -- this is the blocker.**
3. **Delete the `EXPERIMENTS[]` row** in `src/runtime/experiments.c` and the
   `g_opt_closure_drop_glue` gates (each gate becomes always-taken).

> **R4 VALIDATION FINDINGS (2026-07-22) -- graduation is BLOCKED; earlier
> "graduate after R1" recommendation RETRACTED.** Forcing the flag on corpus-wide
> (`g_opt_closure_drop_glue = true`) with snapshots moved aside so codegen churn
> cannot mask runtime failures, the full suite is **2231 passed, 33 FAILED** --
> not the clean corpus step 2 assumed. All 33 are teardown paths that bare-free a
> now-headered fat handle (interior free -> `invalid pointer` / SIGABRT crashes;
> confirmed on `panic-catch-unwind-basic`, `capturing-closure-struct-field`).
> These are PRE-EXISTING latent flag-on bugs (flag-off is 2264/0; every opted-in
> closure-drop-glue fixture passes) that the opt-in surface never exercised, in
> four clusters -- **catch-unwind/panic (~16), effect/continuation/shift-resume
> (~8), fn-field/struct-closure drop (~5), rc-drop-closure + misc (~4)**. Full
> list + fix directions: `docs/archive/closure-drop-glue-graduation-blockers.md`
> (archived on resolution -- see the R4-prep note below).
>
> Each is the SAME bug class already fixed for httpd/reactor/DK-reap: a free site
> (emitted or precompiled runtime C) that must route through `TUR_CLOSURE_DROP`
> (or the `tur_reactor_release_box` header-aware pattern). Fixing the four
> clusters is the real bulk of R4-prep -- a genuine body of work, NOT the
> mechanical snapshot-regen the plan assumed.

> **R4-prep DONE (2026-07-22) -- crashes fixed, forced-on suite 2261/3.** All
> three crash clusters routed through the header-aware release, each gated so
> flag-off is byte-identical (0 snapshot churn):
> - **catch-unwind / catch-panic-of** thunk box (`emit_expr.c`) -- 16 fixtures.
> - **effect / shift receiver** reap (`emit_cps_ir.c`, `__dk_reap_ptr` ->
>   `__dk_reap_closure`) -- 8 fixtures.
> - **struct/ADT fn-field drop** glue `drop_fnfields_<T>` (`emit_module.c`) -- 5
>   fixtures (+ `dot-receiver-first-call`).
>
> A fresh forced-on full suite is **2261 passed, 3 failed** (was 33). The 3
> remaining are NOT bugs: `rc-auto-drop-closure-capture`,
> `rc-elision-negative-closure-capture`, `closure-env-free-with-owning-sibling`
> print an rc strong-count that legitimately rises by 1-2 flag-on because the
> drop-glue RETAINS an rc capture (the closure holds its own strong ref -- the
> intended Model R semantics). Their `expected.stdout` is flag-off and cannot
> change now without breaking the flag-off suite -- a graduation-time
> expected-output regen, folded into the mass regen below. Report archived:
> `docs/archive/closure-drop-glue-graduation-blockers.md`.

Decision gate at the 0.34.0 cut: **graduate** (the crash blockers are now fixed;
what remains for graduation is mechanical), **extend `expires_at`** (if the cut
arrives first), or **shelve** (revert the ABI). **Current recommendation: HOLD
graduation until the 0.34.0 gate**, but it is now a MECHANICAL step, not a
research one. When taken, the graduation PR is a single coordinated change:
1. Default `g_opt_closure_drop_glue` on / drop the gates, emit the header ABI
   unconditionally.
2. Regen all **140 `expected.c`** snapshots + the **3** rc-count
   `expected.stdout` (the intended flag-on counts).
3. Delete the `EXPERIMENTS[]` row.
Verify with a full suite (now flag-on by default) -- expected green after the
regen. We are at 0.30.2, four minors before the gate; the experiment stays
opt-in until then and is sound (crash-free) for the whole corpus, not just the
opted-in set.

> **Progress note (2026-07-22c) -- reactor/CPS async blocker RESOLVED; the
> async/reactor family is corruption- and leak-free flag-on.** Flag-on, every
> heap fat-closure env is headered (env[-1] drop-glue, fat pointer PAST it), so a
> bare free of the fat pointer is an interior free. Three teardown sites still
> did that; each is now header-aware, and `httpd-async-mw-compose` flipped
> leak-clean (marker dropped, `flags: --enable=closure-drop-glue`; suite 2264/0,
> flag-off byte-identical):
>
> 1. **httpd handler teardown** (`httpd-async-free`) -- the user handler box was
>    bare-freed; now `TUR_CLOSURE_DROP(ha->handler)` (as the blocking `httpd-free`
>    already did). `accept_clos`/`body_closure` stay plain `free` -- they are
>    hand-rolled `{ __fn, ptr }` boxes with no header.
> 2. **CPS boundary reap** (`__dk_reap_ptr` / `__dk_reap_run`, emitted) -- a
>    boundary-reaped closure env (`cps_closure_env_freeable`, e.g.
>    `(let [c (fn ...)] (await (async c)))`) was reaped with a kind-0 bare free.
>    Added reap kind 2 = "headered closure" (released via `TUR_CLOSURE_DROP`,
>    walking owning captures); the `reap_env` emit site uses it flag-on. The DK
>    runtime keeps its exact 2-kind form flag-off -- byte-identical.
> 3. **Reactor / fiber-group owned boxes** (`owns_cb` in `tur_reactor_free`,
>    `owns_body` in `tur_local_fiber_group_free`, precompiled libturi C that
>    cannot name the per-program `tur_closure_drop`) -- both bare-freed a headered
>    callback box. Fixed with a runtime flag `tur_closure_headers_enabled`
>    (**weak** 0 default in libturi; a flag-on program emits a **strong** `= 1`
>    override -- no extern+constructor, so a flag-on program that never links
>    reactor.o still resolves the symbol). When set, the reactor releases an owned
>    box through its header (recovering `env[-1]`, walking captures) instead of an
>    interior free. Sound because every box the reactor/fiber OWNS is an emitted
>    (headered) closure -- httpd's hand-rolled boxes are all disowned
>    (`tur_reactor_disown_cb` / `tur_local_disown_body`).
>
> **Progress note (2026-07-22b) -- `httpd-mw-compose-of` LANDED leak-clean
> flag-on; the three leak sources resolved without a per-apply uniqueness
> analysis.** The earlier note pinned three sources; each is now handled by the
> soundest available mechanism, and the fixture is LSan-clean flag-on (stdout
> matches, suite 2264/0, flag-off byte-identical):
>
> 1. **Closure chain** -- `mw-tag`'s handler param marked `^fat next` (it IS a fat
>    closure handle), so the drop-glue walk frees the wrapper onion, exactly as in
>    `httpd-mw-compose`. Fixture annotation only.
> 2. **Cons spine** (`__tur_cons_of`) -- `compose-middleware-of` owns the fresh
>    `& mws` list and only walks it, so it now frees the cells after the fold via a
>    fixed-arity inline-C helper `httpd-mw-free-spine` (frees the cells, leaves the
>    head VALUES). A stdlib change, flag-off too (the spine was a genuine leak);
>    the cells are always distinct so this needs no uniqueness analysis.
> 3. **Transient factory heads** (`make_hymw`) -- reclaimed CALLER-side, not
>    callee-side. The plan's unsoundness worry (a `(compose-middleware-of base m m)`
>    freeing a consumed factory twice) is specific to a *callee-side per-apply*
>    free. Freeing each factory at the CALLER's scope exit is per-binding-once, so
>    a value passed twice is still freed exactly once -- the aliasing hazard never
>    arises. Wiring (all gated on `--enable=closure-drop-glue`, flag-off
>    byte-identical): (a) `compose-middleware-of`'s rest param is `^borrow & mws`
>    -- a new `TY_FN.rest_borrow` bit set from a `^borrow` immediately before `&`
>    (elab_fns.c); (b) `binding_escapes_impl` (emit_core.c) treats a fresh closure
>    passed in the rest `EX_CONS_LIST` of a `rest_borrow` variadic as non-escaping,
>    peeling the carrier casts on each element; (c) `returns_fresh_closure` now
>    peels a trailing `(let [...] <closure>)` wrapper so a factory like
>    `(defn make-mw [tag] (let [_t tag] (fn [n] (mw-tag _t n))))` is recognised as
>    fresh-closure-returning. The existing `let_binding_env_freeable` then frees the
>    factory envs at the caller's scope exit via `TUR_CLOSURE_DROP`. Sound because
>    the factory captures are scalar-Copy (borrowed cstr, copied into the wrappers),
>    so the surviving chain is unaffected. `requires.no-leak-check` DROPPED; the
>    fixture carries `flags: --enable=closure-drop-glue`.
>
> **Progress note (2026-07-22) -- #1b LANDED: Drop-typeclass dispatch in the
> closure drop-glue; `httpd-mw-cors` leak-clean flag-on.** The drop-glue now
> releases an owned Drop-typeclass capture, and mw-cors captures owned `String`.
> Suite 2254/0.
>
> - **Codegen (flag-gated, zero flag-off churn):** `Closure` gains
>   `capture_drop_insts` (resolved at elaboration -- `elab_fns.c` via
>   `typeclass_env_lookup_typeclass`/`_instance`; NULL-init at all 3 non-zeroing
>   construction sites). A Drop-implementing capture is MOVED into the env (an
>   opaque Drop type has NO scope-exit auto-drop, so retaining a second owner would
>   leak the source -- moving makes the closure the sole owner, releases once, and a
>   second capture is `TUR-E0005`). The `capture_clone_insts` retain path was
>   REMOVED after finding the retain model leaks the source for auto-drop-less
>   opaques. rc/ref and `^fat` captures keep their own arms (no double-handling).
>   The drop-glue releases via the resolved instance
>   (`__inst_Drop_drop_<T>(env->field)`, `emit_fns.c` + `emit_expr.c` fallback).
> - **httpd:** `httpd-cors-own-str` returns owned `String` (`string/from-cstr`);
>   `mw-cors-with` captures `String` and passes `string/to-cstr` borrow views to the
>   emit helpers; `httpd.tur` loads `stdlib/string.tur`. `httpd-mw-cors` flipped to
>   `--enable=closure-drop-glue`, marker dropped (leak-clean, stdout matches,
>   flag-off byte-identical).
>
> **Remaining leakers -- now categorized by ROOT CAUSE (not all the String
> recipe).** Profiling each flag-on:
> - **`httpd-mw-cors` -- FIXED** (String captures, #1b).
> - **`httpd-mw-compose` -- FIXED** by marking `mw-tag`'s handler param `^fat next`
>   (it IS a fat closure handle -- `httpd-call` dispatches it as one -- so `^fat`
>   lets the is_fat walk free the chain; behavior-neutral flag-off). The fixture's
>   `next : int` was simply under-annotated.
> - **`httpd-mw-cors-opts` (64B) -- FIXED.** The `(mw-cors-opts opts)`
>   PARTIAL-APPLICATION head desugars to `(let [<pre-applied args>] <EX_CLOSURE>)`
>   and is bound by the call-head hoist (`elab_call_head_expr`) as a `__call_head`
>   let. `let_binding_env_freeable` now admits such a fresh partial-app head (its
>   `__pap` thunk returns the underlying fn's result, never its own env, so freeing
>   it at scope exit can't alias-UAF), freeing it via TUR_CLOSURE_DROP. Flipped
>   flag-on, marker dropped.
> - **`httpd-mw-compose-of` (120B) -- THREE leak sources, one of them unsound to
>   auto-fix.** Investigated in full:
>   1. **Closure chain** -- fixed by `^fat next` on the middlewares (as mw-compose).
>   2. **Cons spine** (`__tur_cons_of`) -- the fresh `& mws` rest-list cells.
>      SOUNDLY fixable: `compose-middleware-of` owns the fresh list and
>      `httpd-mw-fold` only walks it, so the callee can free the cells after the
>      fold (a spine-free that leaves the head VALUES, which live on in the chain).
>      A prototype confirmed this reclaims the spine with correct stdout flag-off.
>   3. **Transient factory heads** (`make_hymw`, 72B) -- each `& mws` element is a
>      middleware FACTORY `(fn [next] ...)` that `httpd-mw-fold` applies ONCE and
>      discards; its env leaks. This is the `__pap` shape, BUT unlike a partial-app
>      head (provably fresh/inline) a fold list element has NO uniqueness guarantee:
>      `(compose-middleware-of base m m)` applies the same factory twice, so freeing
>      a consumed factory would UAF the duplicate. Freeing-after-apply is therefore
>      UNSOUND without per-element uniqueness/linearity analysis on the rest list.
>   So mw-compose-of is a genuine blocker: (1)+(2) are sound but insufficient alone
>   (LSan still aborts on the remaining (3), eating buffered stdout), and (3) needs
>   move/uniqueness tracking over variadic-rest elements. Left marked.
> - **`httpd-mw-cookie` (12B) / `-form` (26B)** -- OUT OF SCOPE for the closure
>   drop-glue: the leaks are request-scoped strings from `httpd-req-cookie` /
>   `httpd-req-form` (each returns a fresh malloc'd `cstr` the handler never frees).
>   An httpd request-accessor ownership cleanup, unrelated to closures.
> - **`-fold-many` / `-rate-limit`** -- non-closure buffer/state leaks (out of scope).
>
> The reactor/CPS async blocker (`__dk_reap_ptr`, `owns_cb` -- precompiled libturi
> C) still needs a runtime-API change before `httpd-async-*` is safe flag-on.
>
> **Progress note (2026-07-21i) -- #1a Drop typeclass LANDED; #1b codegen
> integration spec (turn-key).** The `Drop` typeclass + `Drop[String]` instance
> are in and verified (`(drop s)` -> `__inst_Drop_drop_String` -> `string/release`,
> leak-clean; no snapshot churn -- Drop instances emit only when referenced). What
> remains for the cors cluster is wiring the closure drop-glue to dispatch a
> capture's Drop instance. Exact recipe:
>
> - **Resolve at ELABORATION** (typeclass method resolution is an elab-time thing;
>   emit has no `TypeClassEnv`). In `elab_fns.c` capture finalization (~4642, beside
>   the `^fat` move-marking), for each capture:
>   `Sym drop = intern_cstr(st,"Drop"); TypeClass *dtc =
>   typeclass_env_lookup_typeclass(&e->typeclass_env, drop);
>   TypeClassInstance *di = typeclass_env_lookup_instance(&e->typeclass_env, dtc,
>   &cap->type, 1);` -- likewise `Clone` -> `ci`.
> - **Store on `Closure`** (add two parallel `TypeClassInstance **` arrays,
>   `capture_drop_insts` / `capture_clone_insts`, NULL per capture with no Drop).
>   Init to NULL at ALL THREE Closure construction sites (arena is NON-zeroing):
>   `elab_fns.c:4636`, `elab_call.c:3268`, `elab_call.c:6029`.
> - **Emit** (emit_fns.c drop-glue + emit_expr.c fallback, in lockstep; and the
>   env-fill in emit_expr.c): for a capture with `capture_drop_insts[i] != NULL`,
>   the symbol is `raw_name_for_binding(inst->method_impls[0]->binding)`.
>   - If `capture_clone_insts[i] != NULL` (Drop+Clone, e.g. String -- refcounted):
>     RETAIN at fill `clone_sym(field)`, RELEASE in drop-glue `drop_sym(field)`.
>     Sound without move analysis (refcount balances) -- exactly the rc-capture
>     pattern, generalized through the typeclass methods.
>   - If Clone is absent (move-only Drop): move-mark the capture at elab (like
>     `^fat`) and RELEASE in drop-glue, no retain.
> - **httpd refactor** (separate, after the wiring): `httpd-cors-own-str` returns
>   `String` (`string/adopt-cstr` of the strdup, or `string/from-cstr`);
>   `mw-cors-with` captures `String`; pass `string/to-cstr` views to
>   `httpd-mw-cors-emit-preflight`/`-decorate` (they keep taking `cstr`). Then flip
>   `httpd-mw-cors` (+ `-cors-opts`, `-cookie`, `-form`, `-compose`) to flag-on and
>   drop markers as each goes leak-clean.
>
> **Progress note (2026-07-21h) -- httpd harvest (7 fixtures leak-clean flag-on)
> + the cors cluster's remaining blocker pinned to a NEW mechanism.** With the
> handler teardown routed through `TUR_CLOSURE_DROP` (2026-07-21, prior commit), a
> profile of the whole `httpd-mw-*` family showed the teardown+walk already
> reclaims every fixture whose only leak was the env chain. Flipped to
> `--enable=closure-drop-glue` + markers dropped (suite 2254/0): `httpd-mw-log`,
> `httpd-mw-basic-auth`, `-basic-auth-attr`, `-basic-auth-noncapture`,
> `-body-size`, `-json`, `-static`.
>
> Still leaking flag-on, with the cause now pinned per fixture:
> - **Owned-cstr CORS cluster** (`httpd-mw-cors` 48B, `-cors-opts`, `-compose`,
>   `-compose-of`, `-cookie`, `-form`): the env chain is freed; what remains is the
>   `httpd-cors-own-str` strdup'd strings, captured as `cstr`. The type system sees
>   `cstr` (borrowed), so no walk can know they are owned.
> - **Larger/non-closure** (`-fold-many` 4504B/211 allocs, `-rate-limit`
>   24640B/2 allocs): buffer/state leaks, not closure envs -- out of scope for the
>   closure walk.
> - `-compress`: a `requires.spices` skip (missing zlib dep), unrelated.
>
> **KEY FINDING for the cors cluster (do not attempt a quick walk-extension).**
> The strings should become owned `String` (an `(defopaque String :ptr<void>)`,
> refcounted, with `string/retain`/`string/release` and an O(1) `string/to-cstr`
> borrow view -- so the emit helpers keep taking `cstr`). BUT: there is NO `Drop`
> typeclass and NO droppable-opaque machinery -- a `String` LOCAL is not even
> auto-released at scope exit today (release is manual), and codegen has no
> principled way to know "this opaque capture is owned, release it with X." So the
> cors slice is really **two pieces**: (1) a compiler mechanism -- a `Drop`
> typeclass (`(drop [x] : void)`) that a `defopaque` implements and that the
> closure drop-glue dispatches through for a capture whose type has a `Drop`
> instance (for `String`, `(drop [x] (string/release x))`), with retain-at-capture
> since `String` is refcounted (reuse the rc retain/release path); then (2) the
> httpd stdlib refactor (own-str returns `String`, mw-cors captures `String`, pass
> `string/to-cstr` views to the emit helpers). Piece (1) is a real typeclass+codegen
> feature -- do NOT special-case the name "String" in the compiler (the codebase
> has zero such precedent and it is the wrong shape).
>
> **The reactor/CPS async blocker is unchanged and separate:** `reactor.c`
> `owns_cb` free + CPS `__dk_reap_ptr` bare-free a headered handle and are
> PRECOMPILED libturi C that cannot use the codegen `TUR_CLOSURE_DROP` macro ->
> they need a runtime-API change (thread a drop fn through callback registration)
> before the async httpd family (`httpd-async-*`) is safe flag-on.
>
> **Progress note (2026-07-21g) -- Model R type-honesty (a): the `^fat`
> nested-closure walk LANDED (move-gated, sound). The middleware
> `(fn [next] (fn [conn] ...))` shape now frees its whole chain.** Three parts,
> all flag-gated:
>
> - **Identify (type-honesty).** A `^fat` capture is the erased int64 carrier in
>   the env FIELD, but the capture BINDING keeps `is_fat` (it propagates through
>   the `(let [_n next])` re-bind, elab_forms.c:878). So `cap->is_fat` distinguishes
>   an owned fat closure handle from a scalar int -- no new type needed, the signal
>   already survives. (The generalized fix -- carrying `^fat` as an owning-closure
>   TYPE rather than a flag-on-int -- is still worthwhile but not required for this.)
> - **Move (soundness).** Capturing a `^fat` handle now MARKS the source consumed
>   (`binding_mark_moved`, elab_fns.c), mirroring Model U's store-into-fn-field
>   move.  A SECOND capture of the same handle -- which would double-free -- is a
>   compile-time use-after-move (TUR-E0005), so the env is the handle's sole owner.
> - **Walk.** `drop_glue_<env>` releases each `is_fat` capture via TUR_CLOSURE_DROP
>   (uniform across representations thanks to 2026-07-21f), excluding the letrec
>   self-capture.  Releasing the outer handle frees the whole chain.
>
> Fixtures: `closure-drop-glue-fat-capture` (a `wrap`/`base` chain freed through
> the outer handle -- leak-clean, no double-free) and
> `errors/closure-drop-glue-fat-alias` (double capture -> TUR-E0005). Suite 2254/0;
> flag-off byte-identical.
>
> **What this still does NOT do (remaining for an httpd marker to drop):**
> 1. **Rewire httpd/reactor/CPS teardown.** httpd's `free(handler)` (and the
>    reactor `owns_cb` free, and `__dk_reap_ptr`) still bare-free a now-headered
>    handle -- so httpd cannot be built flag-on yet without corruption. Rewiring
>    them to `TUR_CLOSURE_DROP` requires the macro to be emitted unconditionally
>    (flag-off it expands to the identical `free`), which churns every codegen
>    snapshot -- a mechanical same-PR regen, deferred out of THIS slice to keep it
>    churn-free.
> 2. **The CORS strings.** mw-cors captures strdup'd strings as `cstr` (not
>    walked); they need owned `String` captures. Orthogonal to the closure walk.
> 3. **Cross-function double-ownership caveat.** The move closes same-function
>    aliasing; a caller that independently frees a handle it also passed in would
>    still double-free. Not a current pattern (flag-off such handles just leak), but
>    a general fix wants the move to thread across the call boundary.
>
> **Progress note (2026-07-21f) -- Model R: uniform header across every fat
> representation (`__tur_fatshim` / poly-to-fat boxes), + the type-honesty wall
> pinned for the nested-closure walk.** Two findings:
>
> - **Landed: multi-representation headers.** The prepend-header now also wraps the
>   `__tur_fatshim` box (bare-fn-to-fat, `{shim, orig_fn}`) and the poly-to-fat box
>   (`{shim, fn, env}`), flag-on. Both own nothing walkable, so their header is
>   NULL and `tur_closure_drop` frees the base. This makes `TUR_CLOSURE_DROP`
>   release ANY fat handle uniformly regardless of representation -- the
>   prerequisite for a walk that recurses a captured handle without statically
>   knowing which representation it is (the documented "give the `__tur_fatshim`
>   box its own header so a bare-fn handler is safe" item). Fixture
>   `closure-drop-glue-fatshim` (a bare fn shimmed to `^fat` then TUR_CLOSURE_DROP'd)
>   is leak-clean, no corruption. Suite 2252/0; flag-off byte-identical.
>
> - **Pinned: the nested-closure walk is blocked on TYPE-HONESTY, not just move
>   analysis.** Auto-deriving the walk needs to identify which captures are owned
>   closure handles. But the middleware `_n` capture comes from a `^fat next : int`
>   parameter -- erased to `TY_INT`, indistinguishable from a scalar. No type
>   predicate can pick it out, and `TY_PTR_VOID` is ambiguous (any pointer). So the
>   walk cannot fire on the httpd shape until `^fat` is carried as an owning-closure
>   TYPE (un-erased) rather than an `int` carrier -- a type-system change. A
>   type-HONEST `boxed TY_FN` capture COULD be walked (move-gated), but such
>   captures are rare in the corpus.
>
> - **Flag-on free-site boundary (audited).** Header-on programs are sound only
>   where every fat-handle free routes through `TUR_CLOSURE_DROP`. Still doing a
>   bare free of a (now-headered) handle, and therefore needing header-aware
>   rewiring before flag-on is safe for those programs: the CPS boundary reap
>   (`__dk_reap_ptr`, `emit_cps_ir.c`), the reactor `owns_cb` free
>   (`src/async/reactor.c`), and httpd's `free(handler)` (`stdlib/httpd.tur:949`).
>   The flag stays opt-in for simple programs (no CPS-reap / reactor / httpd
>   teardown of headered handles) until these are rewired.
>
> **Progress note (2026-07-21e) -- Model R walk slice: rc-capture retain/release
> (the unconditionally-sound half of the owning-capture walk).** The env drop-glue
> now WALKS refcounted owning captures, so an rc-capturing closure participates in
> the rc lifecycle regardless of escape:
>
> - **Retain at capture** (`emit_expr.c` env-fill): flag-on, storing an rc-typed
>   capture emits `rc_strong_increment(env->field)` -- the closure holds its own
>   strong count.
> - **Release in drop-glue** (`emit_fns.c` + the `emit_expr.c` fallback, kept in
>   lockstep): `drop_glue_<env>` emits `rc_strong_decrement` + `rc_free_queue_drain`
>   per rc capture (reverse order) before freeing the base.
> - **Soundness:** this is finding-#1's "retain when duplicated" for the capture
>   kind where it needs NO move/uniqueness analysis -- rc counting is aliasing-safe,
>   so retain+release always balances. An ESCAPING rc-capturing closure that
>   flag-off dangles (the constructor's auto-drop frees the rc out from under it)
>   is now correct flag-on: the retain keeps the rc alive for the closure's life
>   and the drop-glue releases it exactly once.
> - **Verified:** fixture `closure-drop-glue-rc-capture` (`make-counter` returns an
>   rc-capturing closure; `strong-count` reads 1 while held; released on
>   TUR_CLOSURE_DROP) is leak-clean, no UAF, no double-free flag-on. Generated C
>   shows the paired increment/decrement. Full suite 2251/0 (flag off -> a snapshot
>   re-emits byte-identically; the two new fixtures run flag-on).
>
> **Still to do in the walk:** the NON-refcounted owning captures -- a raw
> nested-closure handle (the middleware `_n`), a `ref` -- still are NOT walked,
> because recursing one blind is the finding-#1 double-free without move/uniqueness
> analysis. That analysis (or refcounting the env, Model R proper) plus the httpd
> type-honesty layer (owned `String` CORS captures) is what remains before an
> httpd/reactor marker can drop.
>
> **Progress note (2026-07-21d) -- Model R ABI FOUNDATION landed behind
> `--enable=closure-drop-glue` (experiment, off by default).** The runtime
> drop-glue header + generic-release plumbing is in; the move-aware owning-capture
> WALK and the httpd type-honesty layer are the remaining increments. What landed:
>
> - **Experiment** `closure-drop-glue` (`src/runtime/experiments.c`,
>   `g_opt_closure_drop_glue`, prototype, introduced 0.30.1 / expires 0.34.0).
>   Off by default -> the base language is byte-for-byte unchanged (verified: a
>   sample snapshot re-emits identically; full suite 2250/0 with the flag off).
> - **Prepend-header ABI (contained variant).** Flag-on, every heap
>   `struct __env_N` is allocated as `malloc(sizeof(void*) + sizeof(env))` with the
>   fat pointer handed back PAST an 8-byte header; `env[-1]` holds the env's
>   `drop_glue_<env>` pointer. `fat[0]` dispatch, capture-by-field access, and the
>   escaping handle are all byte-identical to the headerless layout -- chosen over
>   inserting a slot at `[1]` precisely because `[1]` is the fn slot in the
>   `__tur_fatshim` / `tur_poly_fn_t` representations, so an inserted slot would
>   collide (emit_module.c:678, :5910). Box internals stay untouched; only `[-1]`
>   is new. (`src/compiler/emit_expr.c`, `src/compiler/emit_fns.c`.)
> - **Generic release.** `TUR_CLOSURE_DROP(h)` (preamble, emitted only under the
>   flag) recovers `h[-1]` and calls it; `drop_glue_<env>` frees the base
>   allocation. The scope-exit env free (`let_binding_env_freeable`) routes through
>   it flag-on. Fixture `closure-drop-glue-model-r` (a `flags` file enables the
>   experiment) proves construction+dispatch+generic-drop is leak-clean and
>   corruption-free.
>
> **Remaining increments (both needed before any httpd/reactor marker can drop):**
>
> 1. **Move-aware owning-capture WALK.** `drop_glue_<env>` currently frees only the
>    base -- it does NOT recurse owning captures, because doing so blind is the
>    finding-#1 double-free (a captured closure/rc the caller still owns would be
>    freed twice). The walk must gate on move/uniqueness (`is_moved` /
>    `is_unique_consumed` / a fresh capture-clone) so it recurses ONLY when the env
>    is the capture's sole owner. This is what actually reclaims a middleware
>    chain's inner `_n` env.
> 2. **httpd type-honesty + teardown wiring.** The httpd handler reaches an opaque-C
>    holder as a type-erased `:int` and its CORS strings are secretly-owned `cstr`;
>    neither is walkable until `_n` is a recognizable owned-closure capture and the
>    strings are owned `String`. Then rewire httpd's `free(handler)` (~line 949)
>    and the reactor callback frees to `TUR_CLOSURE_DROP`, and give the
>    `__tur_fatshim` (rep-2) box its own header so a bare-fn handler is safe. Until
>    every env free site on those paths is header-aware, the flag stays opt-in for
>    simple programs (no reactor/httpd/CPS-env-reap).
>
> **Progress note (2026-07-21c) -- S1 + S2/Model U closeout; Model R scoped &
> deferred.** After the 2026-07-21b marker sweep, a full audit of every remaining
> `requires.no-leak-check` fixture (rebuilt suite-faithfully, LSan on) established
> that NO tractable closure-env leak remains -- the only closure-family opt-outs
> left are the httpd/reactor server closures, and their sound elimination is
> blocked on the Model R ABI, not on any missing S1/S2 analysis. Concretely:
>
> - **Type erasure is the wall.** `compose-middleware-of` returns `:int`
>   (`stdlib/httpd.tur`), so at `(httpd-new-async 0 composed)` the compiler sees a
>   bare `:int` handler with no env type -- it cannot emit or select the right
>   `drop_glue_env_N`. Model U (the holder's *generated* drop glue frees the
>   field) requires the holder to be a Turmeric struct/ADT whose field type names
>   the closure; httpd's holder is an opaque C `struct { int64_t handler; ... }`.
> - **Sound-without-clone, for these captures.** The httpd chain's captures are
>   either scalar (`donech`) or uniquely-owned-fresh (the `httpd-cors-own-str`
>   strdup'd strings; the `_n` next-closure handle, moved in) -- none alias an
>   outer owner that also drops them, so a walk-glue free of them would NOT
>   double-free. The blocker is purely *dispatch* (how opaque C names the glue),
>   not the finding-#1 capture-clone hazard. That hazard remains real for the
>   general case (capturing a live `rc`), so a general owning-capture walk-glue
>   still needs capture-clone first -- but the httpd family does not.
> - **Decision:** do not force the Model R env-layout ABI change speculatively
>   (it perturbs every closure's env struct + `TUR_APPLY` dispatch assumptions +
>   HKT poly-thunk recovery + `tur_poly_fn_t` + WASM glue, and churns every
>   closure snapshot) to reclaim a documented process-lifetime allocation. Keep
>   the httpd/reactor markers; land Model R only when that family is explicitly
>   prioritized, using the sketch below.
>
> **Model R -- ready-to-execute sketch (contained variant).** Because httpd
> handlers are monomorphic one-word envs (not poly/HKT), the change can be scoped
> to the plain `struct __env_N` path and leave `tur_poly_fn_t` untouched:
>
> 1. **Env header:** emit `struct __env_N { int64_t __fn; void (*__drop)(void *);
>    <captures> }`. `__fn` stays at offset 0 so every `fat[0]` dispatch
>    (`TUR_APPLY*`, httpd/reactor C) is unchanged; captures shift by one word but
>    are always field-accessed, so lifted thunks are unaffected. Audit for any raw
>    `fat[1]`/offset-1 capture read (there should be none).
> 2. **Glue:** emit `drop_glue_env_N(void *p)` per env type -- free owning `cstr`
>    captures, recurse `((struct{int64_t __fn; void(*d)(void*);}*)cap)->d(cap)`
>    into nested fat-closure captures, then `free(p)`. Fill `tmp->__drop =
>    drop_glue_env_N` at construction (or `NULL` for a scalar-only env -> bare
>    free).
> 3. **Teardown:** replace `httpd.tur`'s bare `free((void *)handler)` (~line 949)
>    and the reactor callback frees with
>    `{ void(*d)(void*) = ((...__drop layout...)handler)->__drop; if (d) d((void*)handler); else free((void*)handler); }`.
> 4. **Capture-clone (only if generalized):** for the general owning-capture case
>    (an env capturing a live `rc`/`ref`/nested closure the caller still owns),
>    add capture-time retain/clone at the env fill BEFORE enabling walk-glue on
>    that capture kind, per finding #1. NOT needed for the httpd family.
> 5. **Regen** every closure snapshot in the same PR (env struct + `__drop` fill).


> **Progress note (2026-07-21b) -- S2 exit-gate marker cleanup: six
> `requires.no-leak-check` markers DROPPED (now verified leak-clean under LSan;
> suite 2249/0).** The landed S1/S2 drop machinery has made the escaping /
> HOF-passed value-closure fixtures ASan-clean, so their leak-check opt-outs are
> stale and removed. Each was rebuilt exactly as `tests/run.sh` does
> (`-O2 -L<tur>/src`, ASan/LSan-instrumented output binary) and confirmed to emit
> ZERO LeakSanitizer output:
>
> - **S1/S2 exit-gate fixtures** (`free-lift-bind`, `unsafe-closure-capture`,
>   `cps-backend-fn-param`): the plan's "become ASan-clean and DROP their
>   `requires.no-leak-check` markers" gate item -- now satisfied. (The residual
>   free-monad `Suspend` ADT 16 B leak that kept `free-lift-bind` marked is also
>   gone.)
> - **S2 stored-closure fixture** `hkt-stdlib-parser-instances`: the closure
>   stored in a `Parser` value is now reclaimed -- the "flip ... and become
>   leak-clean" gate item for it is met.
> - **Fat-closure-dispatch regressions** `ascribe-fat-closure-call` /
>   `fat-closure-ascription`: their `make-adder` escaping-env leak, which the
>   marker only ever papered over, is now reclaimed.
>
> Also re-verified: the `make-scaler` minimal repro from
> `escaping-fat-closure-env-leak.md` is ASan-clean, and `currying-effect-partial`
> is green. Full `bash tests/run.sh` = 2249 passed, 0 failed with the six markers
> removed (i.e. those fixtures now run WITH leak detection on).
>
> **Still open (markers RETAINED, correctly):** the **httpd middleware family**
> still leaks -- `httpd-async-mw-compose` (64 B / 5 allocs) and the `httpd-mw-*`
> set (16-64 B each). Root cause confirmed: `httpd.tur` (~line 949) tears down the
> stored handler with a bare `free((void *)handler)`, which reclaims only the
> OUTER env word of a `compose-middleware` chain -- it neither recurses into the
> chained `_n` next-closure envs nor drops the env's OWNING captures (the strdup'd
> `const char *` CORS/header strings). Freeing those needs the `drop_glue_env_N`
> walk-glue AND a way to dispatch it from opaque hand-written C (a drop-glue
> pointer in the fat handle, or Model U with the async server as a
> generated-drop holder) -- both still unbuilt (finding #1: captures are stored
> without a retain/clone, so a naive walk-glue would double-free). So those
> markers stay until the walk-glue + capture-clone unit lands.
>
> **Progress note (2026-07-21) -- local fn-field struct drop LANDED (direct
> path); the "Remaining S2 gap" below is closed for uncolored functions.** A
> by-value struct local that owns a BOXED fn-field now frees that heap fat handle
> at scope exit. Mechanism:
>
> - `elab_forms.c` flags the local (`Binding.drops_fn_fields`) when it passes the
>   SAME moved / consumed / escape guards that admit the existing rc/ref
>   `byvalue-struct-field-leak` auto-drop (`elab_field_is_boxed_fnfield` +
>   `is_binding_consumed` / `is_field_consumed` / `binding_moved_during_init`), so
>   a struct that escapes (returned / moved / consumed) is never flagged.
> - The DIRECT emitter (`emit_let_value`) frees the box via a new
>   `drop_fnfields_<T>(&local)` glue (`emit_module.c`) -- fn-fields ONLY (rc/ref
>   are still discharged by the injected `(defer (drop! (.f o)))`), and NO
>   `free(&local)` (the struct is stack-resident).
>
> Crucially this is **not** a `(defer (drop! (.fn o)))`: an fn-field-drop defer
> reads a fat-fn field that the CPS/DK backend's continuation-capture admission
> rejects, evicting a COLORED fn to the retired direct/fiber path (hard build
> failure -- reproduced on `cps-backend-closure-local` with the defer approach).
> Emitting the free directly in the direct emitter leaves colored functions
> untouched: CPS lowering never runs `emit_let_value`, so a fn-field box in a
> colored fn leaks exactly as it did before local drops existed (no regression,
> no eviction). Uncolored functions release it.
>
> Verified valgrind-clean (definitely-lost 0, exactly-once free, no double-free)
> for: pure-fn-field local (call + drop), mixed rc+fn-field local (rc via defer +
> fn via emit), capturing-closure env box, and the escape case (a returned struct
> is NOT dropped by the producing fn). Suite 2220/0 (one snapshot regenerated:
> `defstruct-field-arrow`, whose local `Cell` fn-field box now frees). Fixture
> `local-struct-fnfield-drop`.
>
> **Still open:** (1) a fn-field box in a COLORED function still leaks (needs the
> CPS backend to admit an fn-field auto-drop, or a scalar-box-pointer capture
> form). (2) A boxed fn-field holding a capturing env with OWNING captures leaks
> those captures (only the env box is freed) -- the S1 walk-glue work. (3)
> Pre-existing, orthogonal: reading an rc field into a var (`(let [s (.r o)] ...)`)
> double-frees the control block -- the field read aliases without an incref while
> both `o`'s field-drop and `s`'s rc-drop decrement it. Filed separately.
>
> **Progress note (2026-07-20f) -- S2 Model U drop glue + move landed for
> fn-fields (rc-wrapped path verified sound).** A boxed fn-field is now an owning
> field: `resolve_ctor_field` sets `needs_drop_glue`, so the holding struct's
> by-value drop glue `free`s the field's heap fat handle (a capturing env, or the
> `{shim, fn}` box for a bare fn). Storing a CAPTURING closure variable into such
> a field MOVES it (`binding_mark_moved` in the constructor arg loop), so aliasing
> -- the same closure in two structs, which valgrind confirmed double-frees the
> shared env -- is now a compile-time `use-after-move` error. A thin fn re-shims
> to a FRESH box per store and an inline closure has no source, so neither is
> consumed. Verified valgrind-clean (0 errors, exactly-once free) for rc-wrapped
> closure structs, thin-fn structs, and rc-cloned structs; the struct-copy path is
> compile-rejected by rc uniqueness. Suite 2219/0 (one snapshot regenerated:
> defstruct-field-arrow). Fixtures `capturing-closure-struct-field` (store+call)
> and `errors/closure-struct-field-move` (aliasing rejected).
>
> **Remaining S2 gap (NOT closure-specific):** a LOCAL by-value fn-field struct
> does not invoke its drop glue at scope exit -- the same local-owning-value drop
> machinery that is deferred for `:heap` structs generally (see
> `docs/archive/drop-glue-shallow-nested-owning-aggregate.md`, verified only via
> rc-wrapping for the same reason). So a closure stored in a plain LOCAL struct
> still leaks its handle (no double-free -- just the pre-existing local-drop gap).
> When local-struct drop invocation lands, this S2 drop glue frees those too with
> no further work.
>
> **Update (2026-07-20e) -- the S2 blocker is FIXED; S2 is now unblocked.** Parts
> 1+2 of `docs/archive/capturing-closure-in-struct-field-segv.md` landed: a
> concrete `(fn ...)` struct/ADT field now uses the fat representation uniformly
> (field type `boxed`; make-struct shims thin fns to fat; field-calls dispatch via
> `TUR_APPLY*`). A capturing closure stored in a struct field now RUNS (no SEGV).
> Suite 2218/0; fixture `capturing-closure-struct-field`. fn-field values are
> intentionally uniformly HEAP-allocated (malloc'd fat handles) so the S2 drop
> glue below can free them uniformly -- so a stored fn/closure currently leaks its
> heap handle (shim box or capturing env) until that drop glue lands. That is the
> remaining S2 work, now buildable on a working store-and-call path:
>   - **S2 Model U:** storing a closure/fn into a struct field MOVES it (source
>     consumed; a second store is a move-check error, preventing the aliasing
>     double-free); the holding struct's drop glue frees the heap fat handle
>     (`free(field)` for the shim box or `drop_glue_env_N` for a capturing env).
>   Without the move check, a closure stored into two structs would double-free,
>   so drop glue must land WITH move semantics, not before.
>
> **Blocker note (2026-07-20d) -- S2 is blocked on a struct fn-field dispatch
> bug, NOT a leak.** Scoping S2 (Model U: a stored closure freed by the holding
> struct's drop glue) surfaced that the store-and-call path does not even work:
> storing a CAPTURING closure in a `defstruct` fn-field and calling it via
> `(.f box)` **SEGVs** -- the fat env pointer lands in the field but the read+call
> emits a THIN function-pointer call (`((R(*)(A))env)(args)`), executing the env
> as code (`emit_expr.c:1153/1470` fat-vs-thin keys on `type.as.fn.boxed` /
> `is_fat`, both false for a field read). A thin top-level fn in the same field
> works; only fat closures crash. Filed as
> `docs/reported/capturing-closure-in-struct-field-segv.md`. S2 CANNOT proceed
> until the field uses the fat representation uniformly (mark the field `boxed`;
> auto-shim thin fns to fat on store -- the "closure-representation-unification
> Phase 0" this plan already names). Freeing a stored closure is moot while it
> mis-dispatches. So the next S2 step is that unification bug, then move + drop
> glue on top.
>
> **Progress note (2026-07-20c) -- S1c fresh-closure-returning CALL args
> (headline `make-scaler` CLOSED).** The other half of S1c landed: a call to a
> fresh-closure-returning fn, passed to a non-retaining fn-param, is now hoisted +
> freed. New `Binding.returns_fresh_closure`, inferred when a fn is elaborated:
> its body is a bare capturing `EX_CLOSURE` with ONLY scalar (Copy) captures and a
> scalar result -- so every call mallocs a fresh, uniquely-owned env whose bare
> `free` is fully safe (no owning capture to double-free, result cannot alias the
> env). `hoist_borrowed_closure_args` (via a shared `arg_is_freeable_closure_source`
> predicate) and `let_binding_env_freeable` both accept such a call arg/init. The
> report's minimal repro `(use-it (make-scaler 2.0))` is now ASan/LSan-clean.
> Suite 2217/0 (3 snapshots regenerated -- kebab-case-capture + two bare-fat --
> where the same hoist now frees a previously-leaked env, all ASan-verified).
> Fixture `closure-env-free-fresh-returning-call`. Guards: a struct-storing
> callee and a fn returning an rc-capturing closure are BOTH correctly left
> unfreed (leak-safe, no UAF). **Still open:** S2 stored/escaping closures
> (httpd middleware, parser combinators; `cps-backend-fn-param`, `free-lift-bind`,
> `unsafe-closure-capture` keep `requires.no-leak-check` -- different shapes, not
> the fresh-consumed-once pattern).
>
> **Progress note (2026-07-20b) -- S1c inferred non-retention (INLINE args).**
> The non-retaining-callee half of S1c landed for INLINE capturing-closure
> arguments. A new `Binding.nonretain_param_mask` records, per fn-typed / `^fat`
> parameter, whether the callee body only CALLS it (inferred at defn elaboration
> via `!closure_binding_escapes(body, param)`). A body containing ANY inline-C is
> excluded (`expr_subtree_has_inline_c`, conservative default-true) -- C text can
> store a param invisibly to the AST, the exact unsoundness that first regressed
> `schema-transform-closure` + the httpd middleware set (they store `^fat` params
> via inline-C). The emit-side escape analysis and `hoist_borrowed_closure_args`
> both consult the mask, so an inline capturing closure passed to a non-retaining
> `^fat`/fn param is now hoisted + freed at scope exit (like the landed `^borrow`
> S1.2 path), no annotation needed. Suite 2216/0; fixture
> `closure-env-free-nonretain-fatparam`; guards verified ASan-clean (freed) and
> leak-safe (struct/inline-C/return retention conservatively NOT freed, no UAF).
> **Still open:** the report's headline `make-scaler` repro passes the closure as
> a CALL result `(use-it (make-scaler ...))`, not an inline `EX_CLOSURE`; hoisting
> a fresh-closure-returning CALL arg (make-scaler's binding already carries
> `returns_closure_fn_binding`) + letting `let_binding_env_freeable` accept a
> `ptr<void>`-typed fresh-env call init is the next slice. S2 (stored/escaping
> closures) unchanged.
>
> **Progress note (2026-07-20).** A second, adjacent leak landed:
> `binding_escapes_impl` (`emit_core.c`) fell to its conservative
> `default: escape` for `EX_DEFER` and the rc/weak/ref-family nodes (`EX_RC_OF`,
> `EX_WEAK`, ...). An owning let-binding lowers its auto-drop to a
> `(defer (drop r))` and its init is `(rc/of ...)`, so BOTH tripped the default
> and flagged every sibling closure as escaping -- a non-escaping closure's env
> leaked (16 B) in any `let` that also bound an `rc`/`ref`, even for a
> scalar-capture closure. Fixed by modeling `EX_DEFER` via its capture set and
> walking the rc/weak/ref operands (strictly more precise; never greenlights a
> free of a referenced env). Suite 2215/0; fixture
> `closure-env-free-with-owning-sibling`; write-up in
> `docs/archive/history/fat-closure-env-free-owning-sibling.md`. This is NOT one
> of the S1/S2 slices below (those are the ESCAPING / inline-HOF-arg cases); it
> is an orthogonal false-escape bug in the same env-free machinery.
>
> **Progress note (2026-07-19).** Verified against the tree: S1.2 is the only
> landed slice. `hoist_borrowed_closure_args` (`elab_call.c:773`), the
> `binding_escapes_impl` FA_BORROW relaxation (`emit_core.c:573`), and the
> `let_binding_env_freeable` scope-exit free (`emit_expr.c:1267`) are all
> present. The **`drop_glue_env_N` walk-glue does NOT exist** (no such symbol
> anywhere in `src/`), and there is no capture-time retain/clone -- so S1 (a/b/c)
> and both S2 models are entirely unbuilt. The `requires.no-leak-check` markers
> still sit on `free-lift-bind`, `unsafe-closure-capture`, `cps-backend-fn-param`,
> and `hkt-stdlib-parser-instances`; `currying-effect-partial` (re-classified out
> of S1) carries no marker. This plan remains **OPEN** -- the ownership feature
> (capture-clone + walk-glue) has not started.

## Landed: S1.2 -- borrowed HOF-arg closure free

A capturing closure passed INLINE to a `^borrow` fn-param now has its heap env
reclaimed at scope exit. `free-lift-bind` / `unsafe-closure-capture` (the
`(free-run (fn [inner] (* inner scale)) ...)` shape) dropped from a 32 B leak to
16 B -- the closure env is freed; the residual 16 B is a SEPARATE free-monad
`Suspend` ADT leak (not a closure), so those fixtures keep `requires.no-leak-check`
for that reason now. Suite 2179/0. Mechanism (no ownership hazard -- these
captures are scalar):
1. `free-run`'s interp param is annotated `^borrow` (it invokes but does not
   retain the closure -- a natural transformation is reused, so the CALLER owns
   and frees it, not the callee).
2. `binding_escapes_impl` (emit_core.c) treats a closure passed to a `FA_BORROW`
   param as NON-escaping (same only-greenlights-a-free posture as the box-accessor
   whitelist).
3. `hoist_borrowed_closure_args` (elab_call.c, applied in the `elab_call_fn`
   wrapper) hoists an inline capturing-closure `^borrow` arg into a fresh
   let-binding, so the existing `let_binding_env_freeable` scope-exit `free`
   reclaims it -- the inline env otherwise has no name to target.

Also fixed a pre-existing latent bug this surfaced: `elab_unsafe` allocated its
`HandleExpr` via `arena_alloc` and never initialized `shallow`, so effect_check
read an uninitialized bool (UBSan `load of value 190`); the arena layout shift
from the hoist made the garbage non-zero. Now `handle->shallow = false`.

Remaining S1: the OWNING-capture case still needs capture-time clone + the
`drop_glue_env_N` walk-glue (Implementation findings below); the `^borrow` free
here is hazard-free only because these payload captures are scalar.

**One-line:** give a captured ("fat") closure's heap env struct a real lifecycle
-- freed when the closure dies, dropped-through when stored, walk-glued when its
captures are themselves owning -- so escaping and HOF-passed closures stop
leaking and the two remaining B2 fixtures (`currying-effect-partial`,
`hkt-stdlib-parser-instances`) CPS-emit instead of evicting on `EX_CLOSURE`.

## What this unblocks

- **The escaping-fat-closure-env leak** (`docs/reported/escaping-fat-closure-env-leak.md`):
  one `malloc`'d `struct __env_N` leaked per capturing-closure construction that
  escapes (returned / stored / passed `^fat`). Currently carries
  `requires.no-leak-check` on `cps-backend-fn-param`, `free-lift-bind`,
  `unsafe-closure-capture`.
- **B2 residuals (2)** in the CPS backend: `currying-effect-partial` (a
  partial-application closure `add10 = (log-add 10)` called in a `Log` handle
  body) and `hkt-stdlib-parser-instances` (closures stored in `Parser` values).
  Both evict on `EX_CLOSURE` today because the closure cannot be admitted without
  a free.
- **The httpd middleware family** (`httpd-async-mw-attr` / `-mw-compose`) and any
  code that stores middleware/handler closures in a chain.

## Current state

A capturing closure lowers to (`emit_expr.c` ~5785):

```c
struct __env_N { int64_t __fn; <captures...> };
struct __env_N *tmp = malloc(sizeof(struct __env_N));
tmp->__fn = <thunk>; tmp->cap0 = ...; ...
```

The fat value carries `tmp` (a one-word env pointer, or a 2-word `tur_poly_fn_t`
for the rank-2 poly protocol). The ONLY free that exists today is
`let_binding_env_freeable` (`emit_expr.c:1267`): a let-bound closure is freed at
scope exit iff it is an **`EX_CLOSURE` literal**, returns a **scalar**, and
**provably does not escape** (`closure_binding_escapes`, conservative -- only ever
greenlights a free). Everything outside that narrow gate leaks:

- a **partial-application** closure (`(log-add 10)` -- init is a CALL, not an
  `EX_CLOSURE` literal),
- a closure passed as a **HOF argument** (`(free-run (fn ...) ...)` -- the arg is
  conservatively flagged escaping),
- a **stored / returned** closure (parser combinators, httpd middleware -- it
  genuinely escapes).

The CPS backend inherits this: it can only admit a capturing closure it can
free, so the un-freeable shapes evict on `EX_CLOSURE`.

## Two sub-problems (different fixes)

The residuals split cleanly by whether the closure ESCAPES its constructor:

**S1. NON-escaping closures that just aren't freed yet.** `currying-effect-partial`
(`add10` called once, locally), the `free-run` HOF args (`free-run` calls the
closure and discards it). These have a single owner and a clear scope-exit death
point; they leak only because the current gate is too narrow (`EX_CLOSURE`-literal
+ scalar-return + a conservative escape check that flags any call argument). Fix
is a scoped free, no ownership tracking.

**S2. ESCAPING closures.** `hkt-stdlib-parser-instances` (the closure is stored in
a `Parser` value that is returned / threaded), httpd middleware (stored in a
chain). Ownership transfers to the holder; the holder must drop it, and a closure
stored in two places must not double-free. Fix needs the closure to participate in
the drop / uniqueness system.

## Design

### The env drop-glue function (shared by S1 + S2)

Emit, per env type that needs it, a `drop_glue_env_N(void *p)`:

```c
static void drop_glue_env_N(void *p) {
    struct __env_N *e = (struct __env_N *)p;
    /* walk-glue: drop each OWNING capture in reverse order, mirroring the
     * ADT/struct drop-glue (emit_module.c emit_adt_byval_drop_glue). */
    <drop e->capK for each owning capture>   /* rc_strong_decrement / drop_glue_* / free */
    free(e);
}
```

- A **scalar-only** env (captures are all Copy scalars) needs no walk -- the glue
  is a bare `free(e)`; the current `let_binding_env_freeable` already emits that
  inline. The glue function matters when captures are themselves owning (an `rc`,
  a `ref`, a NESTED closure -- an env-in-env), exactly the case that leaks worst
  today.
- Reuse the existing owning-value drop machinery keyed off each capture's type
  (`needs_drop_glue`, `rc_strong_decrement`, `drop_glue_<adt>`), so a closure that
  captures an `rc<Foo>` decrements it, and a closure that captures another closure
  recurses into `drop_glue_env_M`.

### S1 -- scoped free for non-escaping closures

1. **Widen `let_binding_env_freeable`**: admit a partial-application closure (init
   is an `EX_CALL` whose `returns_closure_fn_binding` is set -- a curried under-
   saturation producing a closure) and drop the scalar-result restriction where
   the closure result cannot alias the env (needs the walk-glue so a non-scalar
   capture is dropped, not just the env freed).
2. **A HOF-arg free**: a closure passed as a call argument whose callee does NOT
   retain it (`free-run` calls-and-discards) can be freed after the call returns.
   This needs a callee "does not retain fn-param" property -- start conservative:
   a `^fat`/`(fn ...)` param that the callee only CALLS (never stores/returns) is
   non-retaining. `free-run`'s inline-C calls the interp once and returns an int
   -- non-retaining. Emit the env free after the call.
3. **CPS interaction**: the CPS backend already has the boundary-reap mechanism
   (`__dk_reap_ptr`, P3.c/P3.d). A non-escaping closure admitted on the CPS
   delegation path registers its env (and, via the glue, its owning captures) for
   reap at the entry boundary -- the analogue of `cps_closure_env_freeable`
   (which today handles only the scalar-capture let case). Wire the glue so the
   reap drops captures too.

S1 alone clears `currying-effect-partial`, `free-lift-bind`, `unsafe-closure-capture`
(dropping their `requires.no-leak-check`) without any ownership-tracking.

### S2 -- drop glue for escaping closures (the real feature)

An escaping closure is an owning heap value whose owner is the value it is stored
in (a struct field, an ADT payload, a return value). Two sound models:

- **Model U (uniqueness / move) -- preferred for STORED closures.** Plug the
  closure into the existing affine/move system (`is_moved`, `is_linear_consumed`,
  `is_affine`, `CK_MOVE`, the alias-state UT1 machinery). Storing a closure in a
  struct MOVES it (the source binding is consumed); the holding struct's drop
  glue (`needs_drop_glue`) calls `drop_glue_env_N` on the field. No refcount, no
  per-closure overhead; a double-store is a move-check error (as it already is for
  other affine values). This is how `hkt-stdlib-parser-instances` (closure stored
  in a `Parser`) and httpd middleware (closure stored in a chain node) should
  work -- the `Parser` / chain-node drop glue owns the closure.
- **Model R (refcount) -- fallback for genuinely SHARED closures.** Add a
  refcount word to the env (`struct __env_N { int64_t __rc; int64_t __fn; ... }`);
  a clone/dup increments, a drop decrements and runs `drop_glue_env_N` at zero.
  Uniform and sharing-safe, but adds a word + rc ops to every fat closure and an
  ABI change to the fat-closure protocol (the `^fat` layout, the HKT thunk
  recovery, `tur_poly_fn_t`). Reserve for closures the uniqueness model rejects
  (a closure legitimately shared by two owners).

Recommendation: land **Model U** for the stored-closure cases (covers the corpus
residuals) and only reach for **Model R** if a shared-closure fixture appears --
the ABI cost of R is high and the corpus does not yet need it.

## Phasing

Phasing describes ORDER of implementation, not permission to stop between
phases. Phase 1 is the immediate deliverable; Phase 2 follows in the same
push. There is no "land Phase 1 and hand back" -- the exit gate is the
suppressed fixtures going clean, which spans Phases 1 and 2.

- **Phase 1 (S1) -- one atomic ownership unit (see Implementation
  findings).** Order forced by the double-free hazard:
  (1a) capture-time retain/clone for OWNING captures (a bare capture aliases
  today, so an env-drop would double-free), (1b) `drop_glue_env_N` walk-glue on
  top, (1c) the non-retaining-callee (`^once`) annotation + a post-call free hook
  in EX_CALL emission for inline HOF args. 1a+1b are atomic (1a alone leaks MORE;
  1b alone double-frees). Clears the two PD leak fixtures (scalar-capture, so 1c +
  the emit hook, not the walk-glue, is what they need). NOT a "start here quick
  win" -- it is the ownership feature. `currying-effect-partial` is RE-CLASSIFIED
  out of S1 (it is a partial-app of a colored fn -- a B1-style colored closure,
  not a value closure).
- **Phase 2 (S2 / Model U):** closures participate in the move system; struct/ADT
  drop glue drops closure-typed fields via `drop_glue_env_N`. Clears
  `hkt-stdlib-parser-instances` and the httpd middleware family.
- **Phase 3 (S2 / Model R):** refcounted env for genuinely shared closures.
  This is a fallback ALTERNATIVE to Model U, not deferred work on the critical
  path -- Model U is the chosen path and covers the entire current corpus. Model
  R is only built if a fixture appears that Model U's move-check genuinely cannot
  express (a closure legitimately shared by two owners); its ABI cost is why it
  is the fallback, not the default. It is NOT a reason to leave Phases 1-2 open.

## Implementation findings (verified before starting S1)

A tractability pass on S1 established that it is NOT a quick bounded slice -- every
sub-path has either a soundness hazard or needs new analysis/machinery. Three
facts, each verified against the emitter:

1. **Capturing an owning value does NOT clone it.** The env-fill emission
   (`emit_expr.c` ~5793) is a bare `fat_tmp->field = <value>;` per capture -- no
   `rc` increment, no closure retain. So the walk-glue (dropping owning captures
   in `drop_glue_env_N`) is UNSOUND on its own: dropping a captured `rc` that the
   original owner still drops is a double-free. **The walk-glue REQUIRES
   capture-time retain/clone first** (the "retain when duplicated" half of the
   fix). Scalar (Copy) captures are safe (no ownership) -- so a scalar-only env
   drop is a bare `free`, hazard-free; an owning-capture env drop is blocked on
   capture-cloning.

2. **No post-call free hook exists for inline HOF-arg closures.** `free-lift-bind`
   / `unsafe-closure-capture` pass the closure INLINE to `free-run` (not a let
   binding), so `let_binding_env_freeable`'s scope-exit free (the only closure
   free that exists) does not reach it. Freeing it needs (a) a new "free this
   malloc'd env after the enclosing call/statement" mechanism in the EX_CALL
   emission, AND (b) proof the callee does NOT retain the closure -- `free-run` is
   inline-C whose non-retention is not analyzable; it needs a `^once`/non-retaining
   fn-param annotation or a whitelist. Even though these closures capture only a
   scalar (hazard-free to free), the emit hook + non-retention property are real
   prerequisites.

3. **`currying-effect-partial` is a partial-application of a COLORED fn.** `add10 =
   (log-add 10)` where `log-add` performs `Log`; the "closure" performs when
   called, so it is not a value closure at all -- it belongs with the B1-style
   colored-call handling, not S1 value-closure drop. It should be re-classified
   out of S1.

Net revised S1 order: (a) capture-time retain/clone for owning captures, then (b)
`drop_glue_env_N` walk-glue on top of it, then (c) the non-retaining-callee
annotation + post-call free for HOF args. Only step (a) unblocks a hazard-free
`drop_glue_env_N`; steps done out of order double-free.

## Risks / open questions

- **Double-free** is the cardinal risk. The escape analysis
  (`closure_binding_escapes`) is conservative (only greenlights a free), which is
  the right posture -- extend it carefully; a false "does not escape" frees a live
  env. Model U's move-check is the structural guard for S2.
- **Non-retaining callee property (S1.2):** deciding a callee does not retain its
  fn-param. Start with the syntactic "only calls it" rule (covers `free-run`);
  a general effect/escape signature on fn-params is a larger analysis -- keep it
  out of Phase 1.
- **Walk-glue ordering / cycles:** a closure that captures itself (letrec self-
  capture, already handled specially at construction -- `emit_expr.c` "Edge 1")
  must not recurse infinitely in the glue; mirror the ADT walk-glue's
  cycle-awareness or exclude self-captures from the drop walk.
- **Fat-closure ABI (Model R only):** adding an `__rc` word changes the `^fat`
  layout, HKT thunk recovery, and `tur_poly_fn_t`. Audited in
  `docs/archive/fat-closure-abi-audit-plan.md` -- coordinate there if R is ever
  needed.
- **`tur_poly_fn_t` (2-word) vs one-word env:** the drop must free the right
  object for both the plain env-pointer closures and the rank-2 poly-fat
  closures; confirm which allocation each frees.

## Test targets & exit gate

> This was the ORIGINAL S1/S2 exit gate and is now MET -- see the dated progress
> notes and the Status above. The forward gate is R1-R4 in "Remaining work --
> roadmap to graduation" near the top of this file.

- `currying-effect-partial`, `hkt-stdlib-parser-instances` flip from
  `BODY-UNSUPPORTED` (`EX_CLOSURE`) to CPS-emitted (direct == cps == turi). [MET]
- `free-lift-bind`, `unsafe-closure-capture`, `cps-backend-fn-param` become
  ASan-clean and DROP their `requires.no-leak-check` markers. [MET]
- The minimal no-effects repro in `escaping-fat-closure-env-leak.md`
  (`make-scaler`) is ASan-clean. [MET]
- httpd middleware fixtures stay green and leak-clean. [MET for the flipped 12 +
  the async/reactor family; remaining markers are R1 or non-closure -- see R1.]
- Full `bash tests/run.sh` green. [MET -- 2264/0]
