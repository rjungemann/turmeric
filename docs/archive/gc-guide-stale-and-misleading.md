---
status: resolved
severity: low
discovered: 2026-07-24
resolved: 2026-07-24
area: docs (docs/guides/gc-guide.md)
---

# `gc-guide.md` overstates cycle collection and the interpreter "leak"

## Resolution (2026-07-24)

All three items fixed in `docs/guides/gc-guide.md`:

1. Added a "What a collection actually reclaims today" note under the intrinsics
   table and rewrote the Known-gaps bullet -- both now state the collector is
   zombie-only and `(gc!)` does not reclaim strong `rc<T>` cycles, pointing at
   `gc-strong-cycles-not-collected.md` and the cycle-collection plan.
2. Reworded the interpreter section from "closures are never freed" to
   "region-allocated, reclaimed wholesale at `turi_env_free`", with the
   long-lived-env growth caveat, and softened the `detect_leaks=0` rationale.
3. Refreshed stale citations -- the interp-collections plans, `asan-debug-leaks-plan.md`,
   and `end-to-end-monomorphization-plan.md` now point at `docs/archive/history/`;
   the `eval.c:435-443` cite is now `eval.c:424-451`.

The original report follows for the record.

## Summary

`docs/guides/gc-guide.md` contains three accuracy problems found during the
2026-07-24 GC study. None affects runtime behavior; all mislead a reader
studying the memory model. Grouped here as a single doc-hygiene report.

## 1. Advertises `(gc!)` as a working remedy for cycles -- it isn't

Known-gaps bullet (`gc-guide.md:216-217`): *"Programs that build cycles need to
call `(gc!)` explicitly, or opt into `GC_THRESHOLD` via `(gc-enable!)`."* As
detailed in `docs/reported/gc-strong-cycles-not-collected.md`, the collector is
zombie-only and cannot reclaim a live strong `rc<T>` cycle by any mode. The
guide should state that `(gc!)` reclaims **weak-zombie** blocks, that strong
cycles are **not** collected today, and that the current remedy is manual
`weak<T>` cycle-breaking (as in Rust). Same overstatement is implied at
`gc-guide.md:89-90`.

## 2. "The tree-walker leaks its closures by design" is misleading

The interpreter section (`gc-guide.md:148-164`) frames closures/frames as a
"genuine leak." They are **arena (region) allocated** from `env->value_scratch`
via `turi_val_alloc` (`value.c:16-18`, `eval.c:428,456`), with per-object free a
deliberate no-op (`eval.c:446-451`), and the **whole pool is reclaimed at
`turi_env_free`** (`env.c:334-336`). That is region memory reclaimed at
teardown, not memory abandoned to the OS. The accurate framing:

- Short-lived process (fork-per-fixture, one-shot `tur build`): fully reclaimed
  at exit -- benign.
- Long-lived env (REPL/kernel that never tears down): the pool grows
  monotonically until teardown -- the real caveat, and the one the current text
  glosses. Incremental reclamation via scratch promotion exists but is opt-in and
  off by default (see `turi-interp-incremental-reclamation-plan.md`).

The `detect_leaks=0` justification (`gc-guide.md:160-164`, "a genuine leak in
the interpreter is expected") should be softened accordingly -- the historically
genuine case (interp Vec/Set/Map buffers) is now swept at teardown
(`env.c:320-327`).

## 3. Stale citations

- `gc-guide.md:156-158` and `:224` cite
  `docs/upcoming/turi-interp-collections-libturi-plan.md` and its siblings as
  active plans; they have shipped and moved to `docs/archive/history/`
  (`turi-interp-collections-libturi-plan.md`, `asan-debug-leaks-plan.md`,
  `interp-collections-never-freed.md`).
- `gc-guide.md:148-149` cites `eval.c:435-443` for the closure-never-freed
  rationale; the code is now at `eval.c:424-451` (`eval_frame_new` /
  `eval_frame_free`).

## Fix directions

Straight doc edits: reword the two overstatements (items 1-2), and refresh the
citations (item 3). Best done alongside the interpreter-reclamation doc update in
`turi-interp-incremental-reclamation-plan.md` TR5. No code change.

## Recommendation

Low severity, high clarity-per-effort. Not v1-blocking, but item 1 is worth doing
promptly on its own -- a reader currently cannot tell that `(gc!)` won't collect
their cycle.
