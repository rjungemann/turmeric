# ArrowLoop at (->) supports only feedback the looped arrow never reads

**Severity: low** -- true lazy feedback is unimplemented (Turmeric is strict,
no thunks). Found in the 2026-08-20 docs audit.

## Root cause

`definstance ArrowLoop [(->)]` via `__ac_loop_step` in stdlib/arrow.tur:409.

## Fix direction

A thunk/cell-based feedback encoding or an explicit fixpoint combinator.

## Guides to update when fixed

- docs/guides/arrows-guide.md ("ArrowLoop -- non-recursive only")

## Resolution (2026-08-29)

Both fix directions landed, plus a third for the case neither covers.

The fed-back `d` is no longer a sentinel `0`. It is a **`LoopCell`** -- a
two-word heap cell `{ int64_t filled; int64_t value; }` handed to the looped
arrow in slot 1 of the input pair. The cell separates "the value is written"
from "the value is read", which is the only thing laziness was buying here.
Three combinators fill it at three different times, over one shared protocol,
so a single looped arrow works under all three:

| Combinator | The cell starts | Covers |
|---|---|---|
| `arrow-loop` / `arrow-loop-lazy` | empty; filled with the `d` output when the run returns | knot-tying -- the arrow parks the cell in its own `c` output and something forces it later, so `c` observes the `d` that same run produced |
| `arrow-loop-fix` | seeded with `d0`, refilled per pass until `d` stops moving or `fuel` runs out | an arrow that reads `d` strictly, where the answer is a fixpoint |
| `arrow-loop-delay` | seeded with `d0`, carried forward across calls | a unit delay in the feedback path -- call *n* sees call *n-1*'s `d` |

Reading is `loop-cell-of` (erased slot -> handle), `loop-cell-ready?`, and
`loop-cell-force`. Forcing an unfilled cell -- an arrow demanding its own
output -- panics with `<<loop>>` rather than quietly reading a sentinel, which
is a strict improvement on the old behaviour even for the case the old code
claimed to support.

The `ArrowLoop [(->)]` instance now does the knot-tying version, so a looped
arrow that ignores `d` behaves exactly as before (`arrow-instance-loop-nonrecursive`
still passes unchanged) and one that reads `d` is no longer wrong.

- Implementation: `stdlib/arrow.tur` -- `LoopCell`, `loop-cell-of`,
  `loop-cell-ready?`, `loop-cell-force`, `__ac_cell_new`, `__ac_loop_step`
  (rewritten), `__ac_loop_fix_step`, `__ac_loop_delay_step`, and the bare
  `arrow-loop-lazy` / `arrow-loop-fix` / `arrow-loop-delay`.
- Fixtures: `tests/fixtures/arrow-loop-lazy-feedback`,
  `tests/fixtures/arrow-loop-fix`, `tests/fixtures/arrow-loop-delay`.
- Guide: `docs/guides/arrows-guide.md`, section "ArrowLoop -- feedback".

Found on the way: a function declared over `(Tuple2 int int)` instead of the
erased `:int` carrier returns garbage through *any* arrow combinator, because
`TUR_APPLY1` calls it with the `int64` thunk ABI while it was emitted with the
struct-by-value return ABI. That is orthogonal to feedback and is filed
separately as `docs/reported/arrow-struct-typed-arrow-abi.md`.
