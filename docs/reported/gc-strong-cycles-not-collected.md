---
status: open
severity: medium
discovered: 2026-07-24
area: runtime (Bacon-Rajan cycle collector, src/runtime/gc.c)
---

# The cycle collector cannot reclaim live `rc<T>` strong cycles

## Summary

The Bacon-Rajan cycle collector (`src/runtime/gc.c`) does **not** collect a live
strong-reference `rc<T>` cycle -- even when GC is explicitly enabled and
`(gc!)` is called. The `docs/guides/gc-guide.md` "Known gaps" bullet
("Programs that build cycles need to call `(gc!)` explicitly, or opt into
`GC_THRESHOLD`") therefore advertises a remedy that does not work for the case
it names. Only the **weak-zombie** case (a block whose strong count reaches 0
while a `weak<T>` still observes it) is ever reclaimed.

**Impact is currently latent:** the collector is off by default (`GC_DISABLED`,
`gc.c:33`) and stdlib builds no `rc<T>` cycles at all (see
`stdlib-weak-ref-audit-plan.md`), so nothing in-tree hits this today. It is a
correctness gap in a shipped-but-dormant subsystem, not a live production bug --
filed so the guide's promise and the collector's behavior are reconciled before
anyone relies on automatic cycle collection.

## Root cause -- two independent reasons, not one

A genuine strong cycle `A -> B -> A` keeps `strong_count > 0` for every member
(each is referenced by the other). Two separate things in `gc.c` each
independently prevent collection:

1. **Suspects are only buffered for zombies.** `rc_strong_decrement` calls
   `gc_on_strong_decrement` only on strong->0 with weak>0 (`rc.c:180-184`), and
   that function guards again on `weak_count > 0` (`gc.c:369`). A cycle member's
   count never reaches 0, so it is never buffered as a suspect. Classic
   Bacon-Rajan buffers a "possible root" on every decrement that leaves the count
   **> 0** -- that hook is absent.

2. **The mark phase treats every live block as a root.** Even if suspects were
   buffered, `gc_mark_phase` marks *every* registered block with
   `strong_count > 0` BLACK (a root) (`gc.c:249-255`). A cycle member always has
   `strong_count > 0` from its in-cycle back-edge, so it is unconditionally
   marked live. Distinguishing internal (in-cycle) edges from external roots
   requires trial *decrements* over the candidate subgraph -- the current code
   has no such notion, so the "trial deletion phase" (`gc.c:296-343`) is really a
   mark-sweep-from-strong-roots that structurally cannot identify a
   self-sustaining cycle.

This is confirmed in-tree by the fixture's own header,
`tests/fixtures/exg5-exists-cycle/input.tur:22-26`: *"The current Bacon-Rajan
trial-deletion is still zombie-only ... so the live cycle is not reclaimed by
`gc!` today."*

## Minimal repro

`tests/fixtures/exg5-exists-cycle/input.tur` already builds the topology:
`(defstruct S :move [next : rc<S>])`, wire `s1 <-> s2`, both end at
`strong_count = 2`, `(gc!)` runs, both survive. The two `S` control blocks are
then leaked whether GC is disabled, manual, or threshold -- but see **Measured**
below for why LeakSanitizer alone does *not* surface this.

## Measured (2026-07-24)

Empirically confirmed on a Debug build (`v0.30.8`, gcc 13, Linux) with four
probes looping the cycle build 200000 times and reading `mallinfo2().uordblks`
before/after. The cycle body is a self-referential struct
`(defstruct S :move [next : rc<S>])` wired into a two-node ring:

    (defn build-cycle [] : int
      (let [s1 (rc/of (make-struct S (null-rc-s)))     ; null-rc-s: inline-C NULL sentinel
            s2 (rc/of (make-struct S (rc/clone s1)))]
        (set! (.next s1) (rc/clone s2))   ; both at strong=2; scope-exit drops each to 1
        0))

Heap retained over 200000 iterations:

| Probe | Configuration | Retained | Per cycle |
|-------|---------------|---------:|----------:|
| acyclic control (`rc/of` + `rc/drop`) | -- | **0 B** | **0** |
| strong cycle | GC off (default `GC_DISABLED`) | 38,400,000 B | **192 B** |
| strong cycle | `(gc-enable!)` + `(gc!)` | 38,400,000 B | **192 B** |

Three conclusions: (a) plain RC is leak-clean (0 B retained across 200000
build/drop cycles); (b) a strong cycle leaks 192 B each (2 nodes x a 96-B
control-block+value allocation); (c) **`(gc!)` reclaims nothing** -- the
GC-enabled column is byte-for-byte identical to GC-off. This is the direct
measurement behind the analysis above.

### Why LeakSanitizer alone reports "clean"

Running the *same* probe under `ASAN_OPTIONS=detect_leaks=1` reports **no leaks**
for a single cycle. Cause: `rc_cb_alloc_kinded` registers every control block in
the static global `gc_all_blocks[]` (`rc.c:109`), so a leaked block stays
*reachable* from that global and LSan files it under "still reachable" (never
printed) rather than "leaked." This is why leak-checking the compiled program's
runtime would not catch cycle garbage even if the suite did it.

The masking is bounded by the 4096-entry registry cap, which makes it *exactly*
observable. Looping 202000 cycles under LSan reports:

```
SUMMARY: AddressSanitizer: 25,593,856 byte(s) leaked in 799,808 allocation(s)
  -> 199,952 leaked objects per allocation site
```

`202000 - 199952 = 2048` cycles are *not* reported == `4096 blocks / 2 per cycle`
== the registry cap. The first 4096 blocks are pinned in `gc_all_blocks[]`
(invisible to LSan); every block past the cap is silently dropped by
`gc_register_block` (`gc.c:207-210`) and *becomes* LSan-visible. One run thus
confirms both this report's headline and the registry-overflow defect below, to
the integer.

## Related latent defects discovered alongside (same subsystem, GC-enabled only)

1. **Registry overflow silently drops blocks.** `gc_register_block` is a no-op
   once `gc_all_blocks_count >= 4096` (`gc.c:207-210`; the suspect and grey
   arrays are likewise static 4096, `gc.c:22,28`). A program with >4096 live
   `rc<T>` blocks has unregistered blocks that are invisible to the collector --
   a correctness cliff, not just a capacity limit. Needs dynamic growth
   (`gc-cycle-collection-plan.md` CG0).

2. **Trial deletion dangles live weak pointers.** When freeing a WHITE suspect
   with `weak_count > 0`, `gc_trial_deletion_phase` zeroes the weak count and
   frees the whole control block (`gc.c:335-340`), contradicting the zombie
   contract the rest of RC upholds (`rc.c:180-183`, where a strong->0 block with
   weak>0 is kept alive so `upgrade` can return none). Any live `weak<T>` to that
   block becomes a dangling pointer -> use-after-free on the next `upgrade`.
   Needs the zombie discipline (`gc-cycle-collection-plan.md` CG4).

## Fix directions

The full remedy is spec'd in `docs/upcoming/v1/gc-cycle-collection-plan.md`:
CG1 adds the possible-root buffering, CG2 replaces the mark phase with real
trial-deletion (MarkGray/Scan/CollectWhite over the candidate set), CG0 fixes the
registry cliff, CG4 fixes the weak dangling. Until then, the accurate user story
is "manual `weak<T>` cycle-breaking, exactly like Rust" -- not "call `(gc!)`."

## Recommendation

Not v1-blocking (dormant, opt-in, no in-tree trigger). The trigger to act is any
move to advertise or default-on automatic cycle collection: at that point CG0/CG1/
CG2/CG4 must land together, since each alone leaves the collector unsound or
ineffective. In the meantime, the highest-value cheap fix is the doc correction
(`docs/reported/gc-guide-stale-and-misleading.md`) so no one relies on the
non-working remedy.
