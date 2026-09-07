# `run-jit.sh` reports PASS when the jit engine is disabled tree-wide

**Severity: medium (no wrong answers -- but the suite that exists to test the
jit engine can go from "engine compiles 2700 programs" to "engine compiles
zero" and still print a green summary and exit 0).** Filed 2026-09-07.

## What happened

A single GNU-only construct in the emitter -- `__auto_type`, added for the
region erasure note -- made **c2mir reject every translation unit that
contained an erasing ascription**. Because the stdlib prelude has one
(`__inst_Eq_eq_qu_Cons`), that was effectively every program in the tree.

Each one hit `TUR-W0070`, fell back to the cc path, produced correct output,
and `run-jit.sh` counted it:

```sh
PASS_FALLBACK) PASS=$((PASS + 1)); FALLBACK=$((FALLBACK + 1)) ;;
```

The summary then prints the fallback count and asserts nothing about it:

```sh
echo "jit fixture summary: $PASS passed, $FAIL failed, $SKIP skipped"
if [ "$FALLBACK" -gt 0 ]; then
    echo "  (of which $FALLBACK passed via the cc fallback -- TUR-W0070)"
fi
```

So the engine was off for the whole suite and the suite said 0 failed. The
only reason CI noticed at all is that the **fallback** then failed for 10
fixtures on macOS for an unrelated reason (`over_px__lens_*` monomorph
specializations called but never defined on the re-entered `cmd_run` path),
which surfaced as `stdout mismatch (via cc fallback)` -- a symptom three steps
removed from the cause, on one platform, in one job.

## Repro

Any emitter change that c2mir cannot parse. The one that produced this:

```c
buf_printf(body, "__auto_type %s = (%s);\n", et, inner_val);
```

Then:

```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON -DTUR_DEBUG_SANITIZE=OFF
cmake --build build-jit -j
TUR=./build-jit/tur bash tests/run-jit.sh
# jit fixture summary: NNNN passed, 0 failed, NN skipped
#   (of which NNNN passed via the cc fallback -- TUR-W0070)
```

The second line is the whole signal, and nothing reads it.

## Why the comment in `run-jit.sh` does not cover this

The header says the fallback is "a signal, never a failure", and per fixture
that is right -- a fixture the engine cannot yet compile should not be a red
test, and stamping it would stop a future engine improvement from reclaiming
it. The gap is that there is no **aggregate** assertion: nothing distinguishes
"a known handful still fall back" from "the engine is off".

## Fix direction

A ratchet on the fallback count, the way `tests/repr-decision-baseline.txt`
ratchets re-derived representation decisions: store the expected count (or the
expected fallback fixture NAMES, which is stricter and self-documenting) and
fail when it grows. Names are probably better than a number here -- the count
moves whenever fixtures are added, so a bare number will rot into a
rubber-stamp, while a name list makes each new fallback a deliberate one-line
edit with a reviewer looking at it.

Worth pairing with a cheap smoke assertion that does not depend on the
baseline at all: compile ONE trivial program through the engine and fail if it
falls back. That catches the tree-wide case (which is the expensive one) in a
second, without any list to maintain.

## A second thing the fallback was hiding: the suite's answer depends on `TUR_DEBUG_SANITIZE`

With the engine restored, `gc-heap-struct-rc` fails under a JIT build
configured `-DTUR_DEBUG_SANITIZE=OFF` and passes under the CI configuration
(Debug + JIT, sanitizers left ON, which is what `.github/workflows/ci.yml`'s
JIT job uses):

```sh
# -DTUR_DEBUG_SANITIZE=OFF
./build-jit/tur jit tests/fixtures/gc-heap-struct-rc/input.tur
# 0
# 1376          <- expected 0; nothing was reclaimed

# CI's configuration (sanitizers on)
ASAN_OPTIONS=detect_leaks=0 ./build-jit-san/tur jit tests/fixtures/gc-heap-struct-rc/input.tur
# 0
# 0
```

**This is on `main`** (e89594fb gives `0 / 1376` on the nosan build, natively,
no fallback), so it is not a regression -- but it means a GC fixture's answer
turns on a build flag that has no business changing it. The JIT runs the
program IN PROCESS, so the program shares `tur`'s allocator and runtime, and
the sanitizer's presence changes what the collector sees as live. Either the
fixture is measuring something that is not well-defined under an in-process
engine, or the collector's root scan is sensitive to the host build -- worth
knowing which before trusting any in-process GC number.

Practical consequence for anyone reproducing a JIT failure locally: **build
the JIT tree the way CI does.** The `-DTUR_DEBUG_SANITIZE=OFF` escape hatch
that CLAUDE.md offers for the macOS ASan startup deadlock silently changes
results here.

## Related, and not the same bug

The macOS cc-fallback link failure this hid -- `over_px__lens_*` /
`set_px__lens_*` monomorph specializations called but never emitted when
`cmd_jit` falls back into `cmd_run` after an engine attempt -- is a separate
defect and is still open. It reproduces on Linux too:

```sh
./build-jit/tur jit tests/fixtures/van-laarhoven-lens-wide-mono/input.tur
# undefined reference to `over_px__lens_89e16e7f8669ca4e'
```

but ONLY when the engine attempt fails first; `tur run` on the same file emits
no such call and works. So the fallback re-entry appears to inherit monomorph
state from the abandoned attempt. With `__auto_type` gone the engine no longer
falls back on these fixtures, so the path is not currently exercised -- which
is exactly the condition under which it will be rediscovered the hard way.
