# Trowel REPL pane divider is not drag-resizable

> **Severity:** Minor (UX papercut in the Trowel editor, not the compiler)
> **Component:** `tools/trowel/turmeric.lua`
> **Reported:** 2026-06-28
> **Status:** Open — partial fix landed (ratio works), drag still broken

---

## Summary

In Trowel (the Lite XL fork that ships with this repo), the **project
tree** divider is drag-resizable as expected: hovering it flips the cursor
to a horizontal-resize arrow and the user can drag. The **REPL pane**
divider is **not**: hovering shows no cursor change and no drag is
possible. This has been mis-diagnosed by at least 4 previous Claude Code
sessions.

## What works

Setting the initial editor/REPL split ratio. After the most recent fix
(see "Attempted fix" below), `node.divider = 0.7` in
`open_repl_view("down")` correctly produces a 70/30 editor/REPL split at
startup. The ratio change *did* take effect — confirming that
`Node:split` mutates `self` in place into the parent split-node and the
divider lives on `node` itself, not on `node.parent` (Lite XL `Node` has
no `parent` field; parent lookup walks from root via `get_parent_node`).

## What doesn't work

The REPL divider is still not drag-resizable after the most recent
attempt. The intended fix was to bump the REPL auto-open thread's
`coroutine.yield` from `0.1` to `0.3` so it runs *after* the toolbar's
`0.2`s split, on the theory that the toolbar's locked-Y leaf was getting
trapped inside the editor side of the REPL vsplit and propagating
`is_resizable("y") = false` up through `Node:is_resizable`'s AND-recursion
(node.lua:654-661). Re-ordering should have put the toolbar *outside* the
REPL vsplit, leaving the REPL divider with two unlocked children.

It did not. The REPL divider remains non-draggable.

## Hypothesis (current best guess, not yet confirmed)

One of:

1. The yield re-ordering didn't actually re-order — `core.add_thread`
   scheduling, frame timing, or `get_active_node_default()` returning the
   wrong leaf at t=0.3 may produce the same tree shape as before.
2. The toolbar split runs in a third place we haven't accounted for
   (there's an `if ok_tb` branch at `tools/trowel/turmeric.lua:1078`
   that integrates with `plugins.toolbarview` instead of the fallback;
   only the fallback was reasoned about).
3. Something else in the Trowel layout (TaskListView? log pane?
   diagnostics pane?) is sitting on the editor branch with locked-y +
   non-resizable, causing the same propagation regardless of toolbar
   ordering.
4. ReplView itself (or a parent View class it extends) sets something
   that makes its leaf report non-resizable — but a quick read of
   `ReplView:new()` showed no `locked`/`size_lock` assignments. Worth
   re-reading more carefully.

## Repro

1. Launch Trowel (`tools/trowel/launch.sh` or the built `Trowel.app`).
2. Confirm a project tree on the left and the REPL pane at the bottom.
3. Hover the divider between the project tree and the editor → cursor
   flips to `sizeh`, drag works.
4. Hover the divider between the editor and the REPL pane → **no cursor
   change, no drag**.

## Files involved

- `tools/trowel/turmeric.lua` — `open_repl_view` (around line 754),
  auto-open thread (around line 913), toolbar fallback (around line
  1157), `plugins.toolbarview` integration (around line 1078).
- `tools/trowel/Trowel.app/Contents/Resources/core/node.lua` — bundled
  Lite XL `Node:split` (line 80), `Node:is_resizable` (line 654),
  `Node:get_divider_overlapping_point` (line 230).
- `tools/trowel/Trowel.app/Contents/Resources/core/rootview.lua` —
  cursor selection at line 298-303 (the `sizev` request happens only if
  `get_divider_overlapping_point` returns non-nil).

## Fix directions to try next

- Add a one-shot debug log inside `open_repl_view` *after* the split
  that walks `core.root_view.root_node` and prints the tree shape +
  `is_resizable("y")` at each node. The actual tree at runtime will
  immediately reveal which branch is reporting non-resizable, instead of
  reasoning about it from source.
- Specifically check `core.root_view:get_active_node_default()` *return
  value* at t=0.3 — confirm it's the editor leaf and not something the
  toolbar moved.
- If a non-toolbar pane (TaskListView, log, diagnostics) is the culprit,
  the structural fix is the same shape: hoist the REPL split so it
  brackets the entire editor+aux stack, not the editor leaf alone.

## Notes for the next agent

- The user has explained this 5+ times. The instinct to "explain the
  Lite XL split model" or "set `node.divider`" is correct background but
  does **not** fix the drag issue. The drag issue is a *resizability
  propagation* issue, not a divider-value issue. Confirm the runtime
  tree before patching.
- Do not re-attempt the ratio fix — it works.
