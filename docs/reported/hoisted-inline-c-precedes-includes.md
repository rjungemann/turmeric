# Hoisted inline C can precede the `#include` it depends on

**Severity: medium** (currently masked everywhere by
`-Wno-error=implicit-function-declaration`; the masking is the real cost, since
that flag then hides unrelated wrong-code bugs -- it is what let
[../archive/jit-xxh64-missing-prototype.md](../archive/jit-xxh64-missing-prototype.md)
reach a second backend).

## One-line summary

`hoist_tur_include_directives()` emits hoisted user inline-C blocks and hoisted
`#include` directives in an order that can place a code block *before* the
standard header it needs, so the generated TU calls library functions with no
declaration in scope.

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

Line 9 calls `malloc`; `<stdlib.h>` arrives on line 10.

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

That workaround is load-bearing today -- removing it breaks the httpd fixtures
-- but it is indiscriminate. It also silenced the genuinely wrong-code
`tur_hamt_hash_xxh64` case, which truncated a 64-bit hash to `int` on every host
and corrupted argument registers outright under c2mir/MIR on Apple arm64. One
include-ordering bug is currently buying blanket immunity from an entire class
of UB. See findings 21.2 for the full write-up of an attempted removal and why
it was reverted.

## Fix direction

Make the hoist emit in two passes: **all** hoisted `#include` directives (and
`#define`s they depend on) first, then all hoisted code blocks. Today's
interleaving preserves per-block source order across both kinds, which is what
allows a block to precede a later block's include.

Once hoisted includes reliably precede hoisted code, re-attempt the removal of
`-Wno-error=implicit-function-declaration` from all three sites in `src/main.c`
and re-verify with the JIT sweep (`tools/jit-spike/sweep-turjit.sh`), **not**
with `emit-c` output or the plain suite -- findings 21.2 explains why both of
those give a false all-clear.

Interim, cheap improvement: the comment at `src/main.c` still describes the flag
as covering a single now-fixed xxh64 call site. It should say that the flag is
an include-ordering workaround and point here, so the next reader does not
repeat the removal attempt.

## Provenance

Found during the arm64 macOS re-validation of the JIT engine (Apple M2, Darwin
27.0.0, Apple clang 21.0.0), `tur` v0.32.2 at `158397346`. See section 21.2 of
[../upcoming/jit-engine-j0-findings.md](../upcoming/jit-engine-j0-findings.md).
