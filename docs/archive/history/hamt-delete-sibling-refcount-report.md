# HAMT delete path under-retains a shared node (double-free on lineage free)

**Status:** RESOLVED. Root cause was an *over-release*, not a missing retain:
`node_delete`'s collapse arms released the deleted child from the old, shared
node. Fixed in `src/runtime/hamt.c` (bitmap + array collapse arms); regression
guarded by `tests/test_hamt_del_lineage.c` (ctest `tur_hamt_del_lineage`, plain
keys) and a complementary boxed-key `test_delete_collapse_lineage` case in the
`tur_hamt_owned_keys` gate. Fixing this unblocked interpreter Set/Map
teardown-reclaim (`docs/archive/history/interp-collections-never-freed.md`). See
`docs/archive/history/hamt-delete-sibling-refcount.md` for the paper trail.

**Severity:** medium (latent correctness; a genuine double-free / heap
use-after-free, but only surfaces when a *whole* persistent-map lineage that
includes a `tur_hamt_del`-produced map is freed. Programs that leak their maps
-- as the tree-walking interpreter did until now -- never hit it. Blocks
teardown-reclaim of interpreter Set/Map buffers, see
`docs/archive/history/interp-collections-never-freed.md`.)

## Summary

`tur_hamt_del` (and therefore `set-remove` / `map-dissoc`) produces a new
persistent map that shares nodes with its parent. Somewhere on the delete /
collapse path a shared node is handed to the result **without a matching
`tur_hamt_node_retain`**, so its refcount is one too low. Freeing every map in
the lineage then releases that node once too often -> `free` on an already-freed
node -> heap use-after-free.

`tur_hamt_set` (assoc) is *not* affected: an assoc-only lineage frees cleanly.
The bug is specific to the delete path.

## Repro

```turmeric
;; tur interpret this file (Debug build => ASan+LSan)
(defn main [] : int
  (let [e  (set-new)
        s1 (set-add e 1 1)
        s2 (set-add s1 2 2)
        s3 (set-remove s2 2 2)]     ; <-- delete; s3 shares nodes with s2
    (set-free e) (set-free s1) (set-free s2) (set-free s3)
    0))
```

```
==ERROR: AddressSanitizer: heap-use-after-free ... in tur_hamt_node_release
    #0 tur_hamt_node_release   src/runtime/hamt.c:423
    #1 tur_hamt_free           src/runtime/hamt.c:878
  freed by:
    #1 tur_hamt_node_release   src/runtime/hamt.c:425   (the earlier release)
  allocated by:
    #1 hamt_malloc             src/runtime/hamt.c:138
```

Control (no delete -- swap the `set-remove` for a third `set-add`) frees cleanly
with no ASan error:

```turmeric
(let [e (set-new) s1 (set-add e 1 1) s2 (set-add s1 2 2) s3 (set-add s2 3 3)]
  (set-free e) (set-free s1) (set-free s2) (set-free s3) 0)   ; OK
```

Isolation summary:

| lineage, all boxes freed | result |
| --- | --- |
| add, add, add            | OK |
| add, add, **remove**     | heap-use-after-free |

## Root cause

In `node_delete` (`src/runtime/hamt.c:646`), when a child delete collapses a
node, surviving siblings are copied into a freshly built node. The bitmap and
array collapse arms *do* retain survivors (e.g. `tur_hamt_node_retain` at
`src/runtime/hamt.c:691` and `:724`), and `tur_hamt_del` correctly avoids a
double-retain of the fresh root. Nonetheless a shared node ends up with a
refcount one short of the number of maps that reference it -- most likely a
missing retain (or an extra release) on one of the collapse / pull-up arms that
the assoc path does not exercise. The exact site was not pinpointed here; a
debugger watch on the offending node's `ref_count` across the four `set-free`
calls in the repro will localize it quickly.

Reference-counting model for context: `tur_hamt_set` / `tur_hamt_del` return a
map the caller owns (refcount 1 on a fresh root); shared children are retained
by `bitmap_node_copy` / `array_node_copy` / the explicit retains in the collapse
arms. The invariant a fix must restore: after any op, every node's `ref_count`
equals the number of live maps whose reachable structure includes it.

## Impact

- Any program (compiled or interpreted) that frees a full persistent-map
  lineage containing a `del`/`dissoc`-derived map double-frees. Programs that
  keep maps alive to process exit never observe it.
- Blocks env-teardown reclaim of interpreter Set/Map buffers: a bulk
  `tur_hamt_free` over every tracked Set/Map box (the natural companion to the
  Vec reclaim just landed) trips this exact double-free. Once this is fixed,
  wiring Set/Map through `turi_env_track_collection`
  (`docs/reported/interp-collections-never-freed.md`, fix direction 1) becomes
  safe.

## Fix directions

1. Pinpoint the missing retain / extra release in `node_delete`'s collapse arms
   with the four-free repro under a debugger, and restore the retain-count
   invariant. Add a fixture that frees a delete-derived lineage (the repro
   above) so the regression is guarded.
2. Consider a `tests/test_hamt.c` unit case that builds `{1,2}`, deletes `1`,
   and frees both maps while asserting no double-free (runs under ASan in the
   Debug build), independent of the interpreter.
