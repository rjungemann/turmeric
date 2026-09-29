# `jit-ffi-interp-parametric-record-field` fails on a correct answer: ASan's makecontext warning is in the compared text

**RESOLVED 2026-09-29, the same day it was filed.** The check now captures
stderr to its own file, compares stdout alone against `42\n1`, and looks for
the refusal diagnostic on both streams; `bash tests/run-flags.sh` against a
sanitized Debug JIT build reads `flags summary: 130 passed, 0 failed`. Kept
as the paper trail for a red that sat on the ubuntu JIT leg unseen.

**Severity: low (harness defect; no product impact).** The interpreter gives
the right answer. The check in `tests/run-flags.sh` captures stdout and
stderr together (`2>&1`) and compares the whole capture against `42\n1`, and
on the sanitized (Debug, `-fsanitize=address`) JIT build stderr carries
ASan's one-line makecontext/swapcontext warning, so the comparison fails.

Filed 2026-09-29. The `JIT engine (ubuntu-latest)` leg is the configuration
that runs `tur_flags_tests` against a sanitized JIT build, and it is
`continue-on-error`, so the failure never turned a run red: run 36613837100
(`main` at 36a9b88b, 2026-09-29) reports

```
60: FAIL jit-ffi-interp-parametric-record-field -- expected '42' then '1' (the compiled path's answer), got: ==5422==WARNING: ASan doesn't fully support makecontext/swapcontext functions and may produce false positives in some cases!
60: flags summary: 129 passed, 1 failed
1/3 Test  #60: tur_flags_tests ..................***Failed    8.27 sec
```

with the job green. The `Test` jobs build without `-DTUR_JIT=ON`, where the
check SKIPs, and the macOS JIT leg does not print the warning, so nothing
that gates ever saw it.

## Repro

A Debug JIT build (`-DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON`, CI's
configuration for the leg), then the check's own program with the two
streams kept apart:

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret probe.tur 2>err
# stdout: 42
#         1            (rc 0)
# err:    ==NNNN==WARNING: ASan doesn't fully support makecontext/swapcontext
#         functions and may produce false positives in some cases!
```

`probe.tur` is the heredoc at the check
(`tests/run-flags.sh`, "jit-ffi-interp-parametric-record-field"): two
`call-ptr`s into a `cc`-built `libprobe.so` from inside `(unsafe ...)`.
Or run the harness itself: `timeout 720 bash tests/run-flags.sh` prints the
FAIL line above with a sanitized JIT `./build/tur`.

## Root cause

Two halves, only the first of which is a defect.

- **The capture.** `out=$(... 2>&1)` followed by `[ "$out" != "$want" ]`
  compares stderr as if it were the program's answer. The check was written
  that way so its first branch could `grep` the old refusal diagnostic
  ("no by-value C member type" / "parametric monomorph"), which the
  interpreter prints on stderr. The sibling `jit-ffi-*` checks capture with
  `2>/dev/null`, and `effect-export-syntax` in the same file documents the
  hazard it steps around: "stderr is dropped (the interpreter's sanitizer
  build emits a benign ASan makecontext/swapcontext warning there)".
- **Why this program warns at all.** A trivial `--interpret` program prints
  nothing on stderr. This one's body sits in `(unsafe ...)`, which is a
  handler form to the interpreter (`is_unsafe_marker`), and this handle
  takes the ucontext path -- `eval_drive_ex` hands it to `eval_handle`
  (`src/turi/eval.c:8495`), whose `makecontext(&cont->body_ctx, ...)`
  (`eval.c:2951`) trips ASan's warning (gdb, breakpoint on `makecontext`). The warning is libsanitizer's, printed once per process,
  and says nothing about the FFI marshalling under test.

## Fix

Keep stderr in its own file: compare stdout alone against `42\n1`, and look
for the refusal diagnostic on both streams so the "still refused" branch
keeps its meaning. Landed with this report.

Two related things this does not change, kept for whoever next reads the
leg's log: the ubuntu JIT leg's `continue-on-error` is a gating-policy choice
documented in `.github/workflows/ci.yml` ("FLIP LINUX TO BLOCKING"), and is
why a red `tur_flags_tests` there was invisible; and the same run's
`tur_jit_fixture_tests` failures (`r7rs-threads-cont-other-thread`,
`region-escape-via-callcc`) are the call/cc TLS rewind fixed in #971
(fc40ab9e), not this.
