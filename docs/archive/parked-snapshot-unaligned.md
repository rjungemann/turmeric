# r7rs-gc never saw a parked thread's registers on macOS

**RESOLVED 2026-09-29**, in the PR that added
`tests/fixtures/r7rs-threads-fiber-migration`. The collector's copy of a
parked thread's registers is now aligned for the scan, and it is taken in the
frame that makes the blocking call.

## Symptom

While fixing `fiber-tls-address-reuse`, any change to how `tur_gc_park` or
`tur_gc_unpark` read the thread's record made `r7rs-threads-roots` fail on
macOS under `TUR_GC_TORTURE=1` in every run, Release and Debug:
`error: +: not a number`. The worker thread's list had been freed while it
waited on a condition variable holding the only reference. The failures
followed the code's shape, not its meaning. A fresh read in unpark alone
broke it. So did taking the register copy in the caller's frame, which
should have been the more careful version. Leaving park and unpark exactly
as on `main` passed. Linux, under gcc and clang, passed every version.

## What was happening

A thread parked in a blocking call is not stopped by signal. It copies its
callee-saved registers into its record with `setjmp` and records its stack
pointer, and the collector scans that copy and the stack above the pointer.

On macOS `jmp_buf` is `int[48]`, so it is only 4-byte aligned. In the
record it followed seven `int` fields, which put it at offset 84, 4 mod 8.
The scan reads aligned 8-byte words, so each saved register straddled two
of the words it read, and the copy hid every register it held. On Linux,
glibc's `jmp_buf` holds `long`s and is 8-aligned, which is why the bug did
not show there.

So on macOS a parked thread's roots were found only when they happened to
be on the stack too. On `main`, `r7rs-threads-roots` passed because the
worker's list pointer was also saved on its stack, somewhere the scan
covered. Any change to park, unpark, or the code inlined around them could
move it into a register instead.

The copy had a second, smaller problem. `tur_gc_park` was a function that
took the copy from inside its own frame. Any caller value it had saved in
order to use that register itself was in its frame, not in the copy, and
the blocking call's frames wrote over that frame once park returned.

## Fix

- `regs` and `sig_regs` in the thread record, and the collector's own
  `jmp_buf` in `tur_gc_mark_roots`, are declared `aligned(16)`. The stop
  handler's `sig_regs` was misaligned too. It did no harm: a stopped
  thread's registers are also in the kernel's signal frame, which is on
  the stack the scan covers.
- `tur_gc_park` is a macro. `setjmp` runs in the frame that makes the
  blocking call, which is live until the unpark. A noinline helper,
  `tur_gc_park_at`, records the stack pointer below that frame. Each caller
  value is then either in the copy or in a frame the scan covers.

## Verified

macOS, Apple clang 21, Release and Debug, a probe that rebuilt each
fixture's C with the change and with parts of it taken back out (under
`TUR_GC_TORTURE=1`, lifecycle and migration under 31):

| fixture | this fix | alignment taken out | park spilling its own frame, aligned |
| --- | --- | --- | --- |
| r7rs-threads-roots | 6 of 6 | 0 of 6 | 6 of 6 |
| r7rs-threads-pause | 4 of 4 | 0 of 4 | 4 of 4 |
| r7rs-threads-syscall | 3 of 3 | 3 of 3 | 3 of 3 |
| r7rs-threads-lifecycle | 3 of 3 | 3 of 3 | 3 of 3 |
| r7rs-threads-fiber-migration | 8 of 8 | 8 of 8 | 8 of 8 |

The last column shows that the alignment alone fixed what was seen. The
frame change closes the hole described above, which these fixtures happen
not to reach. `tests/run-r7rs-gc.sh` on macOS: Debug 215 of 215; Release
213 of 215. The two Release failures are Saffron refinement checks that
exit 0 where a violation should fail, which has nothing to do with the
collector.

Linux: `tests/run.sh`, `tests/run-r7rs-gc.sh` (gcc; clang 18 but for the
five checks that link the ASan-built libturi, which this container's clang
cannot link, as on `main`), `tests/check-r7rs-prelude-split.sh`.
