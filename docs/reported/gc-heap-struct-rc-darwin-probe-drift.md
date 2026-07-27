# `gc-heap-struct-rc` fails on macOS only: the Darwin heap probe drifts 16320 bytes

**Severity: low** (test-only; no evidence of a collector defect). `Test
(macos-latest)` is red on this fixture, and has been on every run since well
before it was noticed -- it reproduces on `main` and on PR #729's base commit
with byte-identical output, so it is not a regression from any one change.

```
FAIL gc-heap-struct-rc — stdout mismatch
 0
-0
+16320
```

The failing line is the second (cyclic) assertion in
`tests/fixtures/gc-heap-struct-rc/input.tur`: after `(run-ring 5000)` the
fixture asserts that its `heap-bytes` probe reports zero net growth, and on
Darwin it reports 16320.

## Why this looks like the probe, not the collector

Three independent pieces of evidence, all reproducible on Linux:

1. **There is no platform-specific code in the allocator path.**
   `src/runtime/rc.c`, `src/runtime/gc.c`, and `src/runtime/rc_free_queue.c`
   contain no `__APPLE__` / `__MACH__` / `TARGET_OS` split at all. A genuine
   macOS-only *leak* would need a macOS-only code path to leak in. The only
   thing in this fixture that is spelled per-platform is the probe itself.

2. **Linux stays byte-exact zero at 4x the iteration count.** Raising both
   loops from 5000 to 20000 still prints `0` / `0`:

   ```sh
   sed -e 's/(run-acyclic 5000)/(run-acyclic 20000)/' \
       -e 's/(run-ring 5000)/(run-ring 20000)/' \
       tests/fixtures/gc-heap-struct-rc/input.tur > /tmp/gcbig.tur
   ./build/tur build /tmp/gcbig.tur -o /tmp/gcbig && /tmp/gcbig
   # => 0
   #    0
   ```

   A per-iteration leak would scale with the iteration count. This one does not
   appear on glibc at all.

3. **The magnitude is not an object size.** 16320 bytes over 5000 ring
   iterations is ~3.26 bytes/iteration -- no allocation in the ring is 3.26
   bytes, and a leak cannot be fractional. 16320 is however within 64 bytes of
   16 KiB, which is the shape of a *single* region/page-granule carve, i.e. a
   one-time step in allocator capacity rather than retained objects.

## Root cause direction

The two probes do not measure the same quantity:

| platform | probe | measures |
|---|---|---|
| glibc | `mallinfo2().uordblks` | live allocated bytes, whole heap, byte-exact |
| Darwin | `malloc_zone_statistics(malloc_default_zone(), &ms).size_in_use` | one zone, region-granular |

Two distinct problems on the Darwin side:

- **It measures a single zone.** macOS serves small allocations from the *nano*
  zone, which is a different zone from the one `malloc_default_zone()` returns.
  Anything allocated outside the measured zone is invisible, and any migration
  between zones shows up as drift.
- **`size_in_use` reflects region capacity, not live bytes**, and the nano
  allocator never returns memory to the OS. So the number steps up when a
  region is carved and does not come back down on free -- exactly a bounded,
  one-time `+16 KiB`-shaped delta.

The fixture's 500-iteration warmup is evidently not enough to push Darwin's
allocator to its high-water mark before the measured window opens.

## Fix directions

Not attempted here: this cannot be verified without a macOS box, and guessing
at a fix in a file whose failure mode is only observable on the platform you
cannot run is how you get a second wrong number. Options, roughly in order of
preference:

1. **Make the two probes measure the same thing.** Sum `size_in_use` across all
   zones via `malloc_get_all_zones()` instead of reading only the default zone.
   Caveat worth measuring first: if the residual is nano-zone high-water, this
   could *include* more capacity noise rather than less.
2. **Assert on a second window instead of the first.** Run the ring loop twice
   and assert only on the second window's delta. A one-time region carve lands
   in window 1 and leaves window 2 at zero; a real per-iteration leak recurs in
   both. This needs no magic threshold and no per-platform tolerance.
3. **Per-platform noise floor.** Keep the byte-exact assertion where the probe
   is byte-exact (glibc) and allow a documented floor where it is granular
   (Darwin). Weakest of the three -- it hides small leaks by construction --
   but it is the smallest change.

Note that whichever is chosen, the assertion still has real teeth on glibc: it
is what caught the `set!` refcount leak fixed in PR #729 (16000 -> 0 live
blocks).

## Related

- `docs/upcoming/v1/gc-cycle-collection-followup-plan.md` -- records the
  adjacent CG7 finding that `mallinfo2` is blind under ASan, the same class of
  problem (a heap probe that does not measure what the assertion assumes).
- PR #729 discussion, where this was first triaged as pre-existing and
  deliberately not folded in.
