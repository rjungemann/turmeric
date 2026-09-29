# Two r7rs `call/cc` fixtures fail under `tur jit` since the per-thread dynamic environment

**Resolved 2026-09-29** (rjungemann/turmeric#970). Not JIT-specific, and not
the per-thread state's fault as such: a re-entrant continuation captured on
any glibc thread but the main one rewound that thread's thread-locals. The
report as filed is below the resolution.

## Resolution

**Mechanism.** `r7k_stack_base` took the thread's stack top from
`pthread_getattr_np`, which on glibc is the top of the stack *mapping*. For
every thread but the main one, glibc keeps the thread's TCB (the
`struct pthread`) and every module's static TLS block up there, above the first
frame. So a capture that the current top-level form's frame does not bound, and
a worker thread runs no top-level forms, copied the thread's thread-locals into
the image, and every re-entry wrote them all back as they were at the capture.

`82867413` made it visible by moving the value a re-entered continuation
delivers from a process global into `tur_r7rs_dyn`, a thread-local. The
invoker stores the value and the image copy then overwrites it with the
capture-time bytes. Instrumented under `tur jit`, slot 3 sat at
`0x7f26d6e6f050` inside the restored image `[0x7f26d6e6dad0, 0x7f26d6e70000)`;
the invoker wrote `tag=58` and the resumed code read `tag=0 val=0`, which is
the `any holds unknown` panic.

**Why it looked JIT-only.** `tur jit` runs the whole program on the engine's
entry thread, never the main thread, so every capture was exposed. The
compiled path runs top-level forms on the main thread, whose TLS the loader
allocates elsewhere, and neither fixture re-enters on a worker. A compiled
program that does re-enter on a worker failed the same way
(`<: not a number #<unknown>`, every run). macOS keeps no TLS on the thread
stack.

**Why the first bisect pointed nowhere.** Without `libturt_runtime.a` beside
it, the JIT cannot compile an r7rs program at all (`stdatomic.h` not found)
and falls back to cc, which runs on the main thread and passes. A bisect that
builds only `--target tur` sees every commit pass. Built with the archive, the
first bad commit is `82867413`.

**Fix** (`stdlib/r7rs/prelude.tur`, `r7k_stack_base`). On glibc the stack base
is lowered to below the lowest of the thread's TCB (`pthread_self()`) and every
module's TLS block for the calling thread (`dl_iterate_phdr`'s
`dlpi_tls_data`), counting only those that lie on the thread's own stack
mapping. Nothing of a frame lies above them, so the image still holds every
frame. `<link.h>` itself is beyond c2mir, so the prelude declares glibc's
prototype against an incomplete `struct dl_phdr_info` and reads
`dlpi_tls_data` through a mirror of the documented layout, checked against the
callback's `size`.

**Verified.** Both fixtures pass under `tur jit`; the new
`tests/fixtures/r7rs-threads-reentry-on-worker` fails before the fix and
prints `(3 4)` after, compiled and under `tur jit`.

**Not this.** The `tur_r7rs_gc` torture crashes in the fiber and thread cases
(`r7rs-threads-fiber-migration`, `threads-fiber-dynenv`) are a different
bug: `r7rs-threads-fiber-migration` crashed 15 of 60 runs under
`TUR_GC_TORTURE=31` with this fix, against 10-16 of 60 without.

---


**Severity: medium (a wrong answer under the JIT; CI hides it).** Two fixtures
that passed under `tur jit` at `72245ef` (#966) fail deterministically at
`36a9b88` (#969). The compiled path (`tur run`, `tests/run.sh`) still passes
both. The ubuntu JIT leg reports them, but it is `continue-on-error`
(`.github/workflows/ci.yml`, the `JIT engine` job), so `main` reads green:
`main`'s run 36613837100 shows `116: jit fixture summary: 3243 passed, 2 failed`
under a passing check. `main`'s gating macOS JIT leg passed on the same commit.
Whether macOS genuinely passes or never reaches the failing path is not checked.

Found 2026-09-29 while re-validating rjungemann/turmeric#970 after merging
`main`. Reproduced on a clean `origin/main` worktree built Debug + JIT, so it is
not that PR's.

## Repro

```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-jit -j
ASAN_OPTIONS=detect_leaks=0 ./build-jit/tur jit tests/fixtures/region-escape-via-callcc/input.tur
#   1
#   4950
#   panic at <tur-jit>:2937: cast: any holds unknown, not Link      (expected a third line, 7)
ASAN_OPTIONS=detect_leaks=0 ./build-jit/tur jit tests/fixtures/r7rs-threads-cont-other-thread/input.tur
#   error: =: not a number #<unknown>                               (expected 2, then the refusal message)
```

Both fail identically with `TUR_JIT_NO_SPLIT=1` and with `TUR_JIT_NO_PRUNE=1`,
so neither the S2 split nor the JIT prune is involved.

## Where to look

The range `72245ef..36a9b88` has four non-merge commits. The one that touches
the runtime these fixtures exercise (a continuation's captured stack and the
thread it runs on) is `82867413` ("r7rs: the dynamic environment is per thread
and per fiber"), which adds `r7dyn[8]` to the preamble's per-thread state and
changes `src/runtime/tur_tls.c` and `src/compiler/emit_dk_runtime.c`. Both
failures read an `any` whose tag is not one the program knows (`#<unknown>`).
That suggests a value read from the wrong slot after a `call/cc` re-entry or a
cross-thread invoke, in a layout the host runtime (compiled into `tur`) and the
JIT'd program's preamble no longer agree on. Unconfirmed; bisecting the four
commits with a JIT build is the first step.
