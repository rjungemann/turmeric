---
title: Widening a function to `any` through a LOCAL binding leaks a 24-byte shim box per widen
category: Reported
description: An `any` fn payload is normalised to a fat `{thunk, orig_fn}` box. For a file-scope function that box is a link-time constant and the emitter hoists it to a static, so the widen allocates nothing -- but a fn reached through a local binding (`(let [f (fn ...)] (peek f))`) is not a constant, so it falls back to a malloc that nothing frees, unless the callee's non-retain mask lets the frame-box rule fire.
---

# A fn widened through a local binding leaks its shim box

**RESOLVED 2026-09-11** by fix direction 1.  A `Binding` still does not carry
its initializer, so the fact is recorded at the `let` instead: an immutable
binding whose init names a global fn -- a lifted lambda (`__fn_N`) or a
`defn` -- gets `widen_fn_alias` pointing at it (`elab_forms.c`, next to the
`source_binding` alias, which deliberately refuses lifted lambdas because it
changes CALL semantics; this alias changes only where a box lives).  The
`EX_FN_TO_FAT` static-box guard in `emit_expr.c` sees through the alias and
spells the global's name, so the widen hoists the same static box the direct
widen does.  The repro goes from 2400 bytes in 100 allocations to zero;
`tests/fixtures/any-fn-widen-no-alloc` now covers the local-binding shape for
both a lambda and a `defn` alias (400 widens, zero allocations under
`run-leak-check.sh`).  Directions 2 and 3 were not needed for this shape;
direction 3 (widening the non-retain inference) remains a worthwhile
independent measurement for the by-value struct widen it also names.

**Severity (at filing): low-medium.** Bounded and narrow, but unbounded *in a loop*, which
is what turns a small allocation into a real problem.

Filed as the residue of
[any-cannot-recover-a-capturing-closure](../archive/any-cannot-recover-a-capturing-closure.md),
which normalised every `any` fn payload to the fat representation so a capturing
closure could be recovered and called. Normalising means shimming a bare fn into
a `{ thunk, orig_fn }` box on the way in, and that box needs an owner.

**Two of the three shapes already have one**, which is why this is the residue
and not the headline:

| widened value | box | allocates? |
| --- | --- | --- |
| a `defn` named as a value (`(peek add1)`) | static, hoisted | no |
| a lambda widened directly (`(peek (fn [x : int] : int ...))`) | static, hoisted | no |
| a lambda through a local binding (`(let [f (fn ...)] (peek f))`) | **malloc** | **yes, per widen** |
| a capturing closure / partial application | the closure's own box | no new allocation |

Pinned leak-clean for the first two by
`tests/fixtures/any-fn-widen-no-alloc` under `run-leak-check.sh`.

## Repro (2026-09-07)

```turmeric
(defn peek [a : any] : cstr (type-of a))
(defn loop-n [i : int] : int
  (if (< i 100)
    (do (let [f (fn [x : int] : int (+ x 1))]
          (println (peek f)))
        (loop-n (+ i 1)))
    0))
(defn main [] : int (loop-n 0))
```

```
SUMMARY: AddressSanitizer: 2400 byte(s) leaked in 100 allocation(s)
```

24 bytes per widen: `sizeof(void *) + 2 * sizeof(int64_t)`, the fat box.

## Root cause

`ensure_static_fatbox` requires the boxed value to be a link-time constant, and
the emitter's guard spells that as "`inner` is an `EX_VAR` of a **global**
binding". A `let`-bound lambda is lifted to a file-scope function, so the *value*
is a constant -- but the expression at the widen names a **local** binding, and
the guard cannot see through it. The malloc fallback then runs, and nothing
frees an `any` fn payload (its registry row carries `boxed = 0`, deliberately:
the flag is per-type and cannot distinguish a box the widen minted from a
capturing closure's own box, which must not be freed).

The frame-box rule would cover it in argument position, and
`elab_coerce_to_any`'s caller does set `stack_ok` on the shim when it fires --
but it needs the callee's `nonretain_ptr_param_mask` bit, which is 0 for an
ordinary `any` parameter like `peek`'s. So in practice it rarely fires here.

## Fix directions

1. **See through the local binding.** If a local binding's initializer is a
   non-capturing lambda or a global fn reference, the widen's value is still a
   link-time constant and the static box applies. This is the fix that removes
   the allocation rather than managing it, and it covers the reported shape
   exactly. The question is where to record it -- a `Binding` does not carry its
   initializer, so this likely wants a flag set at `let` elaboration ("this
   binding is a constant fn reference").
2. **Own the box at the widen.** The widen knows whether it minted the box: the
   inject's payload is an `EX_FN_TO_FAT` iff it did. `let_binding_any_freeable`
   already greenlights a scope-owned widen, and `let_binding_widen_drop_stmt`
   already emits a plain `free` rather than `__tur_any_drop` for the union case
   where the tag is a compile-time fact -- the same shape applies here. Care
   needed: it must free only the malloc form, never the static box, so the drop
   has to key on the same condition the emitter used to choose.
3. **Widen the non-retain inference** so an `any` parameter that is only read by
   `type-of` / `is?` gets its mask bit set, letting the existing frame-box rule
   fire. Useful independently -- the by-value struct widen leaks in exactly the
   same shape for the same reason.

Direction 1 is the cleanest and the most targeted; 3 would fix a whole family at
once and is worth measuring for its own sake.
