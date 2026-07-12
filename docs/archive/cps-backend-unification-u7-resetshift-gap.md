---
title: "U7 step 2 -- the base reset/shift native gap, mapped and sliced"
status: landed
parent: cps-backend-unification-u7-readiness-plan.md
description: An empirically-grounded map of which base reset/shift shapes the CT-IR backend already emits natively vs still evicts to emit_cps.c's emit_cps_reset, correcting the readiness plan's stale "setjmp/longjmp escape path" framing. The escape/branch shapes are already native (U1); the residual was NESTED reset. Two slices landed: (1) nested reset via delim_ok CT_RESET admission, (2) SIBLING nested resets via collect_caps_rec walking CT_RESET/CT_SHIFT -- which corrected an earlier wrong guess that the sibling case needed a heap-reified join. The base reset/shift-specific gap is now closed.
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

## Slice 2 (landed): SIBLING nested resets

The doubly-nested shape -- **two or more nested resets as non-tail operands** of
the same context, so the second lands in the first's *continuation* -- e.g.

```
(reset (+ (reset (shift (fn [v] v) 3))
          (reset (shift (fn [v] v) 4))))
```

still evicted after slice 1. **The reason was NOT a heap-reified join** (an
earlier guess in this note, empirically disproved with `TUR_CPS_WHY`
instrumentation): the eviction was `!term_core_ok`, tripped by `collect_caps`.
In the CPS IR this shape is a nested `CT_RESET` (`reset t1`) whose *continuation*
holds another nested `CT_RESET` (`reset t2`) before the `(+ t1 t2)` delivery.
`collect_caps_rec` -- which enumerates the scalar captures a lifted RESET_CONT
helper carries in its env -- had no `CT_RESET`/`CT_SHIFT` arm and hit its
conservative `default: cs->ok = false`, so the whole enclosing function evicted.

Fix: `collect_caps_rec` grows `CT_RESET` (walk `delim` + `body`, binding the
reset value) and `CT_SHIFT` (walk the shift body) arms. `emit_reset` already
nests prompts correctly and lifts each nested reset's continuation as its own
RESET_CONT helper, so **no emitter change was needed** -- only the capture
enumerator was over-conservative. `needs_heap_join` still walks these nestings,
so a nesting that genuinely needs a heap-reified join (e.g. a non-tail cps->cps
*call* threaded through the continuation) is still correctly evicted by the
fixpoint -- that remains the shared C1 boundary, unrelated to reset/shift.

Now native and `direct == cps` verified: sibling pairs and triples, sibling
arithmetic contexts, a capture threaded *between* two sibling resets, a param
capture into an inner shift value, and a single nested reset wrapping a sibling
pair. Oracle pair: `tests/fixtures/cps-oracle-reset-nested-siblings{,-cps}`.

## Sequencing note

With both slices in, the base reset/shift-specific gap is **closed**: every
delimited nesting of base reset/shift lowers natively; the only nestings that
still evict do so via the *generic* `needs_heap_join` boundary (a non-tail
cps->cps call reified onto the heap chain), which is shared with the whole C1
subset and is not reset/shift-specific. `emit_cps_reset`'s remaining live
callers are those generic heap-join shapes (until that separate slice lands) and
any base shape a future coloring change routes through it; its deletion (U7
step 3) waits on the heap-join work, not on anything reset-specific.
