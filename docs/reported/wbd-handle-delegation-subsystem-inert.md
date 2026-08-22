# The P5 whole-body handle-delegation subsystem is now inert

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
