---
title: httpd/tourist cascade build fails with `mbedtls/net_sockets.h` not found
severity: medium (hard compile error, but environment/dependency-driven, not a codegen defect; gates the cascade fixture only where mbedtls is absent)
discovered: 2026-06-07
context: building turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur with `tur run`
related: docs/reported/inline-c-struct-redef-at-file-scope.md
---

## Summary

Building the `cascade` fixture in the `tourist` spice fails in `cc` with:

```
fatal error: 'mbedtls/net_sockets.h' file not found
```

The `httpd` spice pulls in mbedtls headers (TLS support) via an inline-C
`#include`, but the local toolchain has no mbedtls installed, so the
generated C cannot be compiled. This is distinct from -- and was
surfaced alongside -- the file-scope inline-C struct redefinition bug
(now fixed; see the `related` report). With that codegen bug resolved,
this header-not-found error is the *next* failure on the same build, and
it is the one that actually blocks `cascade` on a vanilla machine.

## Severity

Medium. It is a hard compile error, but the root cause is an unmet
*external* native dependency (mbedtls), not a flaw in the Turmeric
compiler or codegen. It blocks:

- the cascade fixture
  (`turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur`),
- and any downstream build that links the `httpd` TLS path on a host
  without mbedtls headers/libraries.

It does not affect this repository's own test suite (no in-repo fixture
depends on mbedtls).

## Repro

From a host without mbedtls installed, with the in-tree `tur` built and
the sibling `../turmeric-spices` checkout present:

```sh
build/tur run \
  ../turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur
```

Observed (after the struct-redef fix; previously this was preceded by the
`redefinition of '__httpd_resp'` errors):

```
fatal error: 'mbedtls/net_sockets.h' file not found
tur: cc invocation failed
```

`tur check` on the fixture succeeds -- the failure is purely in the
`cc` compile of the emitted C, because the `#include <mbedtls/...>` line
has no satisfying header on the include path.

## Expected

A clean machine that does not have mbedtls should be able to either:

1. build the non-TLS path of `httpd`/`tourist` without pulling in
   mbedtls headers at all (TLS gated behind a feature/flag so the plain
   HTTP cascade does not require it), or
2. have the fixture **auto-skip** when mbedtls is unavailable, the same
   way `requires.spices` fixtures auto-skip when the sibling checkout is
   absent.

Today neither happens: the mbedtls `#include` is unconditional in the
httpd codegen path, so the cascade fixture is a hard failure rather than
a graceful skip wherever mbedtls is not installed.

## Root cause sketch

The `httpd` spice's TLS support emits an inline-C `#include
<mbedtls/net_sockets.h>` (and likely sibling mbedtls headers) into the
generated C unconditionally -- even when the program only exercises the
plain-HTTP cascade and never touches TLS. The header is a build-time
native dependency of the spice, not something Turmeric vendors or
detects. Pointers live in `../turmeric-spices/spices/httpd/` (the
mbedtls-touching server/TLS source), outside this repo.

## Proposed fixes

Ordered by intrusiveness:

1. **Skip marker for the fixture.** Add a `requires.mbedtls` marker (in
   the spices repo's fixture harness, mirroring `requires.spices`) that
   PASS-skips the cascade fixture when the mbedtls headers are not on the
   include path. Cheapest; unblocks CI on bare hosts. Does not fix the
   underlying "plain HTTP needlessly needs mbedtls" coupling.
2. **Feature-gate the TLS path.** Make the mbedtls `#include` (and the
   TLS code that needs it) conditional on a spice feature/flag, so a
   plain-HTTP build of `httpd`/`tourist` emits no mbedtls dependency and
   the cascade fixture compiles without mbedtls at all. Right long-term
   answer; removes the coupling.
3. **Document + provide mbedtls.** Document mbedtls as a required native
   dep for the httpd TLS path and supply install instructions / a CI
   provisioning step. Least satisfying (every consumer must install
   mbedtls even for non-TLS use), but a stopgap.

This work lives in `../turmeric-spices` (the httpd/tourist spices), not in
this repository -- see the "Spice Repository Layout" rule in CLAUDE.md. No
in-repo change is appropriate beyond this report.

## How to validate a fix

- On a host **without** mbedtls: `build/tur run
  ../turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur`
  should reach the `run-all` summary (fix #2) or PASS-skip (fix #1),
  not fail in `cc` with `mbedtls/net_sockets.h file not found`.
- On a host **with** mbedtls installed: the same command should build
  and run the cascade end-to-end, exercising the TLS path.
