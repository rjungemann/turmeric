# `httpd-new-pool-fail-drops-handler` fails under `tur jit` on macOS

**Severity: low (one fixture, undiagnosed).** Pre-existing -- it fails on the
JIT harness both before and after the `_OSSwapInt*` shim work of 2026-07-30, so
it is not a regression from that change. Recorded so it is not lost, not
because it blocks anything.

Split out 2026-07-30 from the macOS JIT baseline measurement.

## Summary

Against a Debug `-DTUR_JIT=ON` build on arm64 macOS, `bash tests/run-jit.sh`
reports:

```
jit fixture summary: 2393 passed, 1 failed, 47 skipped
  (of which 31 passed via the cc fallback -- TUR-W0070)
failed:
  - httpd-new-pool-fail-drops-handler
```

It was also one of the three failures in the pre-change baseline
(`2391 passed, 3 failed, 58 fallbacks`), alongside `httpd-h4-keepalive` and
`httpd-h6-routing` -- both of which now pass. It is the only one that did not.

## What is known

- Reproducible, not flaky: failed in every one of the four full JIT runs on
  2026-07-30, across two different source states.
- It is a `httpd-*` fixture, so it exercises the same handler/pool path as the
  rest of the family; the others in that family now pass under the JIT.
- The fixture's premise (per its name) is a *failure* path: a pool whose
  construction fails must drop its handler. Failure-path fixtures are exactly
  where a JIT/cc divergence in drop-glue or unwind ordering would show up
  first, so that is the first place to look.

## Not yet established

The actual failure mode was not captured -- whether it is a stdout mismatch, a
nonzero exit, a crash, or a timeout, and whether it still falls back to cc.
Nobody has diffed its `jit.stdout` against `expected.stdout`, and no attempt
was made to run it under the cc path for comparison. **Do that first**; the
diagnosis below the surface is entirely open.

## Fix directions

1. Capture the failure: `TUR_TEST_FILTER='httpd-new-pool-fail-drops-handler'
   bash tests/run-jit.sh`, then read
   `tests/fixtures/httpd-new-pool-fail-drops-handler/jit.stdout` and
   `jit.stderr`.
2. Compare against the cc path (`bash tests/run.sh` with the same filter). If
   cc passes and the JIT does not, it is a genuine engine divergence and worth
   a real report; if both fail, it is an httpd defect that the JIT harness
   merely surfaced.
3. If it is JIT-only, suspect drop-glue ordering on the failure path -- the
   family's other fixtures all take the success path and pass.

## Verification

`TUR_TEST_FILTER='httpd-new-pool-fail-drops-handler' bash tests/run-jit.sh`
should report `1 passed, 0 failed`.

## Environment note

Reproducing any macOS JIT number requires `CC` to name the same compiler that
built `tur`. A Homebrew-LLVM-built `tur` with Apple's system clang on `CC`
fails to link the cc fallback (`___asan_version_mismatch_check_v8` undefined),
which turns ~27 fallbacks into spurious failures and makes the baseline
unreadable. See the environment section of
[jit-macos-apple-sdk-headers-force-cc-fallback.md](jit-macos-apple-sdk-headers-force-cc-fallback.md).
