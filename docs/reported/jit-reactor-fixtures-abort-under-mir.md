# `reactor-*` fixtures abort under the MIR JIT (work under `cc`)

**Severity: medium, and unexplained.** 10 fixtures. Not a regression --
they previously failed at link and now fail at runtime, which is a worse
failure mode but more honest information.

## Summary

Every `reactor-*` fixture aborts with `free(): invalid pointer` and produces no
stdout when run through the J0 JIT harness. All of them pass on the `cc` path
(`bash tests/run.sh` is green at 2399/0, and it runs them).

```
reactor-fd-modify    reactor-fd-readable   reactor-fd-remove    reactor-fd-writable
reactor-fibers-cancel-on-free              reactor-fibers-park-chan
reactor-fibers-park-fd                     reactor-fibers-smoke
reactor-fibers-stop-mid-run                (+ defstruct-field-session-role variants)
```

## Repro

```sh
./build/tur emit-c tests/fixtures/reactor-fibers-smoke/input.tur > /tmp/w.c
python3 tools/jit-spike/normalize-c11-subset.py /tmp/w.c -o /tmp/w.s.c
build-jit/tools/jit-spike/tur-jit-spike -I src -I src/runtime -O 2 \
    --shim tools/jit-spike/subset-shim.h /tmp/w.s.c
# free(): invalid pointer   -- SIGABRT, no stdout
```

Expected `fiber ran` / `completed=1`.

## What has been ruled out

- **Not optimization-dependent.** Identical abort at `-O0`, `-O1`, `-O2`.
- **Not lazy generation.** The sweep runs eager.
- **Not a missing symbol.** Before `7b97d4036` these failed with
  `unresolved import: tur_reactor_new`; linking the runtime fixed the link and
  exposed this.
- **Not a partial-runtime mismatch.** This was the first hypothesis, recorded
  in findings 11.5, and it is **wrong**. The harness was switched from a
  curated 16-TU list to a `--whole-archive` link of `libturi` -- the same
  archive `tur build` autolinks via `-lturi` -- and the abort is unchanged.
  11.5 has been corrected.

## What is established

The emitted preamble carries **its own** fiber runtime, `static` and private to
the module:

```c
static void tur_fiber_shim(uint32_t hi, uint32_t lo) { ... }
static FiberBlock *tur_fiber_block_new(void (*fn)(void), size_t stack_size) {
    ...
    makecontext(&f->ctx, (void(*)(void))tur_fiber_shim, 2, _hi, _lo);
}
static int64_t tur_fiber_block_resume(FiberBlock *f, int64_t arg) { ... }
```

while `tur_reactor_*` resolves to the host. That split is identical on the `cc`
path, where it works -- so the split itself is not the fault.

The distinguishing feature of the JIT path is that `tur_fiber_shim` is
**JIT-generated code used as a `makecontext` entry point**, and the fiber then
runs JIT'd code on a `malloc`'d stack, switching via `swapcontext`. That is the
first thing to examine: whether MIR-generated code is safe as a ucontext entry,
and whether anything in MIR's own state (code-allocation bookkeeping, the
lazy-generation trampoline) is disturbed by a stack switch it does not know
about.

`free(): invalid pointer` is consistent with a context switch that does not
restore the frame the caller expects, but that is inference, not evidence.

## Next steps

Run one fixture under a debugger and get the abort's backtrace -- specifically
whether the bad `free` is the fiber stack, the `FiberBlock`, or something in
MIR's allocator. That single data point likely settles it, and none of the
above speculation should be treated as a conclusion until it exists.

If it turns out MIR-generated code cannot serve as a `makecontext` entry, the
options are: keep the fiber runtime in the host (extend the S2 boundary so the
preamble stops emitting its own copy), or have `tur jit` fall back to `cc` for
programs that use fibers.

## Provenance

docs/upcoming/jit-engine-j0-findings.md section 11.5, during the
recommendation-8 host-symbol-boundary work. Filed rather than fixed because the
one hypothesis that was cheap to test has been tested and disproved; the next
step needs a debugger session rather than another guess.
