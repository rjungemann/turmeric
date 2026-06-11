---
title: ecs/sparse deletion silently leaks entries because insert is linear-probe but delete is Robin Hood backward-shift
category: Reported
severity: Correctness bug in tur-ecs sparse storage (lives in the spice, not the compiler)
discovered: 2026-06-11, during ECS spice E1' execution (docs/upcoming/ecs-spice-plan.md)
resolved: 2026-06-11, by porting `ecs/sparse.tur` to full Robin Hood with a
  parallel `uint8_t probe_dist[]` array. The proposed fix (below) shipped
  unchanged; martinus/robin-hood-hashing v3.11.5 was the port reference.
location: `../turmeric-spices/spices/ecs/src/ecs/sparse.tur` -- `sparse-set!` and `sparse-del!`
---

# `ecs/sparse` deletion silently leaks entries because insert is linear-probe but delete is Robin Hood backward-shift

> **Status: fixed.** The 500-entry leak repro
> (`tests/sparse-rt-large.tur` in the spice) now prints `500, 250, 0`.
> A 10,000-op stress test (`tests/sparse-stress.tur`) interleaving
> insert/delete over a 2,000-index keyspace cross-checks `sparse-len`
> and `sparse-has?` against a parallel `ecs/tag` bitset and reports
> `ok`. The original 100-entry round-trip (`tests/sparse-rt.tur`) and
> all five other ECS tests still pass.

## Summary

`sparse-set!` performs plain linear-probing insertion (first empty
slot wins). `sparse-del!` performs Robin Hood backward-shift deletion
that stops the shift loop when "the next entry is at displacement 0
from its ideal slot." That stop condition is only sound under the
**Robin Hood invariant** (entries along a probe chain are ordered by
displacement), which plain linear-probe insertion does not maintain.

The mismatch is silent. As long as the table is sparse enough that no
two probe chains overlap (the 100-entry / `cap = 256` round-trip in
`tests/sparse-rt.tur`), deletions succeed. Once collisions force
chains to interleave, `sparse-del!` can stop the shift loop at an
entry whose displacement happens to be 0 but which sits *between* an
earlier entry's now-deleted ideal slot and that earlier-displaced
entry's actual home. The gap left behind orphans the displaced entry:
its probe chain from its ideal slot now hits `EMPTY` before reaching
it.

## Concrete failure

Insert `1, 3, 5, ..., 999` into a fresh `(sparse-new)` (grows through
`cap = 16, 32, ..., 1024`, settles at `cap = 1024`, load ~0.49). Then
attempt to delete every-other one (`j = 0, 2, ..., 498`, so keys
`1, 5, 9, ..., 997` -- 250 deletions). Three deletions silently
return `false`:

```
500    ;; sparse-len after inserts
857    ;; sparse-del! 857 returned false
945    ;; sparse-del! 945 returned false
993    ;; sparse-del! 993 returned false
253    ;; sparse-len at end (expected 250)
3      ;; lost counter
```

The lost keys all sit near the top of the table where multiple probe
chains have wrapped across `idx = cap - 1`. Lower-load reproductions
(100 entries / `cap = 256`, load ~0.39) leak nothing because no two
chains intersect.

## Severity

Silent correctness bug in tur-ecs. `sparse-del!` returns `false` but
the entry stays live: `sparse-has?` keeps returning `true`, `sparse-len`
overshoots the live-key count, and any caller iterating "keys still in
the storage" sees stale entries forever. For an ECS world that
despawns entities at scale (a long-running simulation, a streamed-in
level), this is a slow leak of component values the rest of the world
believes to be live.

A caller that re-probes with `sparse-has?` after every `sparse-del!`
can detect the failure, but the underlying invariant -- "if a key is
in the table, its probe chain from its ideal slot is unbroken across
arbitrary insert/delete sequences" -- is broken.

## Root-cause analysis

`sparse-set!` (`ecs/sparse.tur` around lines ~94-117) does plain
linear probing:

```c
uint64_t mask = (uint64_t)(st->cap - 1);
uint64_t k = (uint64_t)idx;
uint64_t h = k * 0x9E3779B97F4A7C15ULL; h ^= h >> 32;
uint64_t p = h & mask;
while (st->keys[p] != UINT64_MAX && st->keys[p] != k) p = (p + 1) & mask;
if (st->keys[p] == UINT64_MAX) { st->keys[p] = k; st->count++; }
((__TUR_TY_A__ *)st->vals)[p] = val;
```

There is **no Robin Hood swap**: a freshly-inserted high-displacement
entry never displaces an already-resident low-displacement one. The
table satisfies the linear-probe invariant ("an empty slot terminates
any probe chain that starts at or before it") but not the Robin Hood
invariant ("displacement is monotonic along a probe chain").

`sparse-del!` (lines ~149-185) implements canonical RH backward-shift:

```c
size_t cur = p;                                      /* p = found slot for k */
for (;;) {
  size_t nxt = (cur + 1) & mask;
  uint64_t nk = st->keys[nxt];
  if (nk == UINT64_MAX) break;                       /* empty -- chain ends */
  uint64_t nh = nk * 0x9E3779B97F4A7C15ULL; nh ^= nh >> 32;
  uint64_t ideal = nh & mask;
  if (((nxt - ideal) & mask) == 0) break;            /* nxt is at home */
  st->keys[cur] = nk;
  memcpy(...);
  cur = nxt;
}
st->keys[cur] = UINT64_MAX;
st->count--;
```

This is the algorithm from Goossaert 2013 and from Celis's 1986
thesis -- both written for Robin Hood tables. The stop condition
("next entry is at home -- don't move it") is *sound for RH* because
under the RH invariant, an at-home entry guarantees no later entry in
the table has a probe chain crossing the current slot. Without the
RH invariant, that guarantee does not hold.

### Worked example (linearised, no wrap, for clarity)

Suppose after a series of plain linear-probe inserts:

| slot | key | ideal | displacement |
|------|-----|-------|--------------|
| 5    | A   | 5     | 0            |
| 6    | B   | 6     | 0            |
| 7    | C   | 5     | 2            |

C was inserted last; its probe walked `5 (A) -> 6 (B) -> 7 (empty)`
and parked at displacement 2. RH would have *swapped* C past A/B
along the way, ending with C earlier and A/B later, but linear-probe
insertion left this layout.

Now delete A. The backward-shift loop visits `nxt = 6` and finds B
with displacement 0. **Stop**. Slot 5 is cleared. But C's probe chain
from its ideal slot 5 now reads `EMPTY` immediately -- C is
unreachable, even though it still sits at slot 7. The next
`sparse-del! C` walks slot 5 (empty) and returns `false`.

The 857/945/993 leak is exactly this scenario, instantiated three
times across wrap boundaries in the cap-1024 table.

## Why my first analysis (unbounded loop) was wrong

My first pass blamed the loop's lack of a `cap`-iteration safety
cap. The literature survey corrected that:

- martinus/robin_hood.h's `shiftDown` (3.11.5 `src/include/robin_hood.h`
  lines 1396-1410) is also unbounded; it terminates via the same
  empty-or-DIB-0 conditions plus a trailing sentinel byte set in
  `mInfo[numElementsWithBuffer]`.
- Goossaert 2013 ("Robin Hood hashing: backward shift deletion",
  <https://codecapsule.com/2013/11/17/robin-hood-hashing-backward-shift-deletion/>)
  states the invariant verbatim: "shift backward all the entries
  following the entry to delete until either an empty bucket, or a
  bucket with a DIB of 0." No length cap.

The shift loop is fine. The premise it depends on is what's missing.

## Why dense doesn't have this

`ecs/storage.tur`'s dense storage has no probe chain; deletion is a
one-bit flip on `present[idx]`. The bug is exclusive to the
open-addressing sparse module.

## Proposed fix: go fully Robin Hood with a parallel `probe_dist[]` byte array

The cleanest long-term fix is to commit to Robin Hood properly --
both on insert and delete -- and lean on the published code in
martinus/robin_hood.h. Concretely:

1. **Add `uint8_t *probe_dist` to the control block**, one byte per
   slot. `probe_dist[i] = 0` means slot `i` is empty; `probe_dist[i] =
   d > 0` means slot `i` holds an entry at displacement `d - 1` from
   its ideal slot. (Reserving 0 for empty matches the martinus encoding
   and lets the shift loop terminate on `probe_dist[nxt] == 0` without
   a separate empty check.)

2. **Rewrite `sparse-set!` to do Robin Hood swap.** Walk forward from
   `ideal`; at each slot, if the resident entry's displacement is
   *less than* the incoming entry's, swap them and continue inserting
   the displaced entry. The published code in martinus/robin_hood.h's
   `shiftUp` (lines 1377-1394) is the direct port target.

3. **Rewrite `sparse-del!` to consult `probe_dist[nxt]`** instead of
   recomputing the displacement from the hash. The shift loop becomes:

   ```c
   while (probe_dist[(cur + 1) & mask] >= 2) {      /* >= 2 == displacement >= 1 */
     keys[cur] = keys[(cur + 1) & mask];
     memcpy(vals + cur*sz, vals + ((cur+1)&mask)*sz, sz);
     probe_dist[cur] = probe_dist[(cur + 1) & mask] - 1;
     cur = (cur + 1) & mask;
   }
   probe_dist[cur] = 0;
   ```

   No `* 0x9E3779B97F4A7C15` rehash per iteration, no modular
   subtraction, no wrap-around math. The loop terminates naturally
   when it hits a displacement-0 entry *or* an empty slot, both
   encoded as `probe_dist[nxt] < 2`.

4. **Wrap handling via a buffer slot.** Allocate `cap + max_probe_len`
   slots so the shift loop never crosses index `cap`. Set
   `probe_dist[cap + max_probe_len - 1] = 0` as a permanent sentinel.
   This is the `numElementsWithBuffer` pattern from robin_hood.h around
   lines 1162 and 1599 -- it makes the modular arithmetic disappear
   from the hot path entirely.

5. **Cap the longest probe at insert time.** martinus uses `max_probe
   = log2(cap)`; Skarupke uses the same in "I Wrote The Fastest
   Hashtable" (<https://probablydance.com/2017/02/26/i-wrote-the-fastest-hashtable/>).
   If a probe would exceed `max_probe`, force a rehash. This bounds the
   `probe_dist` byte to a single byte safely (`max_probe < 256` for any
   table that fits in addressable memory) and keeps lookups predictable.

### Reference implementation to port from

**martinus/robin-hood-hashing v3.11.5, `src/include/robin_hood.h`**:

| Concept | File:line | What to look at |
|---|---|---|
| `mInfo` (probe-distance) array decl | ~1599, 1657 | Allocated alongside `mKeyVals`; same length |
| `mInfoInc` per-step increment | 1358, 1365 | Low bits hold a hash fingerprint, upper bits hold displacement -- we likely don't need the fingerprint trick for E1' |
| `shiftUp` (RH insert) | 1377-1394 | The swap-on-insert loop |
| `shiftDown` (RH delete) | 1396-1410 | Loop guard: `while (mInfo[idx + 1] >= 2 * mInfoInc)` |
| `numElementsWithBuffer` (wrap sentinel) | ~1162, 1599 | Trailing buffer slot + sentinel |

Raw source for the bug-fixer to pull alongside the report:
<https://raw.githubusercontent.com/martinus/robin-hood-hashing/3.11.5/src/include/robin_hood.h>.

### Why not tombstones (alternative considered, not recommended)

A second viable fix is to keep linear-probe insertion and replace
backward-shift with tombstones. This is what Google's SwissTable
(`absl::flat_hash_map`) and Rust's `hashbrown` do, encoded in a
parallel `ctrl[]` byte array (one byte per slot, low 7 bits hold a
hash fragment, top bit distinguishes EMPTY/DELETED). Probes treat
tombstones as "keep going," inserts as "reusable."

References:
- hashbrown v0.14 `src/raw/mod.rs` lines 97/100 (`EMPTY`/`DELETED`),
  960/968 (`erase_no_drop` / `erase`):
  <https://github.com/rust-lang/hashbrown/blob/v0.14.0/src/raw/mod.rs>
- abseil `raw_hash_set.h` (same scheme):
  <https://github.com/abseil/abseil-cpp/blob/master/absl/container/internal/raw_hash_set.h>

Why not this for ECS sparse: tombstones grow with delete count and
require periodic rehash to clean up, which makes worst-case `set!`
latency unpredictable for a real-time game loop. Robin Hood's
backward-shift gives bounded latency per operation, which matches the
ECS surface better. The cost (an extra byte per slot, a swap-on-insert
loop) is small and shipped widely.

### Tactical hotfix while the proper fix is being landed

A one-line workaround that doesn't move us toward the real fix but
*does* stop the leak: in `sparse-del!`, replace the shift loop with
an unconditional walk to the next empty slot, shifting *every*
entry along the way regardless of displacement. This is O(probe-cluster-
length) per delete -- slower than backward-shift -- but it preserves
the linear-probe invariant the table actually has. The 500-entry repro
prints `500, 250, 0` under this change. Reserve for the case where the
proper Robin Hood port is more than a session's work.

## Validation plan

A fix is validated when:

- The 500-entry repro above prints `500, 250, 0` (no lost lines).
- A randomised stress test that interleaves `sparse-set!` and
  `sparse-del!` over `cap * 0.9` live entries for `10 * cap` operations,
  cross-checked against a parallel ground-truth bitset, reports
  `lost == 0` and `sparse-len == popcount(bitset)` throughout.
- The existing 100-entry `tests/sparse-rt.tur` continues to pass
  unchanged.
- Every `sparse-has?` lookup after the stress test returns the same
  answer as the ground-truth bitset.
- Insert/lookup throughput on a benchmark of 1M operations stays
  within 1.5x of the current linear-probe baseline (the Robin Hood
  swap on insert is the perf risk; if it regresses meaningfully we
  fall back to the tactical hotfix above).

Until a fix lands, callers that need exact deletion counts should
re-probe with `sparse-has?` after every `sparse-del!` and treat a
disagreement as the trigger for a full table rebuild.

## References

- Pedro Celis. *Robin Hood Hashing.* Tech. Report CS-86-14, Doctoral
  thesis, University of Waterloo, 1986. (ACM DL stub:
  <https://dl.acm.org/doi/10.5555/13298>; the Waterloo PDF is not
  reliably mirrored -- cite by report number.)
- Emmanuel Goossaert. *Robin Hood hashing: backward shift deletion.*
  Code Capsule, 2013-11-17.
  <https://codecapsule.com/2013/11/17/robin-hood-hashing-backward-shift-deletion/>
  -- the canonical statement of the deletion invariant.
- Malte Skarupke. *I Wrote The Fastest Hashtable.* Probably Dance,
  2017-02-26.
  <https://probablydance.com/2017/02/26/i-wrote-the-fastest-hashtable/>
  -- the probe-distance array layout and the `max_probe = log2(cap)`
  bound.
- martinus/robin-hood-hashing v3.11.5,
  <https://github.com/martinus/robin-hood-hashing> -- the C++ reference
  implementation to port from. File:line references above.
- martinus/unordered_dense -- successor library, same backward-shift
  pattern, used in production. <https://github.com/martinus/unordered_dense>

## See also

- `../upcoming/ecs-spice-plan.md` -- E1' shipped sparse with this bug
  acknowledged in the README's "Known limitations" section.
- `ecs-macro-symbol-synthesis-missing.md`,
  `generic-return-type-not-inferred-from-context.md`,
  `macro-backquote-dot-sym-drops-siblings.md` -- the three
  language-level gaps surfaced alongside this storage-level bug
  during the same E1' session.
