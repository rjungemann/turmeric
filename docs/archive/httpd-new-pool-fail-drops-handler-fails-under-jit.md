# `httpd-new-pool-fail-drops-handler` fails under `tur jit` on macOS

**RESOLVED 2026-07-30.** Root cause was harness drift, not the JIT engine and
not the fixture: `tests/run-jit.sh` never exported `TUR_BIND_LOOPBACK=1`, which
`tests/run.sh:72` has always exported. One line in `tests/run-jit.sh` fixes it.

With the fix the macOS arm64 JIT corpus is **fully green** --
`2414 passed, 0 failed, 47 skipped` (31 via the cc fallback), where it had been
`2413 passed, 1 failed`. That green baseline is what let the new `jit` CI job
(`.github/workflows/ci.yml`) gate the macOS leg rather than merely report it.

Originally split out 2026-07-30 from the macOS JIT baseline measurement.

## Root cause

`stdlib/httpd.tur:703` chooses its bind address **at run time** from the
environment:

```c
addr.sin_addr.s_addr = htonl(getenv("TUR_BIND_LOOPBACK") ? INADDR_LOOPBACK
                                                         : INADDR_ANY);
```

So a fixture's behaviour depends on which harness launched it. `tests/run.sh`
exports `TUR_BIND_LOOPBACK=1`; `tests/run-jit.sh` did not.

The fixture occupies `127.0.0.1:<port>` (its own inline-C `occupy-port`, which
sets `SO_REUSEADDR`, binds loopback, and listens), then asserts that
`httpd-new-pool`'s bind of the same port is **refused** -- that being the
failure path whose handler-box drop it exists to check.

- **Under `run.sh`** httpd binds `127.0.0.1:<port>` -- an exact collision with
  the occupying socket. The bind fails on every platform. Prints `refused`.
- **Under `run-jit.sh`** httpd bound `0.0.0.0:<port>` instead. Both sockets set
  `SO_REUSEADDR`, and BSD permits a **wildcard** bind while a **specific**
  address holds the port. The bind succeeded, so `httpd-new-pool` returned a
  live pool and the fixture printed `built`.
- **Linux** refuses the wildcard-over-specific bind regardless, which is why the
  failure was macOS-only -- and why it read as a JIT defect or a BSD
  `SO_REUSEADDR` portability gap. It was neither.

The earlier "fixture-side BSD-vs-Linux `SO_REUSEADDR` semantics gap" diagnosis
(findings 32.2) was half right: BSD semantics are the *mechanism*, but the
*cause* was the missing export, and the fixture itself needed no change.

## Fix

`tests/run-jit.sh` now exports `TUR_BIND_LOOPBACK=1` alongside its existing
`ASAN_OPTIONS` export, mirroring `tests/run.sh:72`.

Note this also silently affected **every** server fixture under the JIT
harness, not just this one: they were all binding all interfaces rather than
loopback. On Windows that is what makes the Defender Firewall prompt appear per
fixture binary; it also left the JIT harness needlessly exposed to port
conflicts with whatever else is on the machine.

## What the original report asked for, answered

The report's open questions ("the actual failure mode was not captured ... do
that first") resolved as:

- **Failure mode:** stdout mismatch -- `built` where `refused` was expected.
  Not a crash, not a timeout, not a nonzero exit.
- **Deterministic:** yes, 5/5 consecutive JIT runs before the fix.
- **cc-path comparison:** the AOT suite passed it, but only because `run.sh`
  sets the env; invoking the same AOT binary standalone (no
  `TUR_BIND_LOOPBACK`) prints `built` too. So there was never a JIT-vs-cc
  divergence -- the hypothesis in the original fix direction 3 (drop-glue
  ordering on the failure path) is ruled out.

## Verification

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON -DTUR_DEBUG_SANITIZE=OFF
cmake --build build -j
TUR=./build/tur TUR_FORCE=1 bash tests/run-jit.sh
```

Reports `2414 passed, 0 failed, 47 skipped`. The single fixture, filtered,
passes 3/3:

```sh
TUR=./build/tur TUR_FORCE=1 \
  TUR_TEST_FILTER='^httpd-new-pool-fail-drops-handler$' bash tests/run-jit.sh
```

## Environment note (still applies to any macOS JIT measurement)

Reproducing any macOS JIT number requires `CC` to name the same compiler that
built `tur`. A Homebrew-LLVM-built `tur` with Apple's system clang on `CC`
fails to link the cc fallback (`___asan_version_mismatch_check_v8` undefined),
which turns ~27 fallbacks into spurious failures and makes the baseline
unreadable. Building `-DTUR_DEBUG_SANITIZE=OFF` with Apple clang, as above,
sidesteps both that and the ASan startup deadlock. See the environment section
of [jit-macos-apple-sdk-headers-force-cc-fallback.md](jit-macos-apple-sdk-headers-force-cc-fallback.md).
