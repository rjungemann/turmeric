---
title: Reading an rc field into a var double-frees the control block
category: Bug Report
status: resolved
component: compiler/elab (let rc-binding auto-drop)
affects: turmeric main
resolution: clone-on-read -- an rc-managed let binding whose init borrows an `rc` field is wrapped in EX_RC_CLONE so the read increments the strong count and the new binding is a genuine second owner; see Resolution below
severity: medium
---

# Reading an rc field into a var double-frees the control block

**Severity:** medium (memory unsafety -- double-free / use-after-free on a
refcounted payload; only triggers for a specific extract-then-hold shape).

## Summary

Binding an owning `rc<T>` field of a by-value struct local into a new variable
(`(let [s (.r o)] ...)`) copies the `RcControlBlock*` WITHOUT incrementing the
strong count, yet both owners are then decremented at scope exit:

- `o`'s scope-exit auto-drop (the `byvalue-struct-field-leak` injection in
  `elab_forms.c`) decrements `o.r`.
- `s`, bound at type `rc<T>`, gets the ordinary rc-binding auto-drop and
  decrements `s` -- the SAME control block.

Two decrements against one +1 count => the block is freed, then read/written
again => valgrind reports `Invalid read/write` + `Invalid free`.

## Minimal repro

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defn main [] : int
  (let [o     (make-struct Own (rc/of 7) 3)
        saved (.r o)]
    (rc/strong-count saved)))
```

Build with ASan/valgrind:

```sh
TUR_CC_FLAGS="-O1 -std=c99 -g -L build/src" ./build/tur build repro.tur -o repro
valgrind --leak-check=full ./repro     # Invalid read of size 8 in rc_strong_decrement
```

Emitted `main` (abridged) shows the two defers hitting one block:

```c
RcControlBlock *saved = (RcControlBlock *)(o).r;   /* copy, NO incref */
/* __defer_162 */ rc_strong_decrement((RcControlBlock *)(o).r);  /* frees it */
/* __defer_159 */ rc_strong_decrement(saved);                    /* UAF */
```

## Root cause

Reading an owning `rc` FIELD is treated as a plain word copy, not an rc clone.
`rc` denotes SHARED ownership, so `(.r o)` binding a new owner should increment
the strong count (clone-on-read), balancing the second decrement. Equivalently,
the read could be treated as MOVING `o.r` out (suppressing `o`'s field auto-drop
for that field). Either fixes the imbalance; the increment-on-read is the more
faithful `rc` semantics.

Not introduced by the local fn-field drop work (2026-07-21): confirmed on
commit `c97c29f` before it. Independent of fn-fields -- purely an rc-field-read
ownership-accounting gap.

## Resolution (2026-07-21)

Fixed in `elab_let` (`src/compiler/elab_forms.c`) with the increment-on-read
(clone) that the report recommends as the more faithful `rc` semantics.

Inside the rc-binding auto-drop block, BEFORE the auto-drop defers are injected
(they mutate `body` to add `(defer (rc/drop x))`, which would then read back as a
consumption of `x`), a new pass wraps the initializer of every rc-managed binding
whose init borrows an `rc` field in `EX_RC_CLONE`:

- `elab_rc_field_read_init` peels type ascriptions and returns the `EX_GET_FIELD`
  when the init reads an `rc<T>`-typed field directly (`(.r o)` / `(:: (.r o)
  rc<int>)`), else NULL.
- The wrap fires for ANY rc-managed binding disposed exactly once -- its
  scope-exit auto-drop, an explicit `(rc/drop saved)`, or a move into a consumer
  -- because in every case the source struct still releases the field (a
  by-value local via its field auto-drop, an `rc/of` struct via its
  control-block drop glue, a borrowed parameter via its caller). The clone's +1
  balances the binding's single disposal; the source-side release is balanced by
  the field's original +1.
- The ONE exception: when the source field is itself explicitly moved out
  (`(rc/drop (.f o))` / `(drop! (.f o))`, detected via `is_field_consumed`), the
  source no longer releases it, so the raw copy is already the sole owner and the
  clone is skipped (else it would over-count).

`EX_RC_CLONE` already emits `rc_strong_increment(<field expr>)` and returns the
same pointer, so the binding becomes a real second owner. A plain `rc/of`
binding (init is not a field read) is untouched; `ref`/`weak` fields are `TY_REF`
/ `TY_WEAK`, excluded by the `TY_RC` check.

Regression fixture: `tests/fixtures/rc-field-read-into-var-clone/` -- observable
without a sanitizer (a shared block read back at strong count 1 after both
scope-exit drops balance; pre-fix it read freed memory). Verified
`valgrind --leak-check=full` clean (0 lost, no invalid free) on the report's
repro, the explicit-`(rc/drop saved)` variant, and a two-rc-field variant. Full
suite green (2239 passed).

Per-fix paper trail:
`docs/archive/history/rc-field-read-into-var-double-free.md`.
