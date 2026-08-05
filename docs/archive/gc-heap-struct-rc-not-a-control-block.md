---
status: resolved
severity: medium
discovered: 2026-07-25
resolved: 2026-07-25
area: codegen + runtime (rc<T> over a `:heap` defstruct, cycle collector enabled)
---

# `rc<T>` over a `:heap` struct passes a non-control-block to `rc_strong_decrement`

## Resolution (2026-07-25)

Root cause: a **double-indirection mismatch** at the `rc/of` boxing site. A
`:heap` ADT is already represented as a pointer to its payload (its ctor mallocs
and returns `T *`), but `rc/of` boxed it the generic way -- malloc a cell, store
the value in it -- leaving `cb->value` a `T **`. Every consumer (field
read/write, walk glue, drop glue) casts `cb->value` straight to `T *`, so all of
them read the pointer cell as if it were the struct.

That single mismatch produced every symptom: `.next` returned the struct pointer
(handed to `rc_strong_decrement` as a bogus control block -> the crash), the
walker traced garbage, and the drop glue freed the cell while leaking the struct
(the ~64 B/ring of non-cycle leak).

Fix (`src/compiler/emit_expr.c`, `EX_RC_OF`): when the payload is a heap ADT
(`type.as.adt_.def->is_heap`), adopt the pointer the ctor already produced
instead of adding a second indirection, so `cb->value` points AT the payload as
every consumer assumes. Verified: the `:heap` cycle went from segfault to
collected (0 bytes over 5000 rings), and the collector-off leak dropped by
exactly the per-ring struct that drop glue had been leaking.

Guarded by `tests/fixtures/gc-heap-struct-rc`, which asserts both halves: an
acyclic `:heap` rc leaks nothing, and a `:heap` cycle is collected.

The original report follows for the record.

## Summary

An `rc<T>` whose `T` is a **`:heap`-annotated** `defstruct` reaches
`rc_strong_decrement` with a pointer that is **not a full `RcControlBlock`**.
With the cycle collector enabled this is a hard crash (heap-buffer-overflow);
with the collector disabled -- the default -- the same type confusion is silent,
and merely leaks.

Found while auditing walker completeness for CG3. The equivalent `:move` struct
is fine, which is why every existing GC fixture passes.

## Repro

```turmeric
(defstruct H :heap [next : rc<H>])      ; :move here works fine
(defn nullp [] : ptr<void>
  ```c
  return NULL;
  ```)
(defn ring [] : int
  (let [h1 (rc/of (make-struct H (nullp)))
        h2 (rc/of (make-struct H (rc/clone h1)))]
    (set! (.next h1) (rc/clone h2))
    0))
(defn main [] : int
  (gc-enable!)
  (ring)
  0)
```

- **Collector enabled:** segfault. Under ASan:

```
ERROR: AddressSanitizer: heap-buffer-overflow
    #0 gc_add_suspect            (reading cb->gc_buffered)
    #1 rc_strong_decrement
    #2 ring
```

- **Collector disabled (default):** runs to completion, leaking ~300 B per ring
  (608,768 B over 2000 iterations).

## What the evidence shows

The faulting read is `cb->gc_buffered`, a field partway into `RcControlBlock`.
ASan reports it as past the end of the underlying heap allocation, so the
pointer handed to `rc_strong_decrement` refers to a smaller object -- i.e. a
raw `:heap` struct payload, not the control block that owns it.

This confusion is **not** caused by the cycle collector. It is latent in the
`:heap` + `rc<T>` lowering:

- With the collector off, `rc_strong_decrement` only touches `strong_count`
  (offset 0) and `weak_count` (offset 8), which happen to land inside the
  smaller allocation. The corruption is silent and shows up as a leak.
- CG1 added a `PossibleRoot` hook on the count-stays-positive branch, which
  reads `gc_buffered` -- far enough into the struct to fall off the end. So
  CG1/CG2 did not introduce the confusion, they made an existing one fatal
  instead of silent.

Not yet pinned down (deliberately not asserted here): whether the bad pointer
originates at the `rc/of` boxing site for `:heap` payloads, or at the
`(set! (.field ...))` field-store path that decrements the previous value. Both
are plausible from the trace; confirming it needs a debugger session on the
emitted C, not more inference.

## Scope / impact

- Requires **all** of: a `:heap` (not `:move`) `defstruct`, an `rc<T>` over it,
  and `(gc-enable!)`. The collector is off by default, so no shipping program
  hits the crash today.
- The silent-leak half of the bug is reachable **without** the collector, and
  is the more likely thing to bite in practice.
- Not covered by any existing fixture: every GC fixture uses `:move` structs.

## Fix directions

1. Find where a `:heap` struct's rc handle is lowered and make the value handed
   to `rc_strong_increment`/`rc_strong_decrement` the control block, not the
   payload pointer. That single fix resolves both the crash and the leak.
2. Add a `:heap`-struct cycle fixture alongside `gc-collects-strong-cycle` so
   the shape is covered from then on.
3. Consider a debug-build assertion in `rc_strong_decrement` that the block
   looks like a control block (e.g. a magic tag in `reserved[]`), so a future
   type confusion fails loudly at its source instead of hundreds of lines away
   inside the collector.

## Recommendation

Worth fixing before the collector is advertised or defaulted on, since the crash
is a hard failure the moment a user combines `:heap` + `rc<T>` + `(gc-enable!)`.
Independently, the silent leak is a live (if narrow) correctness bug in the
default configuration and can be fixed on its own schedule.
