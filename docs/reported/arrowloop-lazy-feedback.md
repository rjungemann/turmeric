# ArrowLoop at (->) supports only feedback the looped arrow never reads

**Severity: low** -- true lazy feedback is unimplemented (Turmeric is strict,
no thunks). Found in the 2026-08-20 docs audit.

## Root cause

`definstance ArrowLoop [(->)]` via `__ac_loop_step` in stdlib/arrow.tur:409.

## Fix direction

A thunk/cell-based feedback encoding or an explicit fixpoint combinator.

## Guides to update when fixed

- docs/guides/arrows-guide.md ("ArrowLoop -- non-recursive only")
