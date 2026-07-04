# TUR_M7_HKT Flag Retirement Plan

**Status:** proposed (Phase R1 partially verified 2026-07-03; R2-R4 not started)
**Track:** one-track-to-v1 cleanup
**Author:** investigation 2026-07-02

## Progress (verified 2026-07-03)

No code has been retired yet -- the branch surface and the flag itself are
still in place. A re-inspection turned up the following updates to the
"Current state" numbers below:

- **Phase R1 (downstream-dependency sweep):** partially done. `grep -R
  TUR_M7_HKT` across `src/` still shows only the read site
  (`src/main.c:12276-12278`), the default (`src/runtime/globals.c:112`), and
  the extern (`src/runtime/globals.h:83`). All doc references live under
  `docs/archive/` (already-resolved plans) plus this file and CLAUDE.md; no
  live `docs/reported/` item references the flag. The sibling
  `../turmeric-spices/` checkout has **zero** hits for either `TUR_M7_HKT`
  or `g_m7_hkt_enabled`. Deferred reports
  ([../reported/return-directed-methods-pure-empty-inference.md](../reported/return-directed-methods-pure-empty-inference.md),
  [../reported/class-method-level-hkt-tyvar-grounding.md](../reported/class-method-level-hkt-tyvar-grounding.md))
  are still open and neither cites `TUR_M7_HKT=0` as a workaround -- clear
  to proceed.
- **Branch surface has grown slightly, not shrunk.** Current real
  `g_m7_hkt_enabled` conditionals (comments excluded):
  - `src/compiler/elab_typeclasses.c` -- 8 sites (was ~4 areas in original
    inventory; unchanged in shape but counted per-conditional now).
  - `src/compiler/emit_module.c` -- 6 sites.
  - `src/compiler/emit_fns.c` -- 5 sites.
  - `src/compiler/emit_stmt.c` -- 1 site (the `eligible = is_hkt ? ... :
    true` guard).
  - **`src/compiler/elab_call.c:4115` -- 1 site (NEW, not in the original
    inventory).** Fold this into the Phase R2 file list.
  - Total: ~21 real conditionals across 5 files (plus ~7 explanatory
    comments referencing the flag).
- **Phase R2 file order stands**, with `elab_call.c` inserted between
  `elab_typeclasses.c` and the emit-side files (its read is on the
  elaborator side of the pipeline):
  1. `emit_stmt.c` -> 2. `emit_fns.c` -> 3. `emit_module.c` ->
  4. `elab_typeclasses.c` -> 5. `elab_call.c`.
- **Phases R3-R4 not started.** Flag definition, read site, CLAUDE.md
  policy paragraph, and the three archive files still live where they
  were.

Next actionable step: begin Phase R2 by collapsing `emit_stmt.c:464`'s
`is_hkt ? g_m7_hkt_enabled : true` down to `is_hkt` and running the suite
(with the mandatory 600000 ms timeout) to confirm the 65-failure baseline
holds.

## TL;DR

Retire `TUR_M7_HKT` outright. Do **not** move it into the `EXPERIMENTS[]`
registry. The by-value HKT path has been the default since 2026-06-19, both
paths currently produce identical `tests/run.sh` results (1843 pass / 65 fail,
where the 65 failures are unrelated to HKT), and CLAUDE.md already states
the carrier path is "being superseded and may degrade as classes migrate to
by-value -- that is expected, not a regression to chase." A toggle whose
"off" branch is explicitly permitted to rot is not an experiment worth
tracking; it is dead code waiting to be removed.

## Current state (verified 2026-07-02)

- **Read site:** `src/main.c:12264-12267` -- one `getenv("TUR_M7_HKT")` call,
  strict "0" / "1" parsing.
- **Default:** `src/runtime/globals.c:112` -- `bool g_m7_hkt_enabled = true`.
- **Branch surface:** ~27 `g_m7_hkt_enabled` conditionals across
  `src/compiler/elab_typeclasses.c` (partial-app wildcard head reconstruction,
  method-level tyvar handling, return-type threading, inline-instance body
  eligibility), `src/compiler/emit_module.c` (result-type override, per-method
  `__spec` emission), `src/compiler/emit_fns.c` (parametric struct instance
  lowering), `src/compiler/emit_stmt.c` (HKT instance live-code eligibility).
- **Suite parity:** `bash tests/run.sh` and `TUR_M7_HKT=0 bash tests/run.sh`
  both report `summary: 1843 passed, 65 failed`. The 65 failures are all in
  unrelated tracks (fat closures, struct param threading, constrained
  polymorphism, defstruct field handlers).
- **Stdlib:** Functor / Applicative / Monad / Alternative / Bifunctor /
  Foldable signatures live in by-value form on disk today; instances compile
  identically under both paths because the by-value decl parses under carrier
  emit (the feature is gated inside the elaborator, not at parse time).
- **Known deferred issues** (filed 2026-07-02 as part of this investigation;
  do **not** block flag removal):
  - [../reported/return-directed-methods-pure-empty-inference.md](../reported/return-directed-methods-pure-empty-inference.md)
    -- Applicative `pure` / Alternative `empty` need explicit ascription even
    when context could ground the container tyvar.
  - [../reported/class-method-level-hkt-tyvar-grounding.md](../reported/class-method-level-hkt-tyvar-grounding.md)
    -- Traversable-shaped methods (second HKT tyvar introduced by the
    method) fail arm-join unification inside the instance body.

## Why not move it to `EXPERIMENTS[]`

The `EXPERIMENTS[]` registry (`src/runtime/experiments.c`) exists to gate
**in-flight, semantics-in-flux** features behind `--enable=<name>` with
lifecycle warnings (TUR-W0060 / W0061) and hard `expires_at` contracts.
`TUR_M7_HKT` fits none of the three shapes the registry is designed for:

1. **It is not opt-in.** The by-value path is already the default. An
   experiment entry would gate a feature we ship as default -- inverted from
   the registry's purpose.
2. **The "off" branch is explicitly disposable.** CLAUDE.md permits the
   carrier path to degrade; the registry's contract assumes the gated
   feature is what you are stewarding, not the fallback.
3. **There is no user population to warn.** The env var has no CLI form,
   no docs pointing users at it, and its only purpose today is letting
   maintainers A/B a change against a legacy path. That is a git-checkout
   concern, not a compiler-flag concern.

## Retirement plan

### Phase R1 -- confirm no downstream dependency (before touching code)

1. `grep -R TUR_M7_HKT` across the repo and the sibling `../turmeric-spices/`
   checkout. Expected hits: `src/main.c` (the read), `src/runtime/globals.c`
   (the default), `CLAUDE.md` (the policy line), a handful of docs. Any
   fixture or spice that reads it directly gets patched away first.
2. Confirm the two deferred `docs/reported/` items linked in the "Current
   state" section do **not** work around the flag. If either grows a
   "workaround: set `TUR_M7_HKT=0`" note, escalate to a full fix (or
   explicit shelving) before removing the toggle.

### Phase R2 -- collapse the branch surface

For each of the ~27 `g_m7_hkt_enabled` conditionals:

- Keep the **true** branch (by-value / M7 Layer-4 path).
- Delete the **false** branch and any helper it uniquely calls.
- Remove any comments explaining "when M7 is off ...".

Files to touch (in this order to keep intermediate commits testable):

1. `src/compiler/emit_stmt.c` -- live-code eligibility.
2. `src/compiler/emit_fns.c` -- parametric struct lowering.
3. `src/compiler/emit_module.c` -- result-type override + `__spec` emission.
4. `src/compiler/elab_typeclasses.c` -- partial-app wildcard, method-level
   tyvar, inline-instance body eligibility, return-type threading.

After each file: rebuild and run `bash tests/run.sh` with `timeout: 600000`.
Failure count must stay at 65 (or drop; must not rise).

### Phase R3 -- remove the flag itself

1. Delete `g_m7_hkt_enabled` from `src/runtime/globals.c` and its extern in
   the corresponding header.
2. Delete the `getenv("TUR_M7_HKT")` block at `src/main.c:12264-12267`.
3. Update `CLAUDE.md`: replace the "Test suites -- the default (by-value)
   path is the gate" paragraph with a single line noting the carrier path
   no longer exists.
4. Move `docs/archive/end-to-end-monomorphization-plan-2.md`,
   `docs/archive/m7-stdlib-migration-execution.md`, and
   `docs/archive/hkt-dispatch-options-tradeoff.md` under
   `docs/archive/history/` (they are already resolved; this is filing, not
   deletion).

### Phase R4 -- verify

- `bash tests/run.sh` with `timeout: 600000`. Expected:
  `summary: 1843 passed, 65 failed` (same 65 as today).
- Confirm the deferred `traverse` / `pure` / `empty` reports at
  `docs/reported/` still repro and still have accurate root-cause notes.

## What if the carrier path IS load-bearing after all?

If Phase R1 turns up a downstream dependency on `TUR_M7_HKT=0` (a spice, a
CI job, a maintainer workflow), the fallback is:

1. Keep the flag through v1.
2. Do **not** migrate to `EXPERIMENTS[]` -- the mismatch is real. Instead,
   add a one-paragraph section to CLAUDE.md ("Legacy carrier fallback")
   naming the dependency and its removal date.
3. Track the dependency in a new `docs/reported/tur-m7-hkt-carrier-<slug>.md`
   file and remove the flag when that item resolves.

But: nothing surfaced in this investigation to justify that path. The
expected outcome is straight retirement per Phases R1-R4.

## Non-goals

- Fixing the 65 unrelated failures. Those live in their own tracks.
- Fixing the deferred `pure`/`empty` and method-level HKT tyvar items
  linked above. They are independent of the flag and stay in
  `docs/reported/` until their own PRs.
- Reviving carrier-only optimizations. There aren't any -- the carrier path
  is strictly the older, less-typed emit strategy.
