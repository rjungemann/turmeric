---
status: open
severity: medium
discovered: 2026-07-08
discovered-by: catch-unwind-aggregate-returns-plan AR2 valgrind
area: compiled backend / runtime (stackless catch-unwind lowering)
---

# Stackless `catch-unwind` leaks the caught Result box per catch level, even when discarded

## Summary

The **stackless (trampolined) `catch-unwind` lowering** builds a
`tur_result_box_t` at every catch resume (`tur_box_ok(__v)` / `tur_box_err(...)`)
and **never frees it** -- `definitely lost` under valgrind, one 24-byte box per
catch level. In a catch-crossing recursion (the case the stackless path exists
for) this is bounded per catch site but **unbounded over depth**: 50 levels leak
50 boxes, a 200,000-deep run leaks 200,000.

The finding is the **asymmetry with the native path**. The resolved report
`docs/archive/catch-unwind-result-box-leak.md` added a statement-position
`tur_result_box_free` so a native `catch-unwind` whose Result is *provably
discarded* frees its box. The stackless lowering never got that counterpart:
`gs_catch_descend` emits the box and delivers it into the sink (including a
`(void)`-discard sink) with no `tur_result_box_free`. So a discarded catch that
would be leak-free on the native path leaks on the stackless path.

Note the stackless path does **not** leak the 16-byte thunk fat-closure that the
native path leaks (tracked in `docs/reported/catch-unwind-thunk-closure-leak.md`)
-- the stackless lowering re-emits the thunk body inline as a segment, so no fat
closure is materialized. The two paths leak *different* blocks; this report
covers the stackless result box.

Severity medium: long-standing (not a regression -- it is the deferred G5
"no-leak" item from the archived general-lowering plan), bounded per catch site,
but a genuinely unbounded leak under deep catch-crossing recursion.

## Minimal repro

Stackless (trampolined -- catch crosses the recursion), catch result discarded:

```turmeric
(defn go [n : int] : int
  (if (= n 0)
    0
    (do
      (catch-unwind (fn [] : int 0))   ; result discarded
      (go (- n 1)))))
(defn main [] : int (println (go 50)) 0)
```

```sh
TUR=./build/tur
$TUR build /tmp/st_leak.tur -o /tmp/st_leak
valgrind --leak-check=full /tmp/st_leak
# ==> definitely lost: 1,200 bytes in 50 blocks   (24 B tur_result_box_t x 50 levels)
```

Contrast: the same discard on the **native** path frees the box (only the
16-byte thunk closure leaks, per the other report):

```turmeric
(defn main [] : int
  (catch-unwind (fn [] : int 0))
  (catch-unwind (fn [] : int 0))
  0)
;; ==> definitely lost: 32 bytes in 2 blocks   (16 B thunk closure x 2; box FREED)
```

A caught panic additionally keeps its 32-byte `tur_panic_payload` alive on the
stackless err branch (boxed into the err result, matching native's box; not
freed here).

## Root cause

The stackless catch resume segment builds the box unconditionally and never
frees it:

- `src/compiler/emit_fns.c:1313-1323` (`gs_catch_descend`): emits
  `int64_t __box<id>; if (tur_panicking) { ... __box<id> = tur_box_err(...); }
  else { __box<id> = tur_box_ok(__v); }`.
- The box is then routed to the catch's sink at `emit_fns.c:1336`
  (`gs_deliver(gs, rb, boxnm, sink)`). For a **discarded** catch (statement
  position) the sink is `GSK_SEQ`, which emits only `(void)(__box<id>);`
  (`emit_fns.c:1172`) -- no free.

There is no stackless counterpart to the native statement-position free at
`src/compiler/emit_stmt.c:177`
(`tur_result_box_free((int64_t)(intptr_t)<box>);`, helper defined at
`src/compiler/emit_module.c:6611`). `tur_result_box_free` frees the
`tur_result_box_t` and, for an err box, the `tur_panic_payload` *struct* (not
`payload->value`, which may be an inline scalar).

## Fix directions

- In `gs_catch_descend`, when the catch's value is **provably discarded** --
  i.e. the sink is `GSK_SEQ` (or, more generally, a `GSK_ASSIGN`/`GSK_RETURN`
  whose bound Result does not escape) -- emit
  `tur_result_box_free(__box<id>)` before/at the discard, mirroring
  `emit_stmt.c`'s statement-position free. The `GSK_SEQ` case is the safe,
  obvious win: the box is literally `(void)`-cast, so freeing it there cannot be
  premature.
- The harder case (a `let`-bound caught Result that is *used* then dropped
  without escaping) needs the same escape/last-use analysis the parent report
  flags as an open design question, and is shared with the native let-bound-box
  case in `catch-unwind-thunk-closure-leak.md`. Do not free a box the resume
  stores into a saved temp and re-emits into an enclosing expression (`temp_vi
  >= 0` branch) without that analysis -- it may flow into the function's
  returned `Result`.
- Reuse the existing `tur_result_box_free` helper; do not hand-roll a free (the
  err-branch payload-vs-value distinction is subtle and already handled there).

## Related

- Parent (resolved, native half): `docs/archive/catch-unwind-result-box-leak.md`.
- Sibling (native thunk closure + let-bound box): `docs/reported/catch-unwind-thunk-closure-leak.md`.
- Distinct stackless aggregate-box panic-unwind leak (Part B): `docs/upcoming/catch-unwind-aggregate-followups-plan.md`.
