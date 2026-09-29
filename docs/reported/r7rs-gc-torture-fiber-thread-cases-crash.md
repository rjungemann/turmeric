# The r7rs-gc torture run crashes intermittently in the fiber and thread cases

**Severity: medium (a memory-safety crash under the collector; fails the
Auxiliary CI legs).** Under `TUR_GC_TORTURE=31` (a collection every 31
allocations) the compiled fiber/thread fixtures crash some of the time,
and which one fails changes from run to run. The pattern in CI so far:

| run | case | outcome |
| --- | --- | --- |
| `main` 36613837100 (#969) | `r7rs-threads-fiber-dynamic-env` | exit 139 |
| `main` 36613837100 (#969) | `threads-fiber-dynenv` | exit 134, `stack smashing detected` |
| #970 at `202c517e`, ubuntu | `threads-fiber-dynenv` | exit 139 |
| #970 at `202c517e`, macOS | `tur_r7rs_gc` | ctest `TIMEOUT` 720 s after `threads-lifecycle`: a hang |
| #970 at `9a78c6bc`, ubuntu | `r7rs-threads-fiber-migration` | exit 139 |

Filed 2026-09-29 from rjungemann/turmeric#970, where it was established as not
that PR's: `tur emit-c` for `r7rs-threads-fiber-migration` is byte-identical on
`origin/main` and the PR branch.

## Repro

```sh
d=tests/fixtures/r7rs-threads-fiber-migration
./build/tur build $d/input.tur -o /tmp/fm
for i in $(seq 60); do
  (cd $d && ASAN_OPTIONS=detect_leaks=0 TUR_GC_TORTURE=31 timeout 60 /tmp/fm </dev/null >/tmp/fm.out 2>/dev/null)
  echo $?
done | sort | uniq -c
```

On a 4-core Linux box: 10 of 60 runs crashed on `origin/main` (`36a9b88`) and
15-16 of 60 on the #970 branch. Segfaults (139) and bus errors (135) both show
up.

## What it is not

Not the thread-local rewind fixed in
[jit-r7rs-callcc-reads-unknown-tag-after-per-thread-dynenv](../archive/jit-r7rs-callcc-reads-unknown-tag-after-per-thread-dynenv.md)
(a call/cc image that took a non-main thread's TLS with it): with that fix the
rate is unchanged, 15 of 60.

## Where to look

All the failing cases came with #966 (fibers migrating between threads under
the collector) and #969 (`82867413`, the per-fiber dynamic environment). A
fiber's stack is a malloc'd block, not a thread's mapping, so two things are
worth checking first:

- whether the collector scans a parked fiber's stack and its `r7dyn` field
  (the `FiberBlock` word that holds the fiber's dynamic environment) on every
  collection, including while the fiber is between threads;
- what `r7k_stack_base` returns for a capture made on a fiber stack. It asks
  for the *thread's* mapping, so the image of a capture on a fiber is measured
  against the wrong stack.

A core from a crashing run, or the run under `TUR_GC_TORTURE=1`, is the next
step.
