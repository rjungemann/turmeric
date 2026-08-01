# Two collectors in one process: what is actually safe across a `dlopen` boundary

**Severity:** low-medium (no reproduced corruption; one hazard found by
inspection and hardened, one unenforced premise)
**Status:** RESOLVED (2026-07-26). Headline claim VERIFIED; residual item 2
fixed; item 1 (the unenforced premise) now written up for users -- see
[Resolution of item 1](#resolution-of-item-1).
**Found by:** the "Two collectors in one process" open question in
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../../upcoming/v1/gc-cycle-collection-followup-plan.md)
("Believed fine today because values do not cross the boundary; DEDUP-5 item 2
should confirm rather than assume it.")

## What was asked vs what was open

DEDUP-5 item 2 already measured *separate collectors* and hardened the result
with `visibility("hidden")`, so "are there two collectors?" was settled. What was
never checked is the **premise the safety argument rests on** -- that values do
not cross the boundary -- and what happens when they do. That is what this covers.

## Verified good

**1. The DEDUP-5 hardening holds.** `tur build --shared` on
`tests/fixtures/build-shared-smoke`:

```
exported gc_*/rc_* dynamic symbols: 0
exported module symbols:            2
```

No interposition surface. This is a live regression check, not a re-reading of
the claim.

**2. The registries really are separate.** A Turmeric host (archive collector)
dlopens a Turmeric `.so` (replica collector) and asks the `.so` for an `rc<int>`:

| | host `gc_stat_live_blocks()` |
| --- | --- |
| before the `.so` allocates | 0 |
| after the `.so` allocates an rc | **0** |

The `.so`'s block is invisible to the host's registry, as intended.

**3. Reading a foreign block works.** The host reads `rc_strong_count` on the
`.so`'s block and gets the correct `1` -- the control-block layout is identical
across the two copies, which is what the DEDUP-1 layout guard and the DEDUP-4b
`RC_VT_*` asserts are for.

**4. Releasing a foreign block does not corrupt the host registry.** The host
decrements the `.so`'s block to zero and drains. No crash, no ASan report. This
is **structural, not luck** -- `gc_unregister_block` (`src/runtime/gc.c:347-352`)
validates the back-reference before touching the array:

```c
uint32_t idx = cb->gc_index;
if (idx == RC_GC_INDEX_NONE || idx >= gc_all_blocks_count ||
    gc_all_blocks[idx] != cb) {
    cb->gc_index = RC_GC_INDEX_NONE;
    return;   /* not registered (or already removed) */
}
```

A foreign block's `gc_index` indexes the *other* registry, so
`gc_all_blocks[idx] != cb` fails the identity check and the function bails. The
only way to pass it would be for the same `cb` pointer to sit at that index in
both registries, which cannot happen.

## Residual item 1: the premise is a convention, not an invariant

Nothing stops an `rc<T>` from crossing the boundary. This builds and exports
cleanly:

```turmeric
(defmodule rcmod
  (export make-rc)
  (defn make-rc [x : int] : rc<int> (rc/of x)))
```

```
rcmod/make-rc -> rcmod__make_hyrc :: (:int) -> :any
```

So "values do not cross the boundary" is a statement about how people currently
write code, not something the compiler enforces. Finding 4 means the *common*
consequence is contained, but the safety argument should not lean on the premise
-- it should lean on the identity guard, which is the thing actually doing the
work.

Note the manifest types the export as `:any`, so the boundary is untyped in both
directions; a host has no way to know it received something refcounted.

## Residual item 2: a suspect-buffer desync, by inspection

`gc_unregister_block` calls `gc_remove_suspect(cb)` at line 346 -- **before** the
identity guard that makes finding 4 safe. And `gc_remove_suspect`
(`gc.c:186-199`) writes to the block unconditionally at the end:

```c
static void gc_remove_suspect(RcControlBlock *cb) {
    if (!cb) return;
    if (!cb->gc_buffered) return;   /* not in the buffer, nothing to scan */
    for (uint32_t i = 0; i < gc_suspect_count; i++) { ... }
    cb->gc_buffered = false;        /* <-- runs even when not found */
}
```

If a block is buffered as a suspect in the **`.so`'s** collector and the **host**
releases it: the host sees `gc_buffered == true`, scans its own (different)
suspect array, finds nothing, and still clears the flag. The `.so`'s
`gc_suspect_roots` keeps a pointer to a block that is then freed, and the `.so`'s
own `gc_remove_suspect` will now early-return on the cleared flag and never
remove it. The `.so`'s next collection reads freed memory.

That is exactly the failure CG1's own comment at `gc.c:341-345` was added to
prevent ("leaving a stale pointer here would make the next collection read freed
memory") -- the mitigation just does not compose across two collectors.

**Reachability NOT demonstrated.** Two attempts to get a live block into the
`.so`'s suspect buffer from outside did not succeed (the block came back with
`gc_buffered == 0`, so the host's release took the harmless path). The hazard is
identified by reading the code, and the argument above is sound, but it should be
treated as unconfirmed until someone reproduces it. The condition to hit is
narrow: the block must be *alive*, *buffered in the other collector*, and
released by this one.

**FIXED 2026-07-26, defensively.** The first fix that suggests itself -- move the
`gc_remove_suspect(cb)` call *after* the identity guard -- is wrong: the guard
also returns for an *owned* block that is no longer registered, and skipping the
suspect scan for those is exactly the stale-pointer case CG1 added the call for.

The correct minimal change is to clear the flag only on a real hit, moving
`cb->gc_buffered = false;` inside the found-branch of the scan. Then a foreign
block is never written to at all, and an owned block behaves as before. Leaving a
stale `true` on an owned-but-absent block is harmless by comparison -- it costs a
later rescan that finds nothing.

Applied to **both** collector copies (`src/runtime/gc.c` and the replica in
`emit_module.c`); `tools/gc-copy-diff.py` still reports 27 divergent / 0 emitted-
only, unchanged. Suite 2297 passed, 0 failed.

Because reachability was never demonstrated, this is a hardening rather than a
confirmed bug fix -- it makes the hazard structurally impossible instead of
resting on the argument that it cannot be triggered.

## Inference (not measured): cross-boundary cycles are uncollectable

Follows directly from finding 2 rather than from a separate experiment.
`gc_mark_phase` iterates `gc_all_blocks`, and each registry holds only its own
blocks, so a cycle with nodes on both sides of the boundary can never be fully
traced by either collector. The result is a leak, not corruption -- the same
class as the `Vec`/HAMT blind spot in
[collections-cannot-hold-rc-values.md](collections-cannot-hold-rc-values.md)
item 3, and acceptable for the same reason. Worth stating so nobody assumes
enabling the collector on both sides covers it.

## Aside: `gc_possible_root` is archive-only

The archive factors the Bacon-Rajan PossibleRoot edge into a named function
(`gc.c:716`, called from `rc.c:346`); the replica **inlines** the same edge in
`rc_strong_decrement` as `if (gc_mode != GC_DISABLED) gc_add_suspect(cb);`.
Behaviourally identical -- consistent with `gc-copy-diff.py`'s "27 divergent, all
cosmetic" -- but the *symbol* exists only in the archive.

The failure mode is worse than the divergence: inline-C calling
`gc_possible_root` compiles and **links** in a `--shared` build (a shared object
tolerates undefined symbols at link time) and fails only at `dlopen`:

```
dlopen: librc2.so: undefined symbol: gc_possible_root
```

Encountered while writing the probe above. Not a correctness bug; a sharp edge
for anyone writing inline-C against the runtime who tests on the archive path
first.

## Verdict

The headline question is answered: **two collectors in one process is sound
today**, and for a better reason than the plan assumed -- not because values stay
on one side, but because `gc_unregister_block` validates the back-reference
before mutating its array. The `gc_remove_suspect` gap is now closed in
both copies. What remains is documentation: the "values do not cross" premise
should be dropped from the safety argument, since it is unenforced and is not
what makes the thing safe.

## Resolution of item 1

The plan doc (`gc-cycle-collection-followup-plan.md`) had already been corrected
in the same pass that filed this -- it states outright that the safety does not
come from the premise. The **user-facing** guide had not. Its collector-copy
table made only a visibility claim ("its collector is not exported, so a host
that dlopens it cannot partially merge the two registries"), which is true but is
not the safety argument, and said nothing about values crossing, about what
actually guards the registry, or about the cycle leak.

Added a "Two collectors in one process" section to
[docs/guides/gc-guide.md](../../guides/gc-guide.md) covering, in the order a reader
needs them:

- the premise is **not** what makes it safe, with the `defn`-returning-`rc<T>`
  counterexample and the note that the manifest types the export as `:any`, so
  the boundary is untyped in both directions;
- the actual guard -- `gc_unregister_block` validating `gc_all_blocks[idx] != cb`
  before mutating;
- the two supporting properties (no exported collector symbols, separate
  registries), phrased as things you can check rather than claims;
- cross-boundary cycles being uncollectable, and that enabling the collector on
  both sides does not cover it;
- the `gc_possible_root` archive-only asymmetry, which links fine in a `--shared`
  build and fails at `dlopen`.

### Re-verified before writing, not copied forward

Every measurement the guide now states was re-run against the current tree, since
a doc that asserts stale numbers is worse than one that says nothing.
`tur build --shared tests/fixtures/build-shared-smoke`:

| claim | measured |
| --- | --- |
| exported `gc_*`/`rc_*` dynamic symbols | **0** |
| exported module symbols | **2** |
| `gc_possible_root` in the `.so` | **absent** |
| `gc_possible_root` in `libturt_runtime.a` | **present** |

The last two together are the archive-only asymmetry, confirmed rather than
assumed.
