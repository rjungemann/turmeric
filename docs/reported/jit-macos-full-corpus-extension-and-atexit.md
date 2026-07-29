# JIT macOS full-corpus run: `__extension__` codegen gap, and `atexit` that resolves and then crashes

**Severity:** medium (JIT spike / J1 planning only -- nothing here affects
`tur build` or `tur emit-c` output correctness for the normal `cc` path).

Measured 2026-07-28 on Apple Silicon (arm64, Darwin 27.0.0, AppleClang 21.0.0),
MIR at the `a8ab7c31` pin, branch `claude/j0-jit-engine-plan-znqibo` at
`d657707dc`. This is the full-corpus macOS run that
`docs/upcoming/jit-engine-j0-findings.md` section 8.4.2 asks for and leaves
open.

## Summary -- the run does not land where 8.4.2 predicts

| Run | Pass | Rate |
|---|---|---|
| Linux, eager (8.4.2, reported) | 1424 / 1680 | 84.8% |
| **macOS, eager, artifacts as committed** | **1373 / 1680** | **81.7%** |
| macOS, eager, + one shim line (below) | 1410 / 1680 | 83.9% |

Section 8.4.2 concludes "a full-corpus macOS run would be expected near 85%
too" and that "the gap needs no platform explanation and there is no evidence
for one." The full run measures **81.7%** -- a 51-fixture platform delta.

The sampling analysis in 8.4.2 is nonetheless sound and should be kept: the
macOS full run reproduces the stride effect independently (stride-10 spread
78.6% - 88.7%, 10.1 points), so stride-on-an-alphabetical-corpus really is the
dominant error term. The correct reading is narrower than the one recorded:
**sampling explained the 89-vs-78 gap while masking a genuine platform
difference underneath it.** Of the 51 fixtures, 37 are a single token in our
own codegen (finding 1), 9 are Apple SDK headers (finding 3), and the rest is
classification drift.

## Finding 1 -- `__extension__` is emitted unconditionally and c2mir rejects it

This is a **host-independent defect in Turmeric's codegen** that glibc happens
to conceal. It is the single highest-value item in this document.

### Repro

```c
int f(int x){ return x + 1; }
int a = __extension__ ({ f(1); f(2); });   /* c2mir: syntax error */
int b =                ({ f(1); f(2); });  /* c2mir: fine, evaluates to 3 */
```

c2mir supports GNU statement expressions. It has no `__extension__` keyword.

### Root cause

`__extension__ ({ ... })` is emitted with no platform guard from:

- `src/compiler/elab_sessions.c:298`, `:516`, `:571` (session send / send-tag)
- `src/compiler/elab_global.c:609` (router send)
- `src/compiler/emit_expr.c:508`, `:3117`, `:3166`, `:3177`, `:3184`
  (pointer boxing, tagged-union construction)

`tools/jit-spike/normalize-c11-subset.py` does not strip it and
`tools/jit-spike/subset-shim.h` does not define it away.

### Why Linux never saw it

glibc's `<sys/cdefs.h>` defines `__extension__` to nothing when `__GNUC__` is
undefined, which is exactly c2mir's situation -- so on Linux any TU that
includes a libc header silently loses the token before c2mir parses it. Apple's
`<sys/cdefs.h>` has no such fallback (verified: the token does not appear in
the SDK header at all), so it survives to the parser.

This also means section 8.4.4's claim that the `session-*` fixtures pass under
eager on Linux is true only by accident of glibc's headers, not because the
emitted C is inside c2mir's subset.

### Blast radius, measured

Adding a single line to the shim:

```c
#define __extension__
```

takes the corpus from **1373 to 1410 (+37 fixtures, +2.2 points)** and removes
three whole failure classes:

| Class | as committed | with the define |
|---|---|---|
| `syntax error on void` (all 25 are `session-*` / `defstruct-field-session-*`) | 25 | 0 |
| `syntax error on {` | 6 | 0 |
| `syntax error on identifier` (`__auto_type` residue) | 199 | 193 |

Two counts then match Linux's full-corpus figures **exactly** -- `__auto_type`
residue 193 = 193, and GNU constructs in user inline-C 31 = 31. That agreement
is the best evidence available that the two corpora are otherwise measuring the
same thing.

### Fix direction

Fix the emitters, not the shim. `({ ... })` on its own is accepted by gcc,
clang, and c2mir; the `__extension__` prefix suppresses a `-pedantic` warning
we do not enable and costs the JIT 37 fixtures. Dropping it from the nine sites
above is a mechanical change that regenerates fixture snapshots.

## Finding 2 -- resolving `atexit` is not the fix; it converts a clean error into a crash

Section 8.4.3 files `unresolved import: atexit` (3 fixtures, all
`module-defer-*`) as S4 work whose fix is "register it explicitly via
`MIR_load_external`, exactly as c2m already does for `abort`."

macOS is the natural experiment for that recommendation, because `atexit`
**does** resolve there. Verified directly from the spike harness's own linkage:

```
atexit           FOUND      __cxa_atexit     FOUND
printf           FOUND      exit             FOUND
malloc           FOUND      pthread_create   FOUND
abort            FOUND
```

Resolving it is necessary but not sufficient. All three `module-defer-*`
fixtures get further and then die:

```
$ tur-jit-spike -O 2 --eager --shim ... module-defer-basic.subset.c
hello
rc=139            # SIGSEGV;  expected stdout is "hello\ngoodbye"
```

The emitted C does `atexit(__module_defer_0)` (`module-defer-basic`, emitted
line 7505). `__module_defer_0` is JIT'd code, so the registered function
pointer points into MIR-owned memory that is torn down before libc drains its
atexit list at process exit.

**Consequence for J1:** landing the recommendation as written would turn
Linux's diagnosable `unresolved import: atexit` into macOS's silent
crash-at-exit. The real requirement is that the JIT **intercept** `atexit`,
keep its own deferred-handler list, and drain it before finalizing the MIR
context. That is materially more than one `MIR_load_external` row and the S4
sizing should say so.

## Finding 3 -- the predicted Apple SDK residue is real, and is 9 fixtures

Section 8.4.2's residual open question ("a real but second-order excess of
roughly 4-9 fixtures, and the natural suspect is c2mir on Apple SDK headers")
is **confirmed, and lands inside the predicted range** -- but only once
finding 1 is removed. Before that it is buried under the 37.

| Class | Count | Header | Fixtures |
|---|---|---|---|
| `#error TargetConditionals.h: unknown compiler` | 5 | `TargetConditionals.h:398` | `gc-collects-strong-cycle`, `gc-live-cycle-survives`, `hkt-fmap-rc-result-droppable`, `hkt-instance-rc-construct-result`, `weak-breaks-parent-child-cycle` |
| `empty preprocessor expression` | 3 | `mach/port.h:100`, reached via `sys/socket.h` | `image-hooks-tracked`, `image-reload-hook`, `image-roundtrip` |
| `unresolved import: _OSSwapInt16` | 1 | `libkern/OSByteOrder.h:314` (`#error Unknown endianess`) | `async-echo-server` |

Single root cause: c2mir advertises neither `__GNUC__` nor `__clang__` and
implements no `__is_target_arch` / `__has_builtin`, so Apple's compiler-
detection cascade falls through to its error branch. `sys/cdefs.h:81` also
emits `#warning "Unsupported compiler detected"` on every macOS TU.

Cheap to fix in the shim with a few predefines (`__clang__`, `__GNUC__`,
`TARGET_CPU_ARM64=1`, `TARGET_OS_MAC=1`, endianness). Cosmetic next to
findings 1 and 2.

## Incidental corrections to section 8

- **`__atomic_thread_fence` and `__atomic_exchange_n` are reached.** Section
  8.4 says the two atomics the shim omits relative to the original prologue are
  "not reached by any sampled fixture." At full-corpus scale each is reached by
  exactly one fixture. A sample artifact, same family as the rest of 8.4.2.
- **Runtime-failure set agrees with Linux.** With finding 1 applied the macOS
  non-parse failures are `dynvar-log-level`, `dynvar-nested`,
  `dynvar-thread-locale`, `self-recursive-carrier-struct-return`,
  `taskgroup-async` (mismatch); `gc-registry-growth`,
  `set-multiword-struct-element` (SIGSEGV); `any-cast-mismatch-panic`
  (SIGABRT, and is supposed to panic -- 8.4.3's caveat applies here too). The
  three `dynvar-*` mismatches reproduce 8.4.3's finding on a second platform.
- **`Thread local is not implemented`** is emitted by c2mir as a warning for
  every `_Thread_local` in the TU (10+ per program). 8.4.3 attributes the
  `dynvar-*` mismatches to `__attribute__((cleanup))`; unimplemented TLS is at
  least as plausible a root cause and is worth ruling out before S-work is
  scoped against the cleanup hypothesis. Platform-independent.

## Reproducing

```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
cmake --build build-jit -j --target tur-jit-spike
bash tools/jit-spike/sweep-full.sh                       # 1373 / 1680 = 81.7%

printf '#define __extension__\n' > /tmp/shim2.h
cat tools/jit-spike/subset-shim.h >> /tmp/shim2.h
SHIM=/tmp/shim2.h bash tools/jit-spike/sweep-full.sh     # 1410 / 1680 = 83.9%
```

One uncontrolled variable, stated for the record: this run used a **Debug**
`tur` (contracts live), and section 8.4.2's build type is not recorded. The
exact 193/193 and 31/31 agreement above argues it does not matter here, but a
Release-`tur` macOS run would close it.

`tools/jit-spike/sweep-full.sh` uses `nproc`, which is not present on a stock
macOS -- it came from Homebrew coreutils on this machine. Worth a
`command -v nproc || sysctl -n hw.ncpu` fallback before anyone else tries to
reproduce.
