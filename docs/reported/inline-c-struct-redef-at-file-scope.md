---
title: Inline-C struct redefinitions at file scope break multi-block codegen
severity: high (hard compile error; blocks any TU that imports multiple files with overlapping inline-C struct decls)
discovered: 2026-06-07
context: building turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur with `tur run`
---

## Summary

When a translation unit pulls in multiple inline-C blocks that each
redeclare the *same* shared shape (`struct __httpd_resp`,
`struct __httpd_req`, `struct __httpd_warg`, ...), the emitted C file
contains identical top-level `struct` definitions side-by-side and the
C compiler rejects it with `redefinition of '__httpd_resp'`.

This is a structural codegen problem, not a one-off in tourist. Any
spice that uses the "redeclare the carrier struct inside each inline-C
block" idiom (which CLAUDE.md effectively documents as the standard
way to read tourist/httpd handle fields) will hit this the moment a
single binary imports two such blocks.

## Severity

High. It is a hard compile error in a public-facing spice
(`tourist` + `httpd`) on what should be a green path — running the
existing `cascade` test fixture. It blocks:

- the cascade fixture (`turmeric-spices/.../tests/fixtures/cascade/cascade.tur`),
- and by extension every downstream user who imports both
  `httpd/response` and `tourist/routing` (or any other pair that
  redeclares the same struct).

It is not a silent miscompile, but it gates an entire feature surface.

## Repro

From a clean tree with the in-tree `tur` built:

```sh
/Users/rjungemann/Projects/turmeric/build/tur run \
  /Users/rjungemann/Projects/turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur
```

Observed:

```
/tmp/tur-build/..._cascade_tur.c:2884:8: error: redefinition of '__httpd_resp'
/tmp/tur-build/..._cascade_tur.c:2874:8: note: previous definition is here
/tmp/tur-build/..._cascade_tur.c:2925:8: error: redefinition of '__httpd_warg'
/tmp/tur-build/..._cascade_tur.c:2948:8: error: redefinition of '__httpd_req'
... (then a separate mbedtls header-not-found error, see below)
tur: cc invocation failed (status 256)
```

`tur check` on the same fixture and on `tourist/routing.tur` succeeds
(no type errors); the failure is entirely in C codegen / cc.

The emitted C around the first error site
(`/tmp/tur-build/..._cascade_tur.c:2874-2886`) shows two adjacent
top-level definitions of `struct __httpd_resp` with identical bodies
— one emitted from `httpd/response.tur`, another from a different
inline-C block that also needs the layout.

## Expected

Either:

1. The emitter deduplicates identical inline-C struct definitions in
   the same TU (canonical hoisting / once-per-TU guard), or
2. inline-C struct decls are emitted at function-local scope (inside
   the wrapper function we generate per inline-C block), so duplicates
   are fine because they are not at file scope.

Today the emitter appears to place each inline-C block's leading decls
at file scope, so identical structs collide.

## Root cause sketch (with pointers)

Each `tourist`/`httpd` `.tur` file uses the documented idiom of opening
an inline-C block with `#include`s and a fresh `struct __httpd_xxx { ... };`
re-declaration, then doing field access through that local declaration.
Examples (file:line in `../turmeric-spices`):

- `spices/httpd/src/httpd/response.tur:48,87,126,156,180` — repeated
  `struct __httpd_resp { ... };` declarations.
- `spices/httpd/src/httpd/request.tur:36,57,...` — repeated
  `struct __httpd_req { ... };`.
- `spices/httpd/src/httpd/server.tur:269,301,307,328` and
  `spices/httpd/src/httpd/pool.tur:67,87` — `struct __httpd_warg`.
- `spices/tourist/src/tourist/routing.tur:337-340, 297-300, 306-308,
  311-313` — `struct __httpd_resp`, `struct __tourist_cascade`.

Each block individually emits to a C file the structure:

```c
#include <stdlib.h>
struct __httpd_resp { ... };   // at file scope
<wrapper-fn-body using the struct>
```

When two such blocks land in the same TU, the second
`struct __httpd_resp { ... };` at file scope is a C-language
redefinition error, even though the bodies match — C does not allow
two `struct` *definitions* with the same tag at the same scope.

So the bug is not in any one spice; it's in how the inline-C block's
preamble is laid out in the emitted C.

## Proposed fixes

Ordered by intrusiveness:

1. **Emit inline-C preamble at function-local scope.** Wrap the whole
   block (includes + struct decl + body) inside the generated
   `static int64_t __inline_c_<n>(...) { ... }` function. Identical
   struct redeclarations are then fine because each lives in its own
   function scope. Cheapest correct fix; matches the way the idiom is
   already used (field access only, no cross-block sharing).
2. **Dedupe at the TU layer.** Have the emitter collect inline-C
   prelude lines (`#include`, top-level `struct ... { ... };`) and
   emit each *unique textual block* exactly once at the top of the
   file. Closer to what a user might expect ("I declared it once per
   block, why is the compiler seeing N copies"), but requires textual
   normalization.
3. **First-class shared-struct facility.** Add a way to declare a
   shared C struct once (e.g. an `extern-c-struct` form) that is
   visible to every inline-C block in the same module/TU. Most work,
   but removes the underlying ergonomics gap that drives the
   per-block redeclaration idiom.

(1) is the minimum viable fix. (3) is the right long-term answer.

## How to validate a fix

- `cd /Users/rjungemann/Projects/turmeric-spices/spices/tourist`
- `/Users/rjungemann/Projects/turmeric/build/tur run tests/fixtures/cascade/cascade.tur`
  → should reach the `run-all` summary, not fail in `cc`.
- A regression fixture in this repo: a minimal `.tur` that imports two
  trivial modules, each of which has an inline-C block declaring the
  same `struct foo { int x; };`. Today: cc error. After fix: builds.

## Note on the second error in the same run

The cascade build also fails with:

```
fatal error: 'mbedtls/net_sockets.h' file not found
```

That is a *separate* environment / dependency issue (the httpd spice
pulls in an mbedtls header but the local toolchain has no mbedtls
installed). Worth filing under spice docs / a `requires.mbedtls` skip
marker, but it is not the codegen bug above and should not be
conflated with it.
