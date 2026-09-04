# The P5 whole-body handle-delegation subsystem is now inert

> **RESOLVED 2026-08-22.** Removed, after the premise was verified two
> independent ways (see "Verification"). `tests/run.sh` 2694 passed / 0 failed,
> `tests/run-turi.sh` 1857 passed / 0 failed, and **no `expected.c` snapshot
> moved** -- which is the check that matters for a claim that a code path could
> never run. Net -201 lines in `cps_ir.c`.

**Severity: low (dead code, no behaviour at stake).** Surfaced 2026-08-22 while
retiring `g_opt_cps_tramp_resume` (see
`docs/archive/graduated-cps-gates-still-branched-on.md`).

## Summary

`cps_ir.c`'s `safe_to_delegate` had an `EX_HANDLE` arm that pushed a deep
handle's case effects onto `g_wbd_handled` and recursively checked whether the
whole handle could be delegated to the direct/fiber emitter. Since
`cps-tramp-resume` graduated (2026-07-19) that arm returned `false` before
reaching the push -- a deep handle must DK-lower -- so the push was unreachable,
and folding the graduated gate deleted it.

That push was the **only** writer of `g_wbd_handled` / the only place
`g_wbd_n_handled` is incremented. The two remaining assignments are a save and a
reset to 0 (`cps_ir.c:4207-4213`). So `g_wbd_n_handled` is now permanently 0,
and everything keyed on it is dead:

| Site | Reads as | Now always |
|---|---|---|
| `wbd_effect_handled()` | loop over `0..g_wbd_n_handled` | `false` |
| `case EX_PERFORM` | `!wbd_effect_handled(...) -> false` | `false` |
| `case EX_RESUME` | `g_wbd_n_handled > 0 && ...` | `false` |
| `colored_call_wbd_delegatable` (`:769`, `:806`) | `if (g_wbd_n_handled == 0) return false;` | early `false` |
| `:1792` | `g_wbd_n_handled > 0 && ...` | `false` |

`g_whole_body_delegate` itself is still set (`:4209`) and still read at `:850`,
`:1640`, `:1662`, `:1895`, `:1908`, so the flag is live even though the
handled-effect table it works with is empty. The subsystem is therefore not
*entirely* dead -- which is exactly why this wants reading rather than a
mechanical sweep.

## Why it was not removed with the gate fold

The gate fold was provably emission-identical and the suite confirmed it
(2693 passed, no snapshot movement). This cascade is a different claim -- that
a whole delegation path can never fire -- and proving it means reading each
`g_whole_body_delegate` site to see which ones still do useful work with an
empty table. Bundling that into the fold would have made a clean diff
unreviewable.

## Fix directions

Work outward from `g_wbd_n_handled`: delete `g_wbd_handled`,
`WBD_MAX_HANDLED`, `wbd_effect_handled`, and the five constant-false sites
above, then re-read the remaining `g_whole_body_delegate` uses and decide
whether the flag still earns its keep or collapses too.

Expect no emission change -- if a snapshot moves, the premise is wrong and the
path was reachable after all, which is the useful signal here.


## Verification (2026-08-22)

The report asserted a path could never execute. That is a stronger claim than
"this branch is constant", so it was checked two independent ways before
anything was deleted:

1. **By induction on the assignments.** After the `EX_HANDLE` push was folded
   away, the only writes to `g_wbd_n_handled` were `= 0` and a restore of a
   value read from the variable itself. It starts at 0 (static storage), so it
   is 0 forever.
2. **Empirically.** A one-shot probe was compiled into every read site
   (`wbd_effect_handled`, both `g_wbd_n_handled == 0` early-outs, and the
   `g_wbd_n_handled > 0` tightening) and `tur emit-c` was run over all **2119**
   fixtures. It never fired.

## What was removed

`WBD_MAX_HANDLED`, `g_wbd_handled`, `g_wbd_n_handled`, `wbd_effect_handled`,
`row_concrete_all_wbd_handled`, `row_nonempty_all_wbd_handled`,
`fnvalue_call_wbd_delegatable`, and -- once their last live callers went with
those -- `fndef_of_binding` and `expr_fn_effect_row`.
`colored_call_wbd_delegatable` collapsed to its one live test (the leaf-fiber
self-recursive call). `EX_PERFORM` and `EX_RESUME` joined the
control-operator group in `safe_to_delegate`, and the entry point stopped
scoping a stack that no longer exists.

## Two things that are NOT dead, and nearly went anyway

The report flagged that `g_whole_body_delegate` was still read at six sites and
that this needed reading rather than a mechanical sweep. That was right, and two
specific traps showed up:

- **`g_whole_body_delegate` itself stays.** The closure-only whole-body
  delegation it guards -- a function "colored" only because it builds a
  capturing closure, delegated so the direct emitter's scoped-env free applies
  and the env does not leak -- never used the effect stack. It is still read at
  `EX_DEFER` and in the leaf-fiber classification.

- **`EX_HANDLE` is not unconditionally false.** Its `(unsafe ...)` MARKER branch
  (`if (h->is_unsafe_marker) return safe_to_delegate(b, h->body);`) is live and
  runs constantly -- every `unsafe` block in the language is one. Only the
  *real*-handle tail below it was dead. Folding `EX_HANDLE` into the
  control-operator group alongside `EX_PERFORM`/`EX_RESUME` was tried and caught
  by the compiler (duplicate case), but it would otherwise have silently stopped
  every `unsafe` body from delegating. **The lesson generalizes: a case arm that
  ends in an unconditional `return false` is not necessarily an unconditionally
  false arm.**
