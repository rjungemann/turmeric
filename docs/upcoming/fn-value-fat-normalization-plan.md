# Fn-value fat normalization -- one calling convention for non-carrier fn boundaries

**Status:** proposed. This is the plan the investigation in
`docs/reported/poly-result-hof-capturing-closure-sigbus.md` said the fix
wants ("it is a calling-convention change across every non-carrier fn-typed
parameter, so it wants its own plan and a full-suite regen, not a patch"),
plus the sibling findings that arrived since
(`docs/reported/fn-typed-value-return-ascribe-miscompiles.md`). No `--enable`
gate: this is a miscompile fix converging on existing checked behavior, not a
new user-facing feature -- the experiment machinery does not apply, the
regen-coordination rules do.

## Problem

A fn-typed value today travels in one of (at least) six representations,
chosen independently per boundary: the `tur_poly_fn_t {env, fn}` carrier,
`^fat` handles, `:ptr<void>`-fat sinks (with an `is_fat` dispatch flag),
nominal bare `TY_FN` code pointers, struct-field-fat, and (visible in emitted
C) a by-value fat struct in parameter slots. Producer and consumer each pick
a form with no shared rule, so specific compositions pass `tur check` and
then emit invalid C or crash:

- capturing closure -> nominal thin `TY_FN` param (tyvar arg/result, by-value
  struct arg/result, effect row, `^linear`/`^borrow`): SIGSEGV/SIGBUS
  (poly-result report, mechanism confirmed -- the callee jumps into the env
  struct).
- fn-typed value RETURNED through a pass-through param: invalid C thin
  (`return (int64_t)(intptr_t)v;` on an aggregate) and SIGSEGV `^fat`.
- ascription around a let-wrapped closure value: SIGSEGV.
- `^fat` HOF over a nested fn type `(fn [] (fn [] T))`: SIGSEGV.

See `docs/guides/value-representations-guide.md` for the full inventory and
the missing-cells table.

## Direction

Do for non-carrier fn-typed *parameters, returns, and let/ascribe positions*
what was already done for `defstruct` fn-typed *fields*
(`tests/fixtures/capturing-closure-struct-field/`), because that fix is the
existence proof:

> Concrete `(fn ...)` fields now use the fat representation uniformly ...
> the make-struct store shims a bare/thin fn into a fat `{thunk, env}`
> handle, and every field-call dispatches via the fat protocol (TUR_APPLY).

Invariant after this plan: **a fn-typed VALUE crossing any boundary that is
not the carrier is a fat `{thunk, env}` handle, always.** Bare top-level fns
and non-capturing lambdas are shimmed into fat handles at the boundary (env =
NULL or a sentinel; the shim is the existing struct-field one). Every invoke
of a non-carrier fn value dispatches via the fat protocol. The carrier stays
exactly as-is for carrier-eligible signatures -- this plan does not widen
carrier eligibility (`fn_type_has_named_tyvar` exists because widening it
miscompiled by-value struct args; see `poly-hof-constrained-arg-baked-carrier`).

## Stages

1. **Param normalization.** Non-carrier fn-typed parameters take the fat
   handle: call sites shim bare/thin values in, callee invokes fat. Sites:
   `src/compiler/emit_expr.c:4536` (nullary nominal-`TY_FN` invoke) and the
   n-ary sibling ~4550; the shim exists in the struct-field store path.
   Kills the whole poly-result crash table (tyvar arg/result, by-value
   struct arg/result, effect row, `^linear`/`^borrow` rows).
2. **Return/let/ascribe positions.** A fn-typed value returned from a defn,
   bound by `let`, or passed through `(:: e T)` keeps the fat handle -- no
   thin re-casts on the way out (`return (int64_t)(intptr_t)v` on an
   aggregate is the current failure). Kills the
   fn-typed-value-return-ascribe matrix rows, including nested
   `(fn [] (fn [] T))`.
3. **Unify the flag'd sinks.** `:ptr<void>`-fat's `is_fat` runtime flag and
   the struct-field-fat protocol become the same code path as stages 1-2
   where practical; at minimum, document any residual difference in the
   representations guide.
4. **Regen + acceptance.** Full fixture regen in the same PR (this will
   move every snapshot that touches closures). Acceptance tests:
   - the full matrix from `fn-typed-value-return-ascribe-miscompiles.md`,
     ok rows included;
   - the poly-result crash table as fixtures;
   - `python3 tests/type-fuzz-src.py --known-probes` shows the three
     fn-value probes `FIXED`;
   - retire the corresponding `known_bug_slug` rows in
     `tests/type-fuzz-src.py` (the thunk-crossing avoid rule and the
     thin-hof by-value rule), returning those shapes to the default fuzz
     pool -- then a `--n 500` session on two fresh seeds stays green;
   - verify `van-laarhoven-lens-*`, `hkt-cata-fn-arg-carrier`,
     `local-struct-fnfield-drop`, and `capturing-closure-struct-field` still
     pass -- the fixtures the abandoned compile-time diagnostic falsely
     fired on; they pin today's correct fat behavior and must not move.

## Costs / risks

- **ABI churn:** every non-carrier fn boundary changes emitted C; expect a
  large snapshot regen (coordinate timing per the fixture-churn rule -- one
  regen window, same PR).
- **Perf:** fat dispatch adds an indirection for previously-thin bare fns at
  non-carrier boundaries. These boundaries are exactly the ones that today
  crash on closures, so the population is small; benchmark `tur run bench`
  before/after and note the delta in the PR.
- **Interpreter parity:** turi already treats closures uniformly; verify
  with `tests/run-turi.sh` rather than assuming.

## Non-goals

- Widening carrier eligibility (explicitly rejected; see above).
- A compile-time diagnostic for unrepresentable combinations -- tried,
  abandoned, and documented in the poly-result report (six false positives
  on correct fixtures); normalization makes the combinations representable
  instead of rejected.
- `generic-closure-return-type-app` Defect A/B (checker-side type-app
  erasure and missing ctor emission) -- same neighborhood, different layer;
  its report keeps its own fix directions.

## Doc follow-up

Update `docs/guides/value-representations-guide.md` in the landing PR: the
closure zoo collapses to carrier + fat, the missing-cells table loses its
closure rows, and the two closure reports move to `docs/archive/` with their
links corrected. (Each report's "Guide upkeep" section says the same from
the other side.)
