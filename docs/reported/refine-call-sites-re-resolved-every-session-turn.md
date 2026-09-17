# A long-lived session re-resolves every refinement call site it has ever seen, on every turn

**Severity:** medium (performance of every interpreter session; the playground
and `tur repl` pay it on every turn, and a session rebuild pays it once per
replayed turn).

**Status:** OPEN. Filed 2026-09-16 while executing
[playground-session-hygiene-plan](../archive/playground-session-hygiene-plan.md)
(PS1), whose session replay made the cost visible.

## What happens

`refine_resolve_call_sites` (`src/compiler/elab_fns.c`, called from
`elaborate_program_session` in `src/compiler/elab_toplevel.c`) walks
`e->refine_call_sites[0 .. n_refine_call_sites)`. Under an `ElabSession` that
array is session state -- it is copied in with the rest of the `Elab`
(`e = *sess`) and saved back -- and nothing clears it between calls. So every
turn re-resolves every refinement crossing collected by every earlier turn,
the stdlib preload's included, and re-runs the path-condition and set-target
walks for each.

## Measured

A native harness (`libturi_wasm`, the playground's preload) with 300 `defn`
turns, then alternating a failing turn and a normal one, sampled with macOS
`sample` for 4 s: of 3241 main-thread samples, 3234 were in the session replay
and **1591 (49%) were under `refine_resolve_call_sites`** --
`rt_collect_set_targets`, `rt_form_occurrences`, `rt_collect_path_conds` and
`strcmp`. A replayed turn costs about 0.75 ms against ~0.2 ms for a turn with
no definitions, and the per-turn cost of an ordinary `defn` turn is close to
the replayed one, which fits the whole accumulated set being walked each time.

## Minimal repro

```c
turi_wasm_init();
for (int i = 0; i < 300; i++)   /* (defn f<i> [x : int] : int (+ x <i>)) */
    turi_wasm_eval(defn_i);
turi_wasm_eval("(undefined-thing 1)");   /* fails: the next turn replays */
turi_wasm_eval("(f9 1)");                /* ~0.25 s, half of it here */
```

## Fix directions

1. Under a session, resolve only the call sites THIS call added: record
   `n_refine_call_sites` at session restore (next to the PS4 `turn_start_*`
   watermarks) and start the loop there. Check first that a crossing recorded
   in an earlier turn cannot need re-resolution when a later turn defines its
   callee -- the deferral comment says a frame's callees "may be defined later
   in the unit", and a turn is a unit.
2. Or clear the array (and the obligations it produced) once a session call
   has resolved and discharged them.

Either way, check `refine_obs` too: if resolution re-collects obligations for
old crossings, a warning such as TUR-W0377 may also repeat per turn.
