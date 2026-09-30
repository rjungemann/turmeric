# The r7rs-gc torture run crashes intermittently in the fiber and thread cases

**RESOLVED 2026-09-30.** Not a fiber or thread bug: glibc's x86-64 `setjmp`
stores `rbp` pointer-mangled, and every register spill the collector reads
was a `setjmp`. A root a function kept in `rbp` across a park was therefore
never in the words the scan saw. Here that root was main's freshly made
`FiberBlock`, held in `rbp` across the contended scheduler lock inside
`tur_scheduler_mt_spawn`; a torture collection while main was parked there
freed the block and its stack, and main then enqueued a dangling block --
every crash shape below is that reuse. `TUR_GC_FP` (src/runtime/r7gc.c) now
records the frame pointer as a plain word at each spill (the park, the stop
handler, the collector's own) and the scan marks it. Measured on the repro
below: 0 of 200 runs fail (100 split, 100 single-unit) where 6 of 40 to 23 of
80 failed before; `r7rs-threads-fiber-dynamic-env` 0 of 40. The investigation
and the instrumentation that found it are in "Root cause" at the end.

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

## Root cause (found 2026-09-30)

Established in this order, each step with an instrumented copy of the
emitted C (`TUR_SHOW_CC=1 tur build` gives the cc line; the program TU is
under `/tmp/tur-build/`, rebuilt by hand with the probes below):

1. **A per-fiber "running" flag** (CAS in `tur_fiber_block_resume`) never
   fired: no fiber was ever resumed on two workers at once. **A hidden
   registry of fiber stacks** checked in `tur_gc_release_large` fired in
   3-10 of 60 runs: the collector released the stack of a fiber with
   `done=0`, not running, whose `FiberBlock` was **unmarked** (in some runs
   already freed by an earlier sweep). So the collector was freeing a live
   fiber, not a scheduler race.
2. **The scheduler queue was marked and scanned** (`s` and `s->queue` both
   marked) and the victim was **not in it** -- and not in any worker's
   spilled registers, signal-frame registers, scanned stack, or thread-local
   roots. A snapshot taken at the end of `tur_gc_stop_world` matched the
   state at the sweep exactly: the collector saw what it scanned; the
   pointer was genuinely in none of it.
3. **The victim had never run**: `enq=0 deq=0` and a zero peak stack depth
   for most victims. A fiber main had CREATED (`tur_fiber_block_new`) but not
   yet ENQUEUED. The disassembly of the spawn loop (`input____fn_65`):

   ```
   call tur_fiber_block_new
   mov  %rax,%rbp                 ; the new block lives in rbp
   call tur_gc_mutex_lock.isra.0  ; contended -> park: setjmp(t->regs)
   mov  %rbp,%rsi
   call tur_scheduler_mt_enqueue_locked
   ```

   and glibc's `__sigsetjmp` (`objdump -d libc.so.6`):

   ```
   mov    %rbx,(%rdi)
   mov    %rbp,%rax
   xor    %fs:0x30,%rax           ; PTR_MANGLE
   rol    $0x11,%rax
   mov    %rax,0x8(%rdi)
   mov    %r12,0x10(%rdi) ...     ; r12-r15 plain
   ```

   The lock wrapper saves only `rbx` before it parks, so the caller's
   `rbp` stays in `rbp`, and the word the scan reads for it is the mangled
   one. `rsp` and `rip` are mangled too, which never mattered; `rbx` and
   `r12`-`r15` are plain, which is why the workers' fibers (kept in
   `r13`/`r15`) always survived and only main's spawn window lost one.
4. Not the prelude split (`TUR_PRELUDE_SPLIT=0` fails at the same rate), not
   the fiber stack size (4 MiB stacks fail the same), not the DK driver or
   escape set (the "continuation invoked after its call/cc prompt returned"
   abort is the freed block's `esc_live` read back after reuse).

Why it looked like a fiber/thread bug: the freed block's slot is reused by
the next allocations, so the fiber that main enqueues resumes into garbage
-- a `ucontext` of whatever landed there -- which is the `swapcontext`
fault, the `rip=0` return, the smashed canary, and the garbage Scheme
values (`+: not a number`, `not a procedure`). It needs a torture
collection to land in the few instructions main spends parked in `spawn`,
hence the intermittency, and it needs x86-64 glibc: aarch64's `setjmp`
mangles only `lr` and `sp` (x29 is plain), so the macOS leg's failures on
this pair were the ctest hang, which this does not explain and which stays
with [macos-jit-leg-stall-unexplained](../reported/macos-jit-leg-stall-unexplained.md).

The fix reads `rbp` with one `movq` (x86-64, GCC/clang) into a field next
to each `jmp_buf` (`regs_fp`, `sig_fp`, and a local in `tur_gc_mark_roots`)
and marks it. A c2mir front end (`tur jit` without the split runtime) takes
the `(out) = 0` arm and keeps the old behaviour there. The same change makes
`tur_gc_scan_stack` keep the scan from `sp` when a thread is stopped on its
own stack below `os_sp` (the instants around a resume's `swapcontext`), so
the kernel's signal frame is read there too -- a smaller hole of the same
family, found reading the scan, not measured to bite.
