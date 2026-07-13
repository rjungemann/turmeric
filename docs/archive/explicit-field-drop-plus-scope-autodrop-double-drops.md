# Explicit `(rc/drop (.field o))` plus the scope-exit auto-drop double-drops a by-value struct's owning field

**Severity:** medium (memory-unsafe: use-after-free / double-decrement), but the
shape is unusual -- user code rarely drops a struct's owning field by hand.
Handler-independent; surfaced while resolving the consuming-aggregate handler
capture (`docs/archive/cps-consuming-aggregate-capture-hardfails.md`).

**Status: RESOLVED (straight-line case).** `elab_let` now consults a new
per-field consumption check (`is_field_consumed`, `src/compiler/elab_core.c`)
and skips a by-value struct's per-owning-field scope-exit auto-drop when the body
already drops that field explicitly. Regression fixture:
`tests/fixtures/owning-field-explicit-drop-suppresses-autodrop/`.

## Resolution

The by-value-aggregate scope-exit auto-drop injection (`elab_forms.c`,
`elab_let`) emits one `(defer (rc/drop (.f o)))` / `(defer (drop! (.f o)))` per
owning field, guarded only by *whole-binding* consumption
(`is_binding_consumed`) -- which does not notice a *field-level* drop like
`(rc/drop (.r o))` (its operand is an `EX_GET_FIELD`, not a bare `EX_VAR`). So
the explicitly-dropped field was decremented twice.

The fix adds `is_field_consumed(body, binding, field_idx)`, the field-level
analog: it walks the let body for `(rc/drop (.f o))` / `(drop! (.f o))` matched
by `(binding, field_idx)` and, when found, elab_let skips that one field's
auto-drop (both the count and the inject loops, kept in lockstep). It mirrors
`is_binding_consumed`'s conservative convention -- a drop found ANYWHERE (incl.
one branch of an `if`) suppresses the auto-drop, because a leak on an untaken
path is memory-safe while a double-free is not. Per-field, so a multi-field
struct still auto-drops the fields the body does not touch.

Verified (decrement counts in emitted C + runtime):

- no explicit drop -> auto-drop fires (1 decrement);
- explicit `(rc/drop (.r o))` -> exactly the explicit drop (1), auto-drop
  suppressed;
- two owning fields, only one dropped -> dropped field suppressed, other still
  auto-drops (2);
- `ref` field via `(drop! (.r o))` -> same, one `free`;
- conditional drop on both branches -> suppressed, one drop at runtime.

The regression fixture makes the double-drop observable without a sanitizer: it
clones a shared rc into the struct field (count 2), drops the field explicitly
(back to 1), and reads `(rc/strong-count shared)` -- which is exactly 1 with the
fix, but read freed memory (garbage) before it, because the spurious auto-drop
had freed the block.

## The handler-case sibling (rejected loudly, not suppressed)

`is_field_consumed` does NOT descend into `handle`/`perform`/`resume` bodies --
deliberately, mirroring `is_binding_consumed`. A handler case that drops a
captured field runs 0..N times, so suppressing the outer auto-drop based on a
drop *inside* a case would leak (0 runs) or still over-drop (N runs) -- beyond
simple move analysis. Rather than leave that a silent double-drop, `elab_let`
now REJECTS it with **TUR-E0107** via the companion walker
`is_field_consumed_in_handler` (which finds the `handle` and checks each case
body). So the two siblings are handled differently but both safely: straight-line
field drop -> auto-drop SUPPRESSED (this note); handler-case field drop -> hard
ERROR (`docs/archive/cps-consuming-aggregate-capture-hardfails.md`). (An earlier
draft of this note speculated "fixing the auto-drop here fixes both" -- that was
wrong; the handler-case case is rejected, not fixed.)

## Original repro (straight-line, now resolved)

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defn f [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]
    (do (rc/drop (.r o)) (.tag o))))   ; o.r decremented HERE and again at scope exit
(defn main [] : int (println (f)) 0)
```

Before the fix, `tur build` succeeded and printed `9`, but `o.r` was decremented
twice (the explicit `(rc/drop (.r o))` -> count 0, freed; then the scope-exit
auto-drop decremented the freed block -> use-after-free). Now only the explicit
drop is emitted.

## Root cause (file:line)

- Injection site: `src/compiler/elab_forms.c`, `elab_let`, the
  "byvalue-struct-field-leak" block -- one `(defer (rc/drop (.f o)))` per owning
  field, guarded by `binding_moved_during_init` / `is_moved` /
  `is_binding_consumed` (all whole-binding).
- Missing check (added): `is_field_consumed` in `src/compiler/elab_core.c`, the
  field-level analog of `is_binding_consumed` -- suppresses the straight-line
  field drop's auto-drop.
- Companion (added): `is_field_consumed_in_handler` in the same file -- finds a
  field drop inside a handler case and drives the TUR-E0107 rejection in
  `elab_let` (the sibling shape that cannot be suppressed).
