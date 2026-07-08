# Fix: HAMT delete path double-free on lineage free

**Resolved:** 2026-07-08
**Area:** `src/runtime/hamt.c` -- persistent HAMT reference counting

## Symptom

Freeing a *whole* persistent-map lineage that includes a `tur_hamt_del`
(`set-remove` / `map-dissoc`) result double-freed a shared node:

```
==ERROR: AddressSanitizer: heap-use-after-free in tur_hamt_node_release
  freed by:  tur_hamt_node_release  (an earlier release)
```

Repro (Debug build => ASan+LSan), `tur interpret`:

```turmeric
(defn main [] : int
  (let [e  (set-new)
        s1 (set-add e 1 1)
        s2 (set-add s1 2 2)
        s3 (set-remove s2 2 2)]
    (set-free e) (set-free s1) (set-free s2) (set-free s3)
    0))
```

Assoc-only lineages (`add,add,add`) were never affected.

## Root cause

The original report guessed a *missing retain*. The actual defect was the
opposite -- an **over-release**.

`node_delete`'s two collapse arms handle the case where a child delete returns
`NULL` (the subtree emptied). Both build a *fresh* replacement node
(`bitmap_node_create` / `array_node_create`) and copy + retain only the
**surviving** siblings; the deleted child is simply omitted. But each arm also
did:

```c
tur_hamt_node_release(n->as.bitmap.children[idx]);   /* bitmap arm */
tur_hamt_node_release(n->as.array.children[chunk]);  /* array arm  */
```

`n` is the *old, shared, persistent* node -- an earlier map in the lineage
still references it and still legitimately owns that child. Dropping the
reference here let the child reach refcount 0 and be freed while the old node's
bitmap/array still pointed at it. When that old map was later freed,
`node_free_recursive` released the same child a second time -> use-after-free.

Trace from the repro (`{1,2}` then delete `2`; `b40` = node for key `1`,
`e40` = node for key `2`, `cc0` = the two-child bitmap = `s2`'s root):

```
RELEASE e40 -> 0     <- node_delete frees key-2's node...
...
RELEASE cc0 -> 0     <- ...but s2's root bitmap still points at e40,
                        so freeing s2 releases e40 again  => UAF
```

The non-collapse `else` branches release correctly, because there
`bitmap_node_copy` / `array_node_copy` retained *all* children (including the
one being overwritten), so the release balances the copy's own retain. The
collapse arms have no such copy-retain to balance, so the release was pure
over-count.

## Fix

Removed the two spurious `tur_hamt_node_release` calls (bitmap and array
collapse arms) in `src/runtime/hamt.c`. The old node keeps ownership of the
deleted child and releases it exactly once when it is itself freed; the new
node never referenced it. Refcount invariant restored: every node's
`ref_count` equals the number of live maps whose reachable structure includes
it.

## Regression guard

- `tests/test_hamt_del_lineage.c` + `tests/run-hamt-del-lineage.sh`, registered
  as ctest `tur_hamt_del_lineage` in `CMakeLists.txt`. Compiles the HAMT
  runtime with ASan/UBSan (+LSan on Linux) and frees a full delete-derived
  lineage (the report's repro, an assoc-only control, and a deep-collapse case
  where keys share low-order hash bits). Verified to fail (heap-use-after-free)
  against the pre-fix runtime and pass after.
- Full fixture suite (`bash tests/run.sh`): 1978 passed, 0 failed.

This unblocks env-teardown reclaim of interpreter Set/Map buffers
(`docs/reported/interp-collections-never-freed.md`, fix direction 1).
