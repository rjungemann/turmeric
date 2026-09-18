---
status: RESOLVED 2026-09-17 -- fix direction 1, with one correction to the filing
severity: medium (performance of every interpreter session that carries a
  refinement crossing; the playground and `tur repl` paid it on every turn, and
  a session rebuild paid it once per replayed turn)
discovered: 2026-09-16
area: elaborator (RT1 call-site crossings under an ElabSession)
---

# A long-lived session re-resolved every refinement call site it had ever seen, on every turn

## The mechanism, confirmed

`refine_resolve_call_sites` (`src/compiler/elab_fns.c`, called from
`elaborate_program_session`) walked `e->refine_call_sites[0 .. n)`. Under an
`ElabSession` that array is session state -- copied in with the rest of the
`Elab` (`e = *sess`) and saved back -- and nothing cleared it. Every turn
re-resolved every crossing collected by every earlier turn.

The waste was not only the walk. `refine_collect_obligation`
(`src/compiler/refine_collect.c:105`) does not deduplicate, so each
re-resolution minted a **fresh, undischarged** obligation, which was then
discharged again. The RT7 memo does not save that: it reuses only the verdict,
deliberately --

> "Only the VERDICT is reused -- diagnostics still run below, so a repeated
> unproven obligation reports at its own source location."
> (`refine_discharge.c:504`)

-- so the report ran again too. Which turns that actually reached depends on
the crossing: a TUR-E0371 fails its own turn and the session is discarded with
it, but a crossing with no runtime backstop (a `#reads` measure) or any session
under `--strict-refine` re-emitted TUR-W0372 on every later turn.

## Correction to the original filing

**The repro as filed collects zero obligations.** It is 300 turns of
`(defn f<i> [x : int] : int (+ x <i>))` -- no refinement anywhere -- and
`refine_resolve_call_sites` early-`continue`s on a crossing where neither side
has a predicate, before any of the `rt_collect_*` work the filing's profile
names. Measured directly on the same `libturi_wasm` preload the filing used:

```
440 plain `defn` turns  ->  refine_stats()->collected == 0 for the whole session
```

So the 49%-of-samples attribution cannot have come from that repro as written,
and the cost is **not** paid by an ordinary session. `refine_note_call_site` is
called for every direct call to any `TY_FN` binding, so the ARRAY grows with
every call in the session either way -- but walking it is two pointer
dereferences per entry until a predicate is involved. What the filing got right
is the mechanism and where it lives; what it got wrong is who pays.

The session shape that does pay needs a refinement crossing. A refined callee
plus one crossing into it per turn:

```turmeric
(defn takes-pos [n : #refine{ x : int | (> x 0) }] : int n)   ; turn 0
(defn cross<i>  [x : int] : int (takes-pos (+ x <i>)))        ; turns 1..N
```

`(import refine)` is rejected at a prompt ("import is only allowed inside
defmodule"), which is why the refinement is spelled inline rather than as `Pos`.

## Measured

Obligations collected over N turns that each add exactly one crossing -- the
deterministic number, and the one the regression test asserts:

| turns | before | after |
| --- | --- | --- |
| 60 | **1830** (= 60x61/2, the running total) | **60** (one per turn) |

Wall clock, same machine back to back, Debug + ASan (so the absolute numbers are
inflated; the ratio is the point):

| | before | after |
| --- | --- | --- |
| turns 0-39 | 4.39 ms/turn | 0.13 ms/turn |
| turns 200-239 | 18.11 ms/turn | 0.52 ms/turn |
| turns 400-439 | 48.52 ms/turn | 0.23 ms/turn |
| one *trivial* turn at depth 440 | 27.1 ms | 0.12 ms |

The trivial turn is the clearest statement of the defect: a turn that added
nothing paid for all 440 crossings before it. An earlier run of the same probe,
on a busier machine, measured 184 ms for it.

## Fix

Direction 1, as filed. `turn_start_n_refine_call_sites` joins the PS4
watermarks on `Elab`, is set at session restore, and
`refine_resolve_call_sites` starts there. Zero for a compile and for a
session's first turn, so the whole-program path is untouched.

The safety question the filing asked to check first -- can a crossing recorded
in an earlier turn need re-resolution when a later turn defines its callee? --
resolves no, on two counts:

- The crossing is only recorded for an `fn_binding` that is already `TY_FN`,
  and pass 1 forward-declares everything the same unit defines, so nothing it
  depends on arrives later than the end of its own turn. A turn that referenced
  an unbound name errored, and a failed turn discards the session.
- A later turn redefining the callee **shadows it with a new `Binding`**
  (`elab_fns.c`, the `elab_prior_turn_global` branch), so `cs->callee` still
  points at the definition the crossing actually crossed into. Re-checking turn
  N's call against turn N+1's function would be a diagnostic about code that
  already ran, under a definition that did not exist when it did. Pinned by
  `tur_session_redefinition`, which still passes.

Direction 2 (clearing the array) was not taken: the array is arena-allocated
and the dedup hash table (`refine_cs_htab`) indexes into it, so clearing means
maintaining both, where a watermark is one integer and leaves the existing
`refine_fill_call_site_env` watermark pattern intact.

`refine_obs`, which the filing also asked about: `refine_discharge_all`
early-outs on `ob->discharged`, so re-walking the accumulated vec was always
cheap. What was expensive was the *fresh* obligations the re-resolution minted,
and those stop at the source.

## Regression test

`tests/wasm_glue_session_unit.c` (ctest `tur_wasm_glue_session_unit`) counts
obligations rather than timing anything -- `refine_stats()->collected` is
deterministic and a clock is not, and `g_stats` is per-process here because only
the compiler driver resets it. 60 crossing turns must add between 60 and 240
obligations. Against the unfixed loop it reports `1830 obligations over 60
turns` and fails.

## Not fixed here

A plain-`defn` session still shows some per-turn growth (roughly 0.09 ms at turn
20 to 1.1 ms at turn 420 on the same Debug+ASan build) with **zero** obligations
collected, so it is somewhere else in the turn -- the linear scans in
`scope_lookup` / `elab_prior_turn_global` are the obvious candidates, but that
was not measured and is not claimed. Worth its own filing if it matters; it is
two orders of magnitude below what this report was about.

The three sibling deferred passes run from the same block in
`elaborate_program_session` and have the same session-cumulative shape --
`wf_resolve_write_frames` and `rf_resolve_read_frames` are each a fixed point
over ALL accumulated sites (`rounds = n + 1` times an inner loop over `n`, so
quadratic per turn), and `wf_lint_image_globals` walks every accumulated root
with a `scope_lookup` and a whole-body walk each. They are gated on
`n_* == 0` and only `#writes` / `#reads` / `with-image-cache-after-init` code
records anything, so an ordinary session pays nothing -- but a session that uses
those annotations has this same defect, worse. Not fixed here because nothing
has reported it and the safety argument above is per-pass, not shared.
