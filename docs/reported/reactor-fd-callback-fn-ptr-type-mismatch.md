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
