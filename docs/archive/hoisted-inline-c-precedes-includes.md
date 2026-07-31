# Hoisted inline C can precede the `#include` it depends on

> **Status:** RESOLVED 2026-07-29
> **Fix:** `hoist_tur_include_directives()` now sorts payloads into two buckets
> -- preprocessor directives (`#include`/`#define`/`#undef`/`#pragma`) first,
> then code -- each preserving source order among themselves. `<stdlib.h>` now
> lands on line 3 of the httpd TU, ahead of the `malloc` call on line 10.
> **Follow-through:** with the ordering fixed, the
> `-Wno-error=implicit-function-declaration` suppression was removed from all
> three cc sites (the thing findings 21.2 had to revert). No snapshot churn --
> `emit-c` does not hoist. See findings 21.3.

**Severity: medium** (was masked everywhere by
`-Wno-error=implicit-function-declaration`; the masking was the real cost, since
that flag then hid unrelated wrong-code bugs -- it is what let
[../archive/jit-xxh64-missing-prototype.md](../archive/jit-xxh64-missing-prototype.md)
reach a second backend).

## One-line summary

`hoist_tur_include_directives()` concatenated every `__tur_include__` payload in
**source order**, but that marker carries two different kinds of payload --
preprocessor directives and file-scope *code*. A code payload declared earlier
in a file therefore landed ahead of a directive payload it depended on, so the
generated TU called library functions with no declaration in scope.

## Repro

```sh
./build-turjit/tur build tests/fixtures/httpd-async-echo/input.tur -o /tmp/x
```

Look at the generated C (under `/tmp/tur-build/` or `$TMPDIR/tur-build/`):

```c
  1  #define _DEFAULT_SOURCE 1
  2  #include <stdint.h>
  ...
  9  static char *httpd_conn_own_cstr(HttpdConn *c, char *s) {
     ...  HttpdOwnedStr *n = (HttpdOwnedStr *)malloc(sizeof(HttpdOwnedStr)); ... }
 10  #include <stdlib.h>
```

Line 9 calls `malloc`; `<stdlib.h>` arrives on line 10. Both come from
`__tur_include__` markers in `stdlib/httpd.tur` -- the helper from line 93, the
include from line 3916 -- so source order put the code first.

With the suppression removed (see below) this is a hard error under Apple clang
21 / GCC 14+:

```
...__input_tur.c:9:119: error: call to undeclared library function 'malloc'
with type 'void *(unsigned long)'; ISO C99 and later do not support implicit
function declarations [-Wimplicit-function-declaration]
```

Any spice whose inline C uses a libc function while a *different* block supplies
the corresponding `#include` can hit this; the httpd spice is simply the one in
the corpus that does.

## Why it has never been noticed

`src/main.c` appends `-Wno-error=implicit-function-declaration` to **all three**
cc invocations, so the call compiles as an implicit declaration and, for
`malloc` on LP64, happens to get the right ABI anyway.

That workaround was load-bearing -- removing it before this fix broke the httpd
fixtures -- but it was indiscriminate. It also silenced the genuinely wrong-code
`tur_hamt_hash_xxh64` case, which truncated a 64-bit hash to `int` on every host
and corrupted argument registers outright under c2mir/MIR on Apple arm64. One
include-ordering bug was buying blanket immunity from an entire class of UB. See
findings 21.2 for the write-up of the attempted removal and why it was reverted.

## The fix

`hoist_tur_include_directives()` now scans each `__tur_include__` payload, looks
at its first non-whitespace token, and routes it to one of two buffers:
directives (`#include`, `#define`, `#undef`, `#pragma`) or code. The directive
buffer is emitted first. Relative order *within* each bucket is preserved, so a
feature-test `#define` still precedes the include it conditions; only the two
kinds are separated.

The corpus only ever uses three payload shapes (`typedef`, `static`, `#include`),
so no conditional-compilation structure is at risk of being split.

With that in place the `-Wno-error=implicit-function-declaration` suppression was
removed from all three cc sites in `src/main.c`.

**Verification, and the method matters here** -- per 21.2, `emit-c` output and
the plain suite both give a false all-clear on this specific question, because
`emit-c` does not hoist and the fixtures that would show the difference were
already failing environmentally. The check that counts is the JIT sweep, whose
step-6 fallback drives the real `tur build` path:
`tools/jit-spike/sweep-turjit.sh` reports 1,641 native + 27 fallback = 1,668 of
1,701 with the 31 `httpd-*` back to `fallback-env` -- identical to the run with
the flag still in. `bash tests/run.sh` is 2373 passed / 57 failed, all 57
environmental.

## Provenance

Found during the arm64 macOS re-validation of the JIT engine (Apple M2, Darwin
27.0.0, Apple clang 21.0.0), `tur` v0.32.2 at `158397346`. See section 21.2 of
[../upcoming/jit-engine-j0-findings.md](../upcoming/jit-engine-j0-findings.md).
