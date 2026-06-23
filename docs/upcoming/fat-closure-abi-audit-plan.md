---
status: phases-0-3-landed
owner: unassigned
created: 2026-06-23
updated: 2026-06-23
---

# Fat-closure / `c-fn` ABI audit and paydown

## Motivation

`tourist/middleware.tur`'s `use!` takes `(c-fn [Ctx] (Option Response))` and
stuffs the handler into a single `int64_t` slot inside `struct
__tourist_route { char *method; int64_t pattern; int64_t handler; }`. A fat
closure (code pointer + env) cannot survive that round-trip; the env is
silently dropped. This came up in user review as a "what else is shaped like
this?" question — bare-pointer storage where a fat-closure was expected is a
quietly-corrupting class of bug, not a loud one.

A spice + stdlib + compiler survey (see "Survey baseline" below) shows the
damage is **mostly contained**: the canonical fat-closure dispatch path
(`TUR_APPLY{0..4}` in `src/compiler/emit_module.c:4829-4855`) is used
correctly almost everywhere a function-valued slot is stored. The two real
gaps are:

1. **Tourist's route/middleware storage shape** — a single `int64_t handler`
   field where a `{fn_ptr, env_ptr}` pair is needed.
2. **The type system silently accepts the lossy ascription** — `(fn ...)`
   coerces to `(c-fn ...)` (or `:int`) at the call to `use!` without
   diagnostic, so a captures-using closure compiles, runs, and corrupts
   memory or crashes when the env is read as a `NULL`-or-stale pointer.

The structural fix is small and local; the type-checker fix prevents the
next instance.

## Survey baseline (what we already know)

Categorized by whether the site already uses the fat-closure protocol
correctly:

**Correct (stores fat closure or separate env field, dispatches via
`TUR_APPLY*` or equivalent):**

- `stdlib/reactor.tur:76-107` + `src/async/reactor.c:50-138` — fd/timer/
  signal/chan callbacks. Separate `tur_cb` and `tur_user_data` fields, fat
  dispatch.
- `stdlib/image.tur:181, 207` — reload/finalize hooks. `^fat`, dispatched
  via `TUR_APPLY0`.
- `stdlib/httpd.tur:645, 743, 782, 2728, 2944` — `httpd-new*`, `router-add`.
  `^fat`, dispatched via fat-closure protocol.
- `stdlib/either.tur:122, 145, 165` — combinators. `^fat`.
- Effect handler dispatch — `src/compiler/emit_module.c:5143-5165, 6994`,
  stores `{void *env; int64_t (*fn)(...);}` pair.

**Intentionally bare (documented "no captures"):**

- `stdlib/map.tur:156-250` — `keyeq : (c-fn [K K] bool)`. Docstring forbids
  closures.
- `stdlib/test.tur:119` — `register-test` takes `ptr<void>`.
- `stdlib/thread.tur:49` — `thread-spawn-fn` takes `ptr<void>`; the
  high-level `thread-spawn` wraps it in a lambda.

**Defective:**

- `../turmeric-spices/spices/tourist/src/tourist/dsl.tur` — `route-new`
  stores handler as `int64_t` in `struct __tourist_route`.
- `../turmeric-spices/spices/tourist/src/tourist/middleware.tur:69` —
  `use!` takes `(c-fn [Ctx] (Option Response))` and feeds `route-new`. Same
  defect for `use-after!` (line ~155).
- `route-call-handler` (in `dsl.tur`) calls the handler as a bare code
  pointer, with no env arg.

**Unresolved contradiction (Phase 0 — RESOLVED 2026-06-23):**

- Archived `docs/archive/httpd-middleware-plan.md:55` claimed `EX_CATCH_UNWIND`
  passes `NULL` env. **Phase 0 verified: archive was right; survey was wrong.**
  Root cause: `catch_thunk_to_fat` in `elab_concurrent.c:349` was wrapping a
  capturing closure (`TY_FN { boxed: true }`) through `EX_FN_TO_FAT`, which
  double-boxed the already-fat closure and dropped the env.
  **Fixed 2026-06-23**: guard the auto-shim on `!thunk->type.as.fn.boxed`;
  capturing closures pass through to `TUR_APPLY0` unchanged. Regression
  fixture: `tests/fixtures/panic-catch-unwind-captures/`. See archived
  report: `docs/archive/catch-unwind-drops-captures-segv.md`.

## Scope

In scope:

- Audit and either fix or **explicitly document as "no captures"** every
  function-valued slot in the spice tree and stdlib that doesn't yet make
  its closure-ness contract explicit.
- Make the type checker reject (or at minimum warn on) `(fn ...)` →
  `(c-fn ...)` ascription so the lossy direction has a diagnostic, not
  silent miscompile.
- Widen `__tourist_route.handler` (or refactor route storage) so middleware
  + route handlers can be fat closures, then drop the `(c-fn ...)`
  signatures on `use!` / `use-after!` / route registrations.

Out of scope (for this plan):

- The broader monomorphization track ([[project_monomorphization_north_star]])
  may eventually make `c-fn` vs fat-closure distinctions irrelevant; this
  plan does **not** wait on that. Land the audit + diagnostic + tourist fix
  now; the structural ABI rework subsumes it cleanly later.
- Reactor / httpd / image hooks — already correct, only get test coverage
  added to lock in the contract.

## Phases

### Phase 0 — Verify the `EX_CATCH_UNWIND` claim (1 hr)

Read `src/compiler/emit_expr.c` (or wherever `EX_CATCH_UNWIND` lowers) and
confirm whether the env passed into the thunk call is `NULL` or the fat
closure box. Write a fixture that captures a variable into a `catch-unwind`
thunk and reads it back; expect either green (archive was stale) or a real
crash (archive was right; add to Phase 2 fix list).

Archive `docs/archive/httpd-middleware-plan.md` accordingly — either annotate
"resolved" or carry the panic-recovery fix into Phase 2.

### Phase 1 — Type-checker diagnostic (TUR-E0292) (1-2 days)

Add a type-checker error/warning when a fat closure (a `(fn ...)` value
carrying an env) is ascribed to a bare-pointer function type (`(c-fn ...)`,
or `:int` / `:ptr<void>` in a position annotated as callable). This is the
primary preventative — most of the structural bugs were possible only
because the ascription was silent.

- New diagnostic `TUR-E0292`: "fat closure cannot be ascribed to bare C
  function pointer type; captured environment would be dropped".
- Suggested fix in the message: change the parameter type from `(c-fn ...)`
  to `(fn ...)`, or hoist the captures out so the value really is bare.
- Fixtures under `tests/fixtures/typecheck-fat-to-cfn-rejected/` and a
  positive fixture confirming a bare lambda (no captures) still ascribes.
- Audit existing call sites flagged by the new diagnostic; for each, decide
  whether the slot should become a fat closure (Phase 2 work) or whether
  the API was correct and the caller just needs to drop captures.

### Phase 2 — Tourist route/middleware fat-closure widening (2-3 days)

- Replace `struct __tourist_route { char *method; int64_t pattern; int64_t
  handler; }` with a shape that stores the fat closure box (single
  `int64_t closure;` field is sufficient — the box already contains both
  fn and env pointers).
- Rewrite `route-call-handler` to dispatch via the same protocol the
  reactor uses (extract slot 0, cast, call with box as env). Mirror the
  pattern from `src/async/reactor.c:118-138`.
- Change `use!` / `use-after!` / `get!` / `post!` / `delete!` / `put!` /
  `patch!` / `head!` / `options!` parameter types from `(c-fn [Ctx] ...)`
  to `(fn [Ctx] ...)`.
- Drop `compose-middleware`'s macro-only workaround (see
  `docs/archive/history/variadic-rest-closure-cast-plan.md`) and re-ship it
  as a real function once fat closures flow through.
- Unblock panic-recovery middleware (`docs/archive/httpd-middleware-plan.md`
  table row, line 55) if Phase 0 confirms it was real.
- Tourist fixtures: capture a logger handle, a db handle, and a `next`
  continuation into middleware; assert each survives a request round-trip.

### Phase 3 — Contract documentation sweep (0.5 day)

For every entry classified "intentionally bare" in the survey, confirm the
docstring says so in the same shape `map.tur` does ("Must carry no
environment / captures forbidden"). Currently inconsistent:

- `stdlib/test.tur:119` — `register-test` — no contract note.
- `stdlib/thread.tur:49` — `thread-spawn-fn` — no contract note.
- Anywhere else flagged by Phase 1's diagnostic that we decide to keep
  bare.

Adding the docstring line is cheap and keeps Phase 1's diagnostic from
reading as a "compiler is being annoying" surprise.

### Phase 4 — Spice-wide regression net (1 day)

One fixture per spice that registers a callback, capturing at least one
local into the closure and asserting the captured value is observable after
dispatch. Cheap, mechanical, and prevents a future regression of the
"silently bare" class.

## Risks / non-goals

- **Don't gold-plate.** This is paydown, not a redesign. If a slot is
  correct today, leave it; only add a docstring contract note.
- **Don't block on monomorphization.** The long-term ABI work
  ([[project_monomorphization_north_star]]) will likely retire `c-fn` as a
  user-visible type. Phase 1's diagnostic and Phase 2's tourist widening
  both compose cleanly with that direction; neither becomes wasted work.
- **Snapshot churn.** Phase 2 will regenerate tourist + httpd-related
  fixtures; budget for the regen in the same PR per the project's no-deferred-regen
  rule.
- **API break.** Phase 2 changes the public `use!` / `use-after!` signature
  from `c-fn` to `fn`. Tourist is pre-1.0 (currently v0.2.x per
  `docs/archive/spices-standalone-fetchability.md`); call it a minor bump
  rather than a back-compat shim.

## Links

- [[project_monomorphization_north_star]] — long-term ABI direction; this
  plan is a near-term subset.
- `docs/archive/ascribing-fat-closure-value-to-fn-type-double-shims.md` —
  prior occurrence of the lossy-ascription pattern.
- `docs/archive/history/variadic-rest-closure-cast-plan.md` — why
  `compose-middleware` shipped as a macro; unblocks in Phase 2.
- `docs/archive/httpd-middleware-plan.md` — panic-recovery blocked-on note,
  resolved or carried in Phase 0.
- `docs/archive/spices-int-stand-in-audit-2026-06-14.md` — adjacent
  "everything-is-`:int`" cleanup; same shape of defect, different axis.
