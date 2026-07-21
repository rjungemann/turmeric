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

## Fix directions

- Make an rc-typed `EX_GET_FIELD` read that flows into an owning binding emit an
  `rc_strong_increment` (clone), so the new binding is a real second owner. The
  cleanest home is where the rc-binding auto-drop is decided (`elab_forms.c`
  rc-drop injection) / the get-field emit for an rc field
  (`emit_expr.c` `EX_GET_FIELD`).
- Alternatively, detect `(.r o)` bound/extracted and mark that field consumed on
  `o` (`is_field_consumed`), suppressing `o`'s per-field auto-drop -- but this is
  a MOVE, which is wrong if `o` is used again after the read; the clone is safer.
- Add a regression fixture under `tests/fixtures/` once fixed; verify
  definitely-lost 0 AND no invalid-free under valgrind.
