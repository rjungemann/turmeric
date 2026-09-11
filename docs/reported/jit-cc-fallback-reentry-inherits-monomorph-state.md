# `tur jit`'s cc fallback re-enters `cmd_run` with the abandoned engine attempt's monomorph state

**Status update 2026-09-10: finding 1 is FIXED. Finding 2 is untouched and
still open, so this report stays.**

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
