# `tur jit`'s cc fallback re-enters `cmd_run` with the abandoned engine attempt's monomorph state

**RESOLVED 2026-09-26 -- both findings.** Finding 1 was fixed 2026-09-10
(below). Finding 2 turned out to be a measurement artifact of the fixture, not
a collector difference, and the fixture is now build-independent:

- **Unsanitized JIT (the failure).** The delta is the engine's own one-off
  allocation, not a leak: it reads the same 1344 / 688 bytes at 5000 and at
  50000 iterations, where a leak scales with the count. The in-process engine
  generates code on a path's first execution, and the first measurement window
  was the first execution of the tail of `heap-bytes` after its probe and of
  the subtraction. The fixture now runs one throwaway window before the two
  real ones, and an unsanitized `tur jit` reads `0 0`, like `tur run`.
- **Sanitized JIT (CI, which passed).** `mallinfo2` cannot see the ASan
  allocator at all -- a plain C probe reads 0 for a 100 KB `malloc` under
  `-fsanitize=address` and 100672 without -- so in-process under a sanitized
  `tur` this probe is vacuous whatever the program does. The compiled leg
  (`tur run`, an unsanitized binary) is where the fixture's leak check is
  real, and it stays so.
- **Still detects a leak.** A negative control -- a 16-byte `malloc` per
  `acyclic` call -- reads ~160 KB in the first window on both `tur run` and the
  unsanitized `tur jit`.

The same session's JIT build also ran the rest of the corpus: 3142 passed,
this fixture the one failure. (It surfaced a separate regression -- a literal
`__atomic_store_n` in the threaded-async preamble that sent every `tur jit`
program to cc -- fixed in the same change set and guarded by
`tests/check-no-atomic-builtins.sh`.)

**Status update 2026-09-10: finding 1 is FIXED. Finding 2 was untouched and
still open, so this report stayed.**

Reproduced first, which needed a forcing route: the lens fixtures no longer
fall back on their own, so `TUR_JIT_FORCE_FALLBACK=1` was added to `cmd_jit`
to decline the engine deliberately. It declines BEFORE `tur_jit_execute`
rather than after, so the program is not run twice -- the real shape is a
front end that has already elaborated handing over to a cc compile that never
executed anything. With it, the reported symbol reproduces exactly:

```
error: call to undeclared function 'over_px__lens_89e16e7f8669ca4e'
```

The carried state is `mono_specs.c`'s file-scope registry: `cmd_jit` falls
back by calling `cmd_run` IN THE SAME PROCESS, so the re-entered compile
elaborates against a registry that already believes those specs were
requested, and emits calls to definitions nothing then emits.

`mono_specs_reset()` has existed since the registry was added and was never
called from anywhere -- this is the call site it was written for. One line,
before the `cmd_run` re-entry.

Pinned by `jit-cc-fallback-reentry-monomorph-state` in `tests/run-flags.sh`,
verified as a real negative control: with the reset removed the assertion
fails on the reported symbol, and passes with it restored.

Worth recording for anyone forcing a fallback by hand: a fixture whose output
IS the registry dump (`--dump-mono-specs`, e.g.
`van-laarhoven-lens-wide-mono-resolve`) prints that dump TWICE under a forced
fallback -- once from the engine attempt's front end and once from the
re-entry. That is inherent to running the front end twice, not a consequence
of the reset; the second dump is complete, which is the reset working.

**Severity: low today, latent.** Split out 2026-09-09 from
[jit-suite-reports-pass-when-the-engine-is-disabled](../archive/jit-suite-reports-pass-when-the-engine-is-disabled.md)
when that report's suite-accounting half was fixed (engine smoke check +
by-name fallback ratchet in `tests/run-jit.sh`). These are the two findings
that report recorded as separate and still open; nothing here was
re-verified on the day of the split.

## 1. Fallback re-entry: monomorph specializations called but never emitted

When `cmd_jit`'s engine attempt fails and it falls back into `cmd_run`, the
re-entered compile can reference `over_px__lens_*` / `set_px__lens_*`
monomorph specializations that are never defined:

```sh
./build-jit/tur jit tests/fixtures/van-laarhoven-lens-wide-mono/input.tur
# undefined reference to `over_px__lens_89e16e7f8669ca4e'
```

...but ONLY when the engine attempt fails first. `tur run` on the same file
emits no such call and works, so the fallback path appears to inherit
monomorph state from the abandoned attempt. Reproduced on Linux and macOS
(the macOS CI leg is where it surfaced, as `stdout mismatch (via cc
fallback)` on 10 fixtures).

With the `__auto_type` construct gone from the emitter the engine no longer
falls back on these fixtures, so the path is not currently exercised --
which is exactly the condition under which it will be rediscovered the hard
way. To reproduce, force a fallback (e.g. temporarily emit a construct c2mir
rejects) and run the lens fixtures through `tur jit`.

**Fix direction:** reset (or snapshot/restore) the monomorph-request /
emitted-spec registries between the engine attempt and the `cmd_run`
re-entry, or run the fallback compile in a fresh `Env`. Then pin it with a
fixture that forces the fallback deliberately.

## 2. `gc-heap-struct-rc` under `tur jit` answers differently by build flag

With the engine active, `gc-heap-struct-rc` fails under a JIT build
configured `-DTUR_DEBUG_SANITIZE=OFF` and passes under CI's configuration
(Debug + JIT, sanitizers ON):

```sh
./build-jit/tur jit tests/fixtures/gc-heap-struct-rc/input.tur       # nosan
# 0
# 1376          <- expected 0; nothing was reclaimed
ASAN_OPTIONS=detect_leaks=0 ./build-jit-san/tur jit .../input.tur    # CI config
# 0
# 0
```

On `main` at e89594fb, natively, no fallback. The JIT runs the program IN
PROCESS, so it shares `tur`'s allocator and runtime, and the sanitizer's
presence changes what the collector sees as live. Either the fixture measures
something not well-defined under an in-process engine, or the collector's
root scan is sensitive to the host build. Worth knowing which before trusting
any in-process GC number -- and anyone reproducing a JIT failure locally
should **build the JIT tree the way CI does**; the `-DTUR_DEBUG_SANITIZE=OFF`
escape hatch CLAUDE.md offers for the macOS ASan startup deadlock silently
changes this result.
