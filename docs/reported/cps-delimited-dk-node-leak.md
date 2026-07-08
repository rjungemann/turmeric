# Delimited-control lowering leaks DK chain nodes per reset/shift

**Severity:** low (bounded, per-reset/shift-execution leak; not a correctness
bug). Blocks running affected programs under LeakSanitizer without a
`requires.no-leak-check` marker.

## Summary

The abortive delimited-control lowering allocates DK continuation-chain nodes
(`dk_shift` / `dk_prompt` / `dk_frame` / `dk_done`) for each `reset`/`shift`
execution and never frees them. `DK` is an opaque type outside
`src/runtime/cps_prompt.c`, so emitted code cannot detach a single node
(`chain->next = NULL`) to free it in isolation, and `dk_free` follows `->next`
(freeing a node also frees the caller's chain, risking a double free). The
result is a small heap leak proportional to the number of `reset`/`shift`
evaluations.

## Minimal repro

```turmeric
(defn inner [] : int (shift (fn [v] v) 5))
(defn outer [] : int (+ 1 (reset (inner))))
(defn main [] : int (println (outer)) 0)
```

Build with an ASan/LSan-instrumented `cc` and run: prints `6`, then LSan reports
`... byte(s) leaked` from `dk_shift` / `dk_prompt` / `dk_frame` / `dk_done`.

## Root cause

- Abortive path: `emit_cps_reset` (`src/compiler/emit_cps.c:352-357`) emits
  `dk_run(dk_shift(1, __dk_abort_body, ..., dk_prompt(1, dk_done())), 0)` with no
  matching `dk_free`. (The cloneable/serial paths at `emit_cps.c:1198,1539` do
  free their chains; only the plain abortive path leaks.)
- CPS-IR-to-C backend (`--enable=cps-backend`, `src/compiler/emit_cps_ir.c`,
  Phase C3): emits the same shape (`dk_prompt`/`dk_frame` for the reset
  continuation, `dk_shift`/`dk_run` for the shift) and likewise cannot free the
  nodes safely because `DK` is opaque. It keeps `reset` in tail position
  (stackless) and leaks the nodes, mirroring the abortive path. Its fixtures
  carry `requires.no-leak-check` for this reason.

## Fix directions

1. Add a public single-node free to the DK API, e.g. `dk_free_node(DK *)` that
   frees one node without following `->next` (declare in `cps_prompt.h`, emit
   into the prelude in `emit_cps.c`). Then the shift site can
   `dk_free_node(shift_node)` after `dk_run`, and the reset site can free its
   `prompt`/`frame`/`done` once the delimited computation settles (requires a
   non-tail reset, trading stacklessness for leak-freedom -- or an arena).
2. Or arena-allocate the per-`reset` DK nodes and bulk-free at the enclosing
   reset boundary.
3. Cheapest interim: give `emit_cps_reset`'s abortive lowering the same
   treatment and document the leak as intentional/bounded.

## Follow-up: audit no-`no-leak-check` delimited fixtures

`escape-nested-reset`, `serial-reset-basic`, `shift-result-typing`,
`shift0-result-typing` use `reset`/`shift`/`escape` and carry **no**
`requires.no-leak-check` marker, yet the suite (leak detection ON for compiled
fixtures) passes. Either they do not execute the leaking abortive path
(cloneable/serial free their chains; a body that never reaches its shift
allocates nothing), or the leak is masked. Worth confirming which, so the true
leak surface of the abortive path is understood before wiring
`dk_free_node`.
