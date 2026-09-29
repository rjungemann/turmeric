# Two r7rs `call/cc` fixtures fail under `tur jit` since the per-thread dynamic environment

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
