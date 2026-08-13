# Reactor fd callbacks are called through the wrong function-pointer type

**Severity: medium (undefined behavior, currently benign on arm64).** The call
works today because the mismatched types happen to share an ABI slot on
AArch64 and x86-64. It is still UB: UBSan traps it, and any toolchain that
enforces function-pointer identity at an indirect call (CFI,
`-fsanitize=cfi-icall`, CET/BTI-hardened builds, WASM's strict `call_indirect`
type check) turns it into a hard failure rather than a warning.

Found 2026-07-30 while reconciling `tests/run.sh` against `tests/run-jit.sh`.

## Summary

`call_tur_fd_cb` (`src/async/reactor.c:183-188`) casts every registered fd
callback to:

```c
typedef int64_t (*fn3_t)(void *, int64_t, int64_t, int64_t);
((fn3_t)(intptr_t)fat[0])((void *)fat, id, (int64_t)events, user_data);
```

But the Turmeric-side callback it actually invokes is declared
(`stdlib/httpd.tur:602`):

```turmeric
(defn httpd-accept-cb [env : ptr<void> id : int events : int user : ptr<void>] : nil
```

which the emitter lowers to:

```c
static void httpd_hyaccept_hycb(void * env, int64_t id, int64_t events, void * user);
```

Two independent disagreements:

| | `fn3_t` (call site) | `httpd_hyaccept_hycb` (definition) |
|---|---|---|
| return type | `int64_t` | `void` |
| 4th parameter | `int64_t` | `void *` |

## Repro

Needs a Debug build (UBSan is on by default there):

```sh
CC=$(brew --prefix llvm)/bin/clang TUR=./build-turjit/tur \
  TUR_TEST_FILTER='^httpd-(h4-keepalive|h6-routing)$' bash tests/run.sh
```

Both fail with an empty stdout. `tests/fixtures/httpd-h4-keepalive/actual.stderr`:

```
src/async/reactor.c:187:5: runtime error: call to function httpd_hyaccept_hycb
  through pointer to incorrect function type
  'long long (*)(void *, long long, long long, long long)'
  note: httpd_hyaccept_hycb defined here
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior src/async/reactor.c:187:5
```

Confirmed pre-existing: both fixtures fail identically with the working tree's
`stdlib/httpd.tur` change stashed.

## A second, distinct instance in emitted C

The same run reports a second mismatch, this one entirely inside the generated
TU -- closures invoked through a pointer type that does not match their
definition:

```
tests_fixtures_httpd-h4-keepalive_input_tur.c:9339:7: runtime error: call to
  function __fn_2324 through pointer to incorrect function type
  'void (*)(void *, long long)'
```

`httpd-h6-routing` shows it at two sites (`:9383`, `:11363`) with `__fn_2342`
and `__fn_2324`. This is the emitter's own closure-invocation lowering, not the
reactor's hand-written typedef, so it is a separate fix even though the
symptom is identical.

## Why `tests/run-jit.sh` does not see it

Both fixtures **PASS** under `bash tests/run-jit.sh` while failing under
`bash tests/run.sh`. That is not the JIT being more correct -- MIR-generated
code simply carries no UBSan instrumentation, so the same mismatched call is
made and nothing checks it. Do not read the JIT harness's green as evidence
this is fixed, and do not "fix" a UBSan failure by observing that the JIT is
happy.

This asymmetry is worth remembering generally: the JIT harness cannot see any
`-fsanitize=undefined` finding in program code.

## Root cause

`call_tur_fd_cb`'s typedef spells the callback's env and user pointers as
`int64_t`, which is the `:int` stand-in CLAUDE.md's "No Lazy `:int` Stand-Ins"
rule is about -- a pointer typed as a machine integer because it is a pointer
underneath. The Turmeric declaration got the types right (`ptr<void>`, `nil`);
the C side that calls into it did not, and nothing cross-checks the two.

## Fix directions

1. **Make the reactor's typedef match the emitted signature.** For a
   `(fn [ptr<void> int int ptr<void>] nil)` callback that is
   `void (*)(void *, int64_t, int64_t, void *)`. Smallest correct change; fixes
   the `reactor.c:187` site outright. Check the sibling comment at
   `reactor.c:179-181` -- the signal/timer callbacks it describes ("where the
   second argument carries signum/value") may need the same audit, since they
   share the `fat[0]` convention.
2. **Fix the emitter's closure-call lowering** for the `__fn_*` sites. Separate
   change; needs the emitted pointer type to be derived from the closure's real
   signature rather than a fixed `void (*)(void *, int64_t)` shape.
3. **Consider making this class visible.** Both instances went unnoticed
   because the only harness that would catch them is the one whose failures
   were being attributed elsewhere. A UBSan-clean gate on the httpd fixtures
   would keep it from regrowing.

Fix 1 is self-contained and worth doing on its own; fix 2 is the larger one.

## Verification

`CC=$(brew --prefix llvm)/bin/clang TUR=./build-turjit/tur
TUR_TEST_FILTER='^httpd-' bash tests/run.sh` should report zero failures and no
`UndefinedBehaviorSanitizer` lines in any `actual.stderr`.

## Resolution (2026-08-13)

Fix directions 1 and 2 landed; 29 of the 32 UBSan findings are gone. The
residue is filed separately as
[emitter-thunk-type-return-mismatch](emitter-thunk-type-return-mismatch.md),
because its cause is not the one this report assumed.

### Reproduced on Linux, with a correction to the repro

The report's repro is macOS/Homebrew-clang specific. It reproduces on stock
Ubuntu clang-18, with two prerequisites the report does not mention -- and one
of them had to be fixed before the report could be verified at all:

- **`src/compiler/elab_memory.c` had no trailing newline**, so *any* clang build
  of this tree died at `-Werror,-Wnewline-eof` before compiling anything.
  Pre-existing on `main`. One byte, fixed here, because it is the gate on the
  only toolchain that can see this bug.
- `libclang-rt-18-dev` must be installed, or `src/runtime/arena.c` cannot find
  `sanitizer/asan_interface.h`.

**GCC cannot see any of this.** GCC has no `-fsanitize=function`, so a stock
Debug build on Linux reports nothing here regardless of how wrong the types are.
That is a third blind spot alongside the two the report already names (the JIT
harness carrying no instrumentation, and the failures being attributed
elsewhere).

One correction to the symptom: on Linux the affected fixtures **PASS** while
emitting the UB diagnostics, because UBSan defaults to print-and-continue.
`summary: 33 passed, 0 failed` is not evidence of anything. The signal is
`grep "incorrect function type" tests/fixtures/*/actual.stderr`.

### Fix 1 -- the reactor (four sites, not one)

The report names `reactor.c:187`. A clang/UBSan run found **four**:

| Site | Helper | Wrong type | Correct type |
|---|---|---|---|
| `:187` | `call_tur_fd_cb` | `int64_t (*)(void *, int64_t, int64_t, int64_t)` | `void (*)(void *, int64_t, int64_t, void *)` |
| `:198` | `call_tur_timer_cb` | `int64_t (*)(void *, int64_t, int64_t)` | `void (*)(void *, int64_t, void *)` |
| `:209` | `call_tur_chan_cb` | as `:187` | as `:187` |
| `:879` | `local_fiber_trampoline` | `int64_t (*)(void *, int64_t)` | `void (*)(void *, void *)` |

The report's direction 1 asks for exactly this audit ("check the sibling comment
at `reactor.c:179-181` -- the signal/timer callbacks ... may need the same
audit"). They did. The three shapes are now named typedefs (`TurFdCbFn`,
`TurTimerCbFn`, `TurFiberBodyFn`) with a comment recording the mapping -- `nil`
is `void`, `ptr<void>` is `void *` -- and that nothing cross-checks them against
the Turmeric declarations, which is how they drifted.

**`local_park_wake_cb` needed more than a typedef.** It is a C callback
registered as a fat closure, and it served *both* the 4-argument fd/chan
convention and the 3-argument timer convention from one 4-parameter definition,
on the reasoning recorded in its comment: "we only read `arg2` for the fd/chan
path ... so the differing arity is harmless." That is true of the ABI and false
of the language -- an indirect call through a mismatched function-pointer type is
UB whether or not the callee reads the extra slot, which is the same argument
this whole report rests on. It is now two entry points over a shared body, each
matching its call site exactly.

That in turn forced a struct change: `tur_local_park_fd` registers an fd source
*and* a companion timeout source in one call, so the two handlers cannot share
one `park_cb_fat` array. `LocalFiber` gains `park_timer_cb_fat`.

### Fix 2 -- not the emitter

The report's "second, distinct instance in emitted C" attributes the `__fn_*`
mismatches to "the emitter's own closure-invocation lowering". Following them
into the generated C, **most were hand-written inline C in `stdlib/httpd.tur`** --
the same defect as fix 1, in a second file:

```c
/* stdlib/httpd.tur, three sites: 353, 2530, 3178 */
typedef void (*fn1_t)(void *, int64_t);
((fn1_t)(intptr_t)fat[0])((void *)fat, (int64_t)(intptr_t)conn);
```

The handler is `(fn [c : ptr<void>] : nil)`, which emits
`void (void *, void *)`. Correcting the three typedefs took the count from 32
findings to 3. (A fourth typedef at `:1987`, the basic-auth verifier's
`int64_t (*)(void *, const char *, const char *)`, was already right --
`cstr` emits as `const char *`.)

The genuinely-emitter-caused residue is 3 findings, all return-type-only, and
its cause is the `:int`-typed closure sinks in the httpd API rather than a fixed
pointer shape in the lowering. Details in the new report.

### Fix 3 -- deferred, with a reason

The report's direction 3 (a UBSan-clean gate on the httpd fixtures) cannot pass
until the residue is fixed, so it is carried forward on the new report rather
than added here in a form that would have to start out disabled.

### Verification

`CC=/usr/bin/clang TUR=./build-clang/tur TUR_TEST_FILTER='^httpd-' bash tests/run.sh`
reports **zero** `reactor.c` findings, down from four, and 3 emitted-C findings,
down from 32.

`tests/run.sh` (GCC): 2592 passed, 0 failed. `tests/run-turi.sh`: 1779 passed, 0
failed.

Under clang the full suite is 2589 passed, 3 failed -- `complex-basics`,
`complex-smith-div`, and `load-in-imported-module`. All three were confirmed
pre-existing by building the parent commit with clang and reproducing them
identically; they are clang-vs-GCC differences unrelated to this report, and are
recorded at the end of the new one.
