---
status: resolved
severity: medium
discovered: 2026-08-01
resolved: 2026-08-01
area: test harness x sanitized JIT (was filed as: JIT engine x GC / Rc / weak refs, macOS arm64)
---

# Six GC / Rc / weak-reference fixtures fail under the JIT engine on macOS only

**RESOLVED 2026-08-01.** Not a GC bug, not an `Rc`/weak bug, not a JIT codegen
bug, and not arm64-specific. All six assert on a **process-wide live-heap byte
count**, which measures the program's heap only when the program owns its
process -- i.e. the `cc` path. Under one-process `tur jit` the program shares
the compiler's address space and allocator, and on the sanitized Debug build
CI's JIT job uses, ASan registers itself as the default malloc zone and its
quarantine grows monotonically, so the probe reports a large delta.

All six now carry a `requires.cc` marker (the same marker the `--dump-*`
compile-phase fixtures already use for the same "measures the wrong process
under one-process jit" reason). The `cc` path still runs them, so no coverage
is lost.

## Summary (as filed)

With the harness bug in
[`ci-macos-suites-fail-while-linux-passes`](ci-macos-suites-fail-while-linux-passes.md)
fixed, `JIT engine (macos-latest)` went from 407 failures to **6**:

```
jit fixture summary: 2409 passed, 6 failed, 47 skipped
failed:
  - gc-auto-collects-without-gc-call
  - gc-collects-strong-cycle
  - gc-live-cycle-survives
  - hkt-fmap-rc-result-droppable
  - hkt-instance-rc-construct-result
  - weak-breaks-parent-child-cycle
```

(PR #753, run 30685603734, job 91330616390, head `03ec0d4a`.) They looked like
one coherent family -- cycle-collecting GC, `Rc`, and weak references -- because
those are the features whose tests need a live-heap assertion. The shared
feature is the *assertion*, not the feature under test.

## Diagnosis (2026-08-01, arm64 macOS, Apple clang 21, macOS 27)

The report asked for two things first: the per-fixture failure mode, and a test
of the sanitizer axis. Both, measured on one box at one commit:

| Configuration | `gc-collects-strong-cycle` prints |
| --- | --- |
| `Debug` + `-DTUR_JIT=ON` (sanitizers ON -- **CI's config**), JIT path | `800000` |
| `Release` + `-DTUR_JIT=ON` (no sanitizers), JIT path | `0` (correct) |
| `Debug` + sanitizers ON, **`cc`** path | `0` (correct) |

The failure mode is `stdout mismatch` on all six -- no crash, no timeout, no
`cc` fallback involvement.

So it is ASan **x** JIT: neither alone reproduces it. That is what pins the
mechanism, because the assertion is a process-wide allocator query:

```c
(defn heap-uordblks [] : int
  ```c
  #if defined(__APPLE__)
  #include <malloc/malloc.h>
  malloc_statistics_t ms;
  malloc_zone_statistics(malloc_default_zone(), &ms);
  return (int64_t)ms.size_in_use;
  #elif defined(__GLIBC__)
  struct mallinfo2 mi = mallinfo2();
  return (int64_t)mi.uordblks;
  #else
  return 0;
  #endif
  ```)
```

Each fixture reads it before and after a workload and asserts the delta is 0.

- On the **`cc` path** the program is its own process, compiled by a plain
  (unsanitized) `cc`, so the probe measures exactly the program's heap. Correct.
- Under **`tur jit`** the program runs *inside* the compiler process. Its
  allocations go through the compiler's allocator, and so do the compiler's.
  An unsanitized build still prints `0`, but only because the compiler's own
  heap is near steady-state across the loop -- the probe is measuring the wrong
  thing either way, and the pass is luck.
- Under **`tur jit` on a sanitized build**, ASan replaces the default malloc
  zone. `malloc_zone_statistics(malloc_default_zone(), ...)` then reports ASan's
  accounting, which includes the quarantine and does not shrink on `free`. The
  delta grows with the workload, so the assertion fails.

### Why this was macOS-only

The same asymmetry the fixture's own comment describes. On Darwin ASan
registers a malloc zone, so `malloc_zone_statistics(malloc_default_zone())`
picks up ASan's numbers. On Linux the probe is glibc's `mallinfo2()`, which
reports glibc's arenas -- and under ASan glibc's allocator is barely used, so
the probe returns a near-constant and the delta is 0. The Linux JIT leg passes
**vacuously**, exactly as the fixture warns the `#else` branch does. Nothing
about arm64, MAP_JIT/W^X, or the AAPCS64 `__uint128_t` skew is involved.

This also explains the "local passes, CI fails" gap the report flagged as
genuinely real: a developer Mac commonly builds `-DTUR_DEBUG_SANITIZE=OFF` (or
Release) to dodge the ASan startup deadlock CLAUDE.md documents, and CI does
not. The hand-measured `2414 passed, 0 failed, 47 skipped` baseline in
`.github/workflows/ci.yml:195-212` was taken on an unsanitized build.

(For the record: the ASan startup deadlock CLAUDE.md documents did **not**
reproduce on this box -- a Debug+JIT build with default `TUR_DEBUG_SANITIZE`
runs `tur --version` fine on Apple clang 21 / macOS 27.)

## Fix

`requires.cc` on all six. The marker's existing users are the `--dump-*`
compile-phase fixtures, whose rationale is the same shape: output that belongs
to a separate process cannot be observed under one-process `tur jit`. Each
marker file carries the measurement above.

Deliberately *not* fixed by making the probe sanitizer-aware or by loosening the
assertion: the probe is correct on the path it was written for, and a JIT-mode
version of it would have to measure the program's allocations separately from
the compiler's -- which is a real feature (per-program heap accounting), not a
test tweak. If that ever exists, drop the markers.

### Result, in CI's exact configuration

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON   # sanitizers ON
TUR=./build/tur bash tests/run-jit.sh
  -> jit fixture summary: 2409 passed, 0 failed, 53 skipped   (exit 0)
```

and the `cc` path still covers all six:

```
bash tests/run.sh -> 2500 passed, 0 failed
  PASS gc-auto-collects-without-gc-call
  PASS gc-collects-strong-cycle
  PASS gc-live-cycle-survives
  PASS hkt-fmap-rc-result-droppable
  PASS hkt-instance-rc-construct-result
  PASS weak-breaks-parent-child-cycle
```

Note for whoever next reads `.github/workflows/ci.yml:195-212`: the JIT leg's
documented baseline of `2414 passed / 47 skipped` is now `2409 passed / 53
skipped`. Same corpus, six fixtures moved from run-under-jit to `cc`-only.

## Still open, and unrelated to this finding

`.github/workflows/ci.yml:210-212` asks that the **Linux** JIT leg be flipped
to blocking once it publishes a clean run. It has (run 30685603734). Doing so is
a gating-policy change, not part of this finding, but the precondition is met
and the file asks not to let the asymmetry sit indefinitely.

Weigh it against what this report just showed, though: the Linux leg's green is
partly vacuous on exactly this family, because `mallinfo2` under ASan measures
nothing. Blocking on it is still worth more than not blocking on it.

## Related

- [`ci-macos-suites-fail-while-linux-passes`](ci-macos-suites-fail-while-linux-passes.md)
  -- the harness bug that hid these. Closed.
- [`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md)
  -- the AOT half of the same CI redness. Closed.
