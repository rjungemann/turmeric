# JIT macOS full-corpus run: `__extension__` codegen gap, and `atexit` that resolves and then crashes

**RESOLVED 2026-07-30.** All four findings are closed or carried forward:

- **Finding 1 (`__extension__`)** -- FIXED at the time of writing; verified
  still fixed (no emission site remains in `src/compiler/`, only explanatory
  comments).
- **Finding 2 (`atexit`)** -- FIXED. The engine intercepts `atexit` into a
  per-image list rather than registering the real one (`src/jit_engine.c`,
  findings 9.4), and J2 drains that list at image teardown. All
  `module-defer-*` fixtures pass under `tur jit`.
- **Finding 3 (Apple SDK header residue)** -- still live, and the only part
  that is. Split into its own report so `docs/reported/` carries something
  actionable rather than this whole document:
  docs/archive/jit-macos-apple-sdk-headers-force-cc-fallback.md. It is
  performance-only -- every affected fixture passes via the cc fallback.
- **Finding 4 (c2mir ignores `#pragma pack` / `__attribute__((packed))`)** --
  not exercised. Verified 2026-07-30 that the emitter produces neither
  construct, so nothing depends on the layout c2mir gets wrong. Recorded as a
  constraint in the split-out report: do not start emitting packed structs
  without checking c2mir first.

The original report follows, including the corpus measurements, which remain
the record of how the platform delta was decomposed.

**Severity:** medium (JIT spike / J1 planning only -- nothing here affects
`tur build` or `tur emit-c` output correctness for the normal `cc` path).

Measured 2026-07-28 on Apple Silicon (arm64, Darwin 27.0.0, AppleClang 21.0.0),
MIR at the `a8ab7c31` pin, branch `claude/j0-jit-engine-plan-znqibo` at
`d657707dc`. This is the full-corpus macOS run that
`docs/upcoming/jit-engine-j0-findings.md` section 8.4.2 asks for and leaves
open.

## Summary -- the run does not land where 8.4.2 predicts

**Status:** finding 1 is FIXED in this branch; findings 2, 3 and 4 are open.

These results are reconciled into the canonical J0 write-up as **section 9** of
[docs/upcoming/jit-engine-j0-findings.md](../upcoming/jit-engine-j0-findings.md),
which also carries the amendment pointers on each superseded claim in sections
0, 3.1, 3.2, 6 and 8. This file is the detail; section 9 is the summary.

| Run | Pass | Rate |
|---|---|---|
| Linux, eager (8.4.2, reported) | 1424 / 1680 | 84.8% |
| **macOS, eager, artifacts as committed** | **1373 / 1680** | **81.7%** |
| macOS, eager, after the finding-1 codegen fix | 1409 / 1680 | 83.9% |

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

First isolated by adding a single line to the shim (`#define __extension__`) as
a diagnostic probe, which took the corpus from **1373 to 1410 (+37 fixtures,
+2.2 points)**. The landed emitter fix reproduces that at 1409; the
one-fixture difference is `stm-stress` flaking on the shim's atomic lowerings,
not a real gap between the two approaches. It removes three whole failure
classes:

| Class | as committed | with the define |
|---|---|---|
| `syntax error on void` (all 25 are `session-*` / `defstruct-field-session-*`) | 25 | 0 |
| `syntax error on {` | 6 | 0 |
| `syntax error on identifier` (`__auto_type` residue) | 199 | 193 |

Two counts then match Linux's full-corpus figures **exactly** -- `__auto_type`
residue 193 = 193, and GNU constructs in user inline-C 31 = 31. That agreement
is the best evidence available that the two corpora are otherwise measuring the
same thing.

### Status: FIXED

Fixed in the emitters rather than the shim. `({ ... })` on its own is accepted
by gcc, clang, and c2mir; the `__extension__` prefix only suppresses a
`-pedantic` diagnostic, and generated C is compiled `-O2 -std=c99 -Wall
-fno-strict-aliasing` (`src/main.c:2444`, `:4675`, `:5378`) which never sets
it. All nine sites now emit the bare form, and the three prefix matchers in
`src/turi/eval.c` (`:7445`, `:7473`, `:7514`) moved in lockstep -- the
interpreter recognizes these inline-C bodies by text, so the emitters and the
matchers must stay in sync.

Verified:

- `bash tests/run.sh` -- **2399 passed, 0 failed**. Zero snapshot churn: of the
  140 `expected.c` files, none exercises these emitters.
- Generated C compiles with **zero warnings** under the real flags. Under an
  added `-pedantic` there is now one `statement expression` warning per affected
  TU; that is the diagnostic `__extension__` was buying, and it is a warning,
  never an error. A user passing `-pedantic` via `TUR_CC_FLAGS` would see it.
- Interpreter session paths (`session-send`, `session-choose-left`,
  `session-mp-ping`) still evaluate correctly.
- JIT corpus **1373 -> 1409/1680**, exactly the predicted +37.

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

### The predefines do not fix it, and what that uncovered

Predicted above as "cheap to fix with a few predefines." **That prediction is
wrong and the measurement says so:** `__arm64__=1`, `__aarch64__=1`,
`TARGET_CPU_ARM64=1`, `TARGET_OS_MAC=1`, `__LITTLE_ENDIAN__=1` (now in
`subset-shim.h` under `#ifdef __APPLE__`) recover **zero** fixtures. Corpus is
1409/1680 with them and 1410/1680 without; the one-fixture delta is
`stm-stress`, which flakes on the shim's atomic lowerings and landed on both
sides across three runs.

What they do is push all 9 past the header gate into a deeper failure, and one
of those is materially more serious than the parse error it replaced:

| Was | Now | Count |
|---|---|---|
| `#error TargetConditionals.h: unknown compiler` | `syntax error on typedef` -- an ordinary c2mir subset gap | 5 |
| `empty preprocessor expression` (`mach/port.h:100`) | **`static assertion failed: "struct changed size unexpectedly"`** at `mach/message.h:543` and `:569` | 3 |
| `unresolved import: _OSSwapInt16` | unchanged | 1 |

The middle row is the find, and it has a root cause worth its own section --
see finding 4. It is invisible until `__arm64__` has a value, which is the
argument for keeping the predefines even though they fix nothing.

Two incidental notes on the mechanism:

- c2mir predefines `__APPLE__`, `__arm64__`, and `__aarch64__` but with an
  **empty replacement list**, so `#if __arm64__` is an empty controlling
  expression rather than a true one. That is arguably a c2mir bug worth
  reporting upstream.
- `_OSSwapInt16` stays unresolved because `libkern/_OSByteOrder.h` emits the
  static-inline bodies only under `__GNUC__`; its fallback declares them extern
  and libSystem exports no such symbol. Defining `__GNUC__` is the obvious next
  probe; it was not tried, because it would also switch many other headers onto
  GNU-builtin paths c2mir lacks.

## Finding 4 -- c2mir silently ignores `#pragma pack` and `__attribute__((packed))`

The root cause behind finding 3's static-assert row, and the only thing found on
macOS whose failure mode is *wrong layout* rather than *refused input*.

### Repro

```c
#pragma pack(push, 4)
struct packed4  { unsigned a, b, c; uint64_t d; };
#pragma pack(pop)
struct natural  { unsigned a, b, c; uint64_t d; };
struct attrpack { unsigned a; uint64_t d; } __attribute__((packed));
```

| | clang | c2mir |
|---|---|---|
| `packed4` sizeof / offsetof(d) | 20 / 12 | **24 / 16** |
| `natural` sizeof / offsetof(d) | 24 / 16 | 24 / 16 |
| `attrpack` sizeof / offsetof(d) | 12 / 4 | **16 / 8** |

Both packing mechanisms are dropped; the unpacked control agrees, so this is
specifically packing and not a general layout difference.

**The two differ in detectability, and the more dangerous one is the quiet
one.** `#pragma pack` at least produces `warning -- unknown pragma`.
`__attribute__((packed))` produces **no diagnostic at all** -- which is the same
mechanism section 3.1 already describes ("c2mir parses `__attribute__((...))`
and discards it with no diagnostic"), but a consequence class 3.1 does not
cover. 3.1's table lists only the attributes *the emitter* uses and concludes
three of four matter; `packed` arrives from **system headers and user
inline-C**, and its consequence is silent ABI divergence rather than missing
initialization. It deserves a fourth row.

### How it reaches the corpus, and why the 3 failures are NOT corruption

`stdlib/image.tur:51` includes `<mach-o/dyld.h>` for `_NSGetExecutablePath`.
That header transitively reaches `mach/message.h`, whose trailer structs live
inside `#pragma pack(push, 4)` (line 291) and carry XNU's own
`xnu_static_assert_struct_size` ABI locks. c2mir drops the packing, computes 64
and 72 where XNU demands 60 and 68, and the assert fires.

Being precise about severity, because the loud version is the harmless one:

- **These 3 fixtures are not miscompiled.** Nothing in `stdlib/` or
  `src/runtime/` references `mach_msg`, `mach_port_t`, or any `MACH_*` symbol;
  the only mach-o symbol used is `_NSGetExecutablePath(char *, uint32_t *)`,
  which passes no packed struct. The header is declared-only. XNU's assert
  catches c2mir's bug at parse time and the program never runs -- a compile
  error, not corruption.
- **The latent risk is real but currently unreached.** Turmeric's runtime and
  stdlib use zero packing (`grep 'pragma pack\|__attribute__((packed))' src/
  stdlib/` is empty), so the JIT/host struct boundary is clean today. The open
  vector is **user inline-C** defining a packed struct that the host runtime
  also sees -- offsets would diverge with no diagnostic whatsoever.

### The platform framing inverts here

The c2mir defect is host-independent -- the repro above is pure C with no
platform conditionals. macOS is not more broken, it is **louder**: XNU ships
`_Static_assert` ABI locks in its own headers, so the divergence is caught at
compile time. Linux headers carry no equivalent guard, so the same wrong layout
there would simply be adopted silently. **If this is going to bite anyone, it
will bite the platform that never reported it.**

### Fix directions

- J1 must decide explicitly whether packing is in-subset. The cheapest honest
  option is to **reject** rather than mislay: teach the normalizer to fail on
  `#pragma pack` and `__attribute__((packed))` so a program that needs them
  falls back to `cc` (the step-6 fallback that already handles GNU constructs in
  user inline-C) instead of silently getting different offsets.
- Implementing packing in c2mir properly is the upstream fix and is out of scope
  for J1.
- Either way this belongs in section 3.1's table, which currently reads as
  though `unused`/`constructor`/`cleanup` are the complete attribute story.

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
bash tools/jit-spike/sweep-full.sh                       # 1409 / 1680 = 83.9%
```

To recover the pre-fix 1373 / 1680 = 81.7% baseline, revert the emitter change
(`git revert` the finding-1 commit) and re-run. The `#define __extension__`
shim line used to diagnose it is no longer needed and is deliberately NOT in
`subset-shim.h`: the emitters no longer produce the token, and adding it back
would only mask a reintroduction.

One uncontrolled variable, stated for the record: this run used a **Debug**
`tur` (contracts live), and section 8.4.2's build type is not recorded. The
exact 193/193 and 31/31 agreement above argues it does not matter here, but a
Release-`tur` macOS run would close it.

`tools/jit-spike/sweep-full.sh` uses `nproc`, which is not present on a stock
macOS -- it came from Homebrew coreutils on this machine. Worth a
`command -v nproc || sysctl -n hw.ncpu` fallback before anyone else tries to
reproduce.
