---
status: resolved
severity: medium
discovered: 2026-08-01
resolved: 2026-08-01
area: test assertions x one-process JIT (was filed as: JIT engine x GC / Rc / weak refs, macOS arm64)
---

# Six GC / Rc / weak-reference fixtures fail under the JIT engine on macOS only

**RESOLVED 2026-08-01.** Not a GC bug, not an `Rc`/weak bug, not a JIT codegen
bug, and not arm64-specific. All six asserted on a **process-wide live-heap byte
count**, which equals the program's heap only when the program owns its process.
Under one-process `tur jit` it measures the compiler's heap instead -- and on the
sanitized Debug build CI's JIT job uses, ASan's quarantine inflates it further.

All six now assert on the **CG6 collector counter** (`gc-live-blocks`) instead.
They run under the JIT, they run under `cc`, and the assertion got *stronger* in
both. The malloc probe survives as one `cc`-only backstop fixture.

## The failing set (as filed)

With the harness bug in
[`ci-macos-suites-fail-while-linux-passes`](ci-macos-suites-fail-while-linux-passes.md)
fixed, `JIT engine (macos-latest)` went from 407 failures to **6**:

```
jit fixture summary: 2409 passed, 6 failed, 47 skipped
  - gc-auto-collects-without-gc-call
  - gc-collects-strong-cycle
  - gc-live-cycle-survives
  - hkt-fmap-rc-result-droppable
  - hkt-instance-rc-construct-result
  - weak-breaks-parent-child-cycle
```

(PR #753, run 30685603734, job 91330616390, head `03ec0d4a`.) They looked like
one coherent family -- cycle-collecting GC, `Rc`, weak references -- because
those are the features whose tests need a leak assertion. **The shared thing is
the assertion, not the feature under test.**

## Diagnosis

The report asked for the per-fixture failure mode and a test of the sanitizer
axis. Both, measured on arm64 macOS (Apple clang 21, macOS 27), one commit:

| Configuration | `gc-collects-strong-cycle` printed |
| --- | --- |
| `Debug` + `-DTUR_JIT=ON`, sanitizers ON (**CI's config**), JIT path | `800000` |
| `Release` + `-DTUR_JIT=ON`, no sanitizers, JIT path | `0` (correct) |
| `Debug`, sanitizers ON, **`cc`** path | `0` (correct) |

Failure mode is `stdout mismatch` on all six -- no crash, no timeout, no `cc`
fallback involvement. Neither ASan nor the JIT alone reproduces it; it takes
both. That is what pins the mechanism, because the assertion was a process-wide
allocator query (`malloc_zone_statistics` on the default zone / `mallinfo2`)
sampled before and after a workload, asserting the delta is 0.

- On the **`cc` path** the program is its own process, compiled by a plain
  unsanitized `cc`, so the probe measures exactly the program's heap.
- Under **`tur jit`** the program runs *inside* the compiler process and its
  allocations go through the compiler's allocator. An unsanitized build still
  prints `0`, but only because the compiler's heap is near steady-state across
  the loop -- the probe is measuring the wrong thing either way, and the pass is
  luck.
- Under **`tur jit` + ASan**, ASan owns the default malloc zone; its accounting
  includes the quarantine and does not shrink on `free`, so the delta grows with
  the workload.

**The ASan startup deadlock CLAUDE.md documents did not reproduce here** -- a
Debug+JIT build with default `TUR_DEBUG_SANITIZE` runs `tur --version` fine on
this toolchain. That hypothesis was available and is not the cause.

### Why it was macOS-only

Exactly the asymmetry the fixtures' own comments described. On Darwin ASan
registers a malloc zone, so the probe reads ASan's numbers. On Linux the probe
is glibc's `mallinfo2()`, and under ASan glibc's allocator is barely used, so it
returns a near-constant and the delta is 0. **The Linux JIT leg was passing
vacuously**, exactly as the fixtures warn their `#else return 0` branch does on
any other libc. Nothing about arm64, MAP_JIT/W^X, or the AAPCS64 `__uint128_t`
skew is involved.

This also explains the local-vs-CI gap the report flagged as genuinely real: a
developer Mac commonly builds `-DTUR_DEBUG_SANITIZE=OFF` (or Release) to dodge
the startup deadlock, and CI does not. The hand-measured
`2414 passed, 0 failed, 47 skipped` baseline in `.github/workflows/ci.yml:195-212`
was taken on an unsanitized build.

## This is the third instance of a known family -- and the repo already had a policy

[`gc-leak-gate-darwin-sanitized-probe-drift`](history/gc-leak-gate-darwin-sanitized-probe-drift.md)
(resolved 2026-07-29) measured **the same `800000`** on Darwin under ASan and
diagnosed the same probe-vs-allocator mismatch. Two earlier archived Darwin heap
reports are the same family again. That report chose fix direction (1) --
`tests/run-gc-leak-gate.sh` stops asserting on probe output -- and explicitly
**rejected** direction (2), making the probe vacuous under ASan, because "it
keeps a check that measures nothing looking like a check that passed."

So the blindness modes were, cumulatively:

| Context | What the probe reports |
| --- | --- |
| glibc + ASan | `0` on both sides -- vacuous |
| Darwin + ASan | quarantine, not retention (`800000`) |
| any `tur jit` | the **compiler's** heap |
| any other libc | hardcoded `0` -- vacuous |

Four ways to be wrong and one narrow way to be right (unsanitized `cc` on glibc
or Darwin). That earlier report also named the alternative it trusted, in
passing: fixtures "whose expected output comes from a malloc probe **rather than
the CG6 counters**." This change simply takes that alternative.

## Fix

The CG6 counters (`gc-collections`, `gc-objects-freed`, `gc-live-blocks`,
`gc-candidate-high-water`, `elab_memory.c:638-684`) read runtime state
(`gc_all_blocks_count`, `gc.c:329`) that has none of those failure modes:

- **Program-scoped.** Nothing in the compiler or the interpreter allocates rc
  blocks -- `rc_cb_alloc*` has no callers outside the runtime and emitted code
  -- so even under `tur jit`, where the counter is host-resident, it reflects
  only the JIT'd program.
- **Identical in every linkage mode.** `g_rcgc_from_archive` decides whether the
  preamble carries its own rc/GC replica or links the real `src/runtime/`; both
  `tur build` and `tur jit` take archive mode, and under JIT the symbols resolve
  via `dlsym(RTLD_DEFAULT)` into the host's compiled `gc.c`. The pre-existing
  `gc-stats-observability` and `gc-registry-growth` fixtures already produce
  byte-identical stdout under both paths.
- **Exact, not tolerance-based**, and ASan cannot perturb it.
- **Portable** -- no `#else return 0` arm.
- **Tracks refcount-only frees too**, not just collections: `rc_cb_free`
  (`rc.c:239`) unregisters, so the three non-collector fixtures work unchanged.

Every value these six fixtures leak-or-free is an `rc<T>` control block, so
`gc-live-blocks` measures exactly what each was trying to measure.

### The assertions got stronger, not weaker

Verified by injecting the defect each fixture pins, on both paths:

| Fixture | Injected defect | cc | jit + ASan | tolerance |
| --- | --- | --- | --- | --- |
| `gc-collects-strong-cycle` | `(gc-disable!)` | `10000` | `10000` | `== 0` |
| `gc-auto-collects-without-gc-call` | `(gc-disable!)` | `40000` | `40000` | `< 512` |

`10000` is both blocks of all 5000 pairs; `40000` likewise for 20000. Under the
old probe the same control was *impossible* on glibc (identical output either
way) and was quarantine noise on sanitized Darwin.

`weak-breaks-parent-child-cycle` resists its injection at the type level -- the
back-edge field is `weak<T>`, so strengthening it is a compile error, which is
the stronger guarantee.

The AUTO tolerance moved from `< 65536` bytes to `< 512` blocks against a
measured residual of exactly **64**, identical on both paths. The old byte
tolerance was ~682 blocks' worth, so this is tighter as well as deterministic.

### What was preserved

Bytes can still catch a leak that does **not** leak a control block (a payload,
or non-rc scaffolding), which the block counter cannot see. So the malloc probe
survives verbatim in one new fixture, `gc-collects-strong-cycle-heap-bytes`,
carrying `requires.cc` -- the marker's existing users are the `--dump-*`
compile-phase fixtures, whose rationale is the same shape ("measures something
that belongs to a separate process"). Its `requires.cc` file records the
measurement.

Six probe copies collapsed to one; the other five gained nothing from bytes that
blocks do not give them.

### `tests/run-gc-leak-gate.sh` got stronger too

`gc-collects-strong-cycle` no longer produces probe output, so it moved from
`PROBE_OUTPUT_FIXTURES` to `CONTROL_FIXTURES`: **both of its skips became real
assertions**, because the sanitized numbers now discriminate (0 on, 10000 off).
The `-heap-bytes` sibling inherits the two skips.

```
before: gc-leak-gate: 14 passed, 2 skipped, 0 failed
after:  gc-leak-gate: 18 passed, 2 skipped, 0 failed
```

## Verification (arm64 macOS, Apple clang 21, macOS 27)

| Check | Result |
| --- | --- |
| `tests/run.sh` | `2501 passed, 0 failed` |
| `tests/run-jit.sh`, Debug+JIT+ASan (**CI's exact config**) | `2415 passed, 0 failed, 48 skipped` |
| `tests/run-gc-leak-gate.sh` | `18 passed, 2 skipped, 0 failed` |
| `regen-snapshots --check` | `140 up to date` -- zero drift |
| All six, `cc` vs sanitized JIT | byte-identical stdout, both matching `expected.stdout` |

The six are back in the JIT corpus rather than skipped out of it. The one new
skip (47 -> 48) is the `-heap-bytes` backstop.

### Bonus: the six now JIT natively instead of via the `cc` fallback

Two archived JIT reports
([SDK headers](history/jit-macos-apple-sdk-headers-force-cc-fallback.md),
[full-corpus extension](history/jit-macos-full-corpus-extension-and-atexit.md))
record these same five fixtures hitting
`#error TargetConditionals.h: unknown compiler` and being pushed onto the
engine's `cc` fallback. The cause was the probe's `#include <malloc/malloc.h>`,
which drags in `TargetConditionals.h`. With the probe gone they compile through
MIR directly -- verified: no TUR-W0070 on `gc-collects-strong-cycle`,
`gc-live-cycle-survives`, or `weak-breaks-parent-child-cycle`. The
`-heap-bytes` fixture is now the only fixture in the corpus reaching that
header, and it is `cc`-only anyway.

So this change also converts five fixtures from "compiled by `cc` and merely
run under the JIT harness" to actually exercising the JIT engine -- coverage the
JIT leg was not previously getting from them at all.

## Note for `.github/workflows/ci.yml:210-212`

It asks that the **Linux** JIT leg be flipped to blocking once it publishes a
clean run. It has (run 30685603734). That is a gating-policy change, not part of
this finding -- but weigh it against what this report showed: the Linux leg's
green was *partly vacuous on exactly this family*, because `mallinfo2` under ASan
measures nothing. After this change it is not, which makes the flip more
worthwhile than before, not less.

## Related

- [`ci-macos-suites-fail-while-linux-passes`](ci-macos-suites-fail-while-linux-passes.md)
  -- the harness bug that hid these. Closed.
- [`macos-int-conversion-carrier-pointer-straddles`](macos-int-conversion-carrier-pointer-straddles.md)
  -- the AOT half of the same CI redness. Closed.
- [`gc-leak-gate-darwin-sanitized-probe-drift`](history/gc-leak-gate-darwin-sanitized-probe-drift.md)
  -- the same probe-vs-allocator family, one instance earlier. Its fix direction
  (1) is what this change generalizes.
