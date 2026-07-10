---
title: "U7 step 2 -- the base reset/shift native gap, mapped and sliced"
status: in-progress
parent: cps-backend-unification-u7-readiness-plan.md
description: An empirically-grounded map of which base reset/shift shapes the CT-IR backend already emits natively vs still evicts to emit_cps.c's emit_cps_reset, correcting the readiness plan's stale "setjmp/longjmp escape path" framing. The escape/branch shapes are already native (U1); the real residual is NESTED reset. First slice (nested reset via delim_ok CT_RESET admission) is landed; the remaining sub-gap is operand-position multiple non-tail nested resets that need a heap-reified join.
---

# U7 step 2 -- base reset/shift, what actually still evicts

The U7 readiness plan's row for `emit_cps_reset` names the gap as "shapes
outside `delim_ok` (setjmp/longjmp escape path, non-admitted join shapes)."
That framing is stale: the **escape/branch shapes are already native** (U1's
`delim_ok` relaxation admits `(reset (if c (shift ...) v))` and the
`(reset (+ 10 (if c (shift ...) 5)))` join-discard shape). Probing the backend
under `--enable=cps-backend` (native emission = a `f__cps(...)` function; a
fallback emits `emit_cps_reset`'s lowering inline in `f`) gives the real map.

## What is already native

Every flat / branch / capturing base reset/shift shape emits natively:

| Shape | Example | Status |
|---|---|---|
| flat shift | `(reset (shift (fn [v] v) 100))` | native |
| arithmetic context | `(reset (+ 10 (shift ...)))` | native |
| shift in `if` branch (escape) | `(reset (+ 10 (if c (shift ...) 5)))` | native (U1) |
| shift0 | `(reset (shift0 ...))` | native |
| capturing shift body | `(shift (fn [v] v) (+ x 5))` | native (N6.3) |
| `let` in the context | `(reset (let [a 3] (shift (fn [v] (+ v a)) 100)))` | native |
| tail call in shift body | `(shift (fn [v] v) (g 5))` | native |
| float / cstr result | `(reset (shift (fn [v : cstr] v) "hi"))` | native |

## What still evicted (before this slice): NESTED reset

A `reset` inside another reset's delimited body evicted to the direct emitter --
*every* nesting, down to `(reset (+ 1 (reset 5)))`. Root cause: `delim_ok`
(emit_cps_ir.c) admits prompt delivery (`KK_PROMPT`) at tail positions of a
delimited body, but on hitting a **nested `CT_RESET`** it fell through its
`default` to the stricter `term_core_ok`, whose `CT_APPCONT` rejects `KK_PROMPT`.
A nested reset's continuation (`reset.body`) legitimately delivers the nested
value *outward* to the enclosing prompt (a `KK_PROMPT` appcont in tail position),
so `term_core_ok` rejected it and the whole enclosing function evicted.

The DK runtime already supports this: it is multi-prompt, and Turmeric's base
shift binds to the *nearest* enclosing reset, so `emit_reset`'s prompt simply
nests (each `dk_prompt` is a distinct chain node; a `dk_shift` walks to the
nearest matching prompt). No emitter change was needed -- only the classifier
was over-conservative.

### The slice (landed)

`delim_ok` grows a `CT_RESET` case that admits a nested reset: slot-ok result,
`delim_ok(reset.delim)` for the inner delimited body, `delim_ok(reset.body)` for
its continuation (so outward `KK_PROMPT` delivery is admitted), and scalar-only
captures via `collect_caps`. `needs_heap_join` already walks `reset.delim` /
`reset.body`, so a nesting that *does* need a heap join is still evicted by the
fixpoint (see below).

Now native, verified `direct == cps` byte-for-byte on output:

- `(reset (reset (shift (fn [v] v) 100)))` -> 100 (tail nesting)
- `(reset (* 2 (reset (+ 100 (shift ...)))))` (arith context, two levels)
- `(reset (let [a 7] (+ a (reset (shift ...)))))` (capture across the nesting)
- `(reset (+ 1 (reset (if c (shift ...) 9))))` (branch inside the inner reset)
- `(reset (reset (reset (shift (fn [v] v) 42))))` (three deep -> 3 `dk_prompt`)

Oracle pair: `tests/fixtures/cps-oracle-reset-nested{,-cps}`.

## What still evicts (the next sub-gap): operand-position multi-join

A nesting where **two or more nested resets are non-tail operands** of the same
context still evicts -- e.g.

```
(reset (+ (reset (shift (fn [v] v) 3))
          (reset (shift (fn [v] v) 4))))
```

The first inner reset's value must be held live while the second inner reset
runs, then both are combined. That is a **heap-reified join** (`needs_heap_join`
true), which is the same C1 boundary that evicts any non-tail cps->cps call --
not specific to reset. Closing it is the general "heap join reification" work,
not a reset/shift-specific slice; it is correctly left evicting (the direct
fallback keeps `direct == cps`). A single non-tail nested reset combined with a
*pure* context (`(reset (+ 1 (reset (shift ...))))`) is fine and native -- only
*multiple* non-tail delimited operands trip the join.

## Sequencing note

With nested reset admitted, the residual base reset/shift eviction is the
generic heap-join case, shared with the rest of the C1 subset. So the base
reset/shift row of U7's table is effectively closed *except* for whatever the
general heap-join reification work later admits -- there is no reset-specific
tail left. `emit_cps_reset`'s remaining live callers are then the heap-join
nestings (until that lands) and any base shape a future coloring change routes
through it; its deletion (U7 step 3) still waits on the heap-join slice, but the
reset/shift-specific gap named in the readiness table is closed.
