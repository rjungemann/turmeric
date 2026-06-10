---
title: File-scope inline-C struct redefinitions across non-identical blocks
severity: high (hard compile error; blocks cascade fixture and any multi-module TU where two modules share a struct name but emit it inside different top-level inline-C blocks)
discovered: 2026-06-09
context: turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur after the mbedtls/yyjson layers were gated out (see cascade-mbedtls-header-not-found.md resolution)
related: docs/archive/history/inline-c-struct-redef-at-file-scope.md
---

## Summary

The previous file-scope inline-C dedup (`inline_c_dedup_seen` in
`src/compiler/emit_module.c`) collapses **byte-identical** top-level
inline-C blocks across modules within a single TU. The cascade build
exposes the next layer of the same class of bug: two modules each emit a
top-level inline-C block that contains the same `struct __foo { ... };`
declaration, but the surrounding blocks differ (different `#include`
preludes, different sibling struct/function decls in the same block).
The dedup key never matches, both blocks are emitted, and the second
`struct __foo { ... };` triggers `error: redefinition of '__foo'`.

## Repro

```sh
build/tur run ../turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur
```

After the mbedtls/yyjson gates from
[`cascade-mbedtls-header-not-found.md`](cascade-mbedtls-header-not-found.md)
have landed, the cascade build fails with:

```
error: redefinition of '__httpd_resp'
error: redefinition of '__httpd_warg'
error: redefinition of '__httpd_req'
```

The first occurrence of `__httpd_resp` sits inside a block like:

```c
#include <stdlib.h>
#include <string.h>

struct __httpd_resp {
  int    status; char  *body; int body_len;
  char **hdr_keys; char **hdr_vals; int hdr_count; int hdr_cap;
};
```

and the second inside:

```c
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

struct __httpd_resp {
  int    status; char  *body; int body_len;
  char **hdr_keys; char **hdr_vals; int hdr_count; int hdr_cap;
};
struct __httpd_wbuf { char *bytes; int len; };
```

The two `struct __httpd_resp` declarations are byte-identical *as
struct decls*, but the enclosing inline-C blocks aren't, so the existing
dedup misses them.

## Root cause sketch

`inline_c_dedup_seen` keys on the full emitted block. The block scope is
"one ` ```c ... ``` ` literal at file scope per Turmeric module," so
when two modules each say:

```turmeric
;; module A
``c
#include <stdlib.h>
#include <string.h>
struct __httpd_resp { ... };
``

;; module B
``c
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
struct __httpd_resp { ... };
struct __httpd_wbuf { ... };
``
```

the keys diverge even though the *shared declaration set* overlaps. C's
one-definition rule doesn't care about block boundaries; it sees two
file-scope `struct __httpd_resp` decls and rejects the TU.

## Proposed fixes

Ordered by intrusiveness:

1. **Per-declaration dedup.** Parse each file-scope inline-C block into
   a stream of preprocessor directives and top-level decls, key dedup
   on `(kind, name, normalized-body)` per decl, and re-emit only the
   first occurrence of each name. Includes are already idempotent (header
   guards), so emitting each include's text once per block is harmless;
   the win is suppressing duplicate `struct`/`typedef`/etc. The keying
   has to be *post-whitespace-normalization* so the cascade case
   collapses cleanly.

2. **Spice-side workaround (short-term).** Hoist the shared
   `struct __httpd_*` decls out of each per-function inline-C block in
   `turmeric-spices/spices/httpd/src/` into a single
   `extern-c-header`-style shared declaration. Avoids the codegen
   change but only fixes httpd; the underlying compiler limitation
   stays.

Fix #1 is the right long-term answer -- the spice-side workaround works
file-by-file but doesn't help the next spice that hits the same shape.

## Validation

- After the fix, `build/tur run
  ../turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur`
  must compile cleanly and reach the `run-all` summary.
- Add a regression fixture in this repo: two modules each emitting a
  file-scope inline-C block with overlapping struct decls but disjoint
  surrounding `#include` sets, both imported into one TU.
- The existing
  `tests/fixtures/inline-c-file-scope-struct-dedup/` byte-identical
  case must still pass.
