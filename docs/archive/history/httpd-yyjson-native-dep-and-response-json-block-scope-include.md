---
title: httpd yyjson native dep + http/response.tur block-scope yyjson include
category: Spices packaging convention + latent inline-C bug (no compiler change)
severity: Compile break in tur-httpd's echo_test / concurrent_test once yyjson lands on httpd's include path. Both root causes live in the turmeric-spices repo, not the compiler. The packaging item is working-as-intended convention; the response.tur item is a genuine latent inline-C defect that was dormant only because no httpd test previously had yyjson visible.
description: Two interacting findings surfaced while giving tur-httpd access to JSON. (1) Transitive native (:cmake-deps) deps are only unioned across *declared* :spices deps, so httpd does not inherit json's yyjson cmake-dep -- httpd must re-declare yyjson where it consumes it, exactly as ecs-raylib re-declares raylib. (2) http/response.tur's response-json put `#include <yyjson.h>` inside the function body under __has_include; yyjson.h is header-only with file-scope static-inline functions, so a block-scope include is illegal C. It stayed dormant while yyjson was off httpd's path (the stub branch compiled); once finding 1's fix put yyjson on the path, the include fired inside the function body and broke echo_test / concurrent_test.
status: RESOLVED 2026-06-20. No compiler change. Finding 1 is convention -- httpd re-declares yyjson in its own :cmake-deps (ecs-raylib/raylib precedent). Finding 2 fixed by hoisting the __has_include-guarded `#include <yyjson.h>` to a file-scope C block in http/response.tur; stub-when-absent behavior unchanged and the http stub path still compiles standalone. Both fixes live in turmeric-spices (spices/httpd/build.tur, spices/http/src/http/response.tur).
---

# httpd yyjson native dep + http/response.tur block-scope yyjson include

Two findings, tightly coupled: the fix for the first is what exposed the
second. Both root-cause in the `turmeric-spices` repo; **neither needs a
turmeric compiler change.**

## Finding 1 -- transitive native deps don't propagate to non-declared siblings (convention, not a limitation)

**Observation.** `tur-httpd`'s tests reach JSON encode/decode (json's
`Encode`/`Decode` derivations emit `#include <yyjson.h>`), but httpd does not
inherit json's `yyjson` `:cmake-deps` entry, so yyjson is not on httpd's
include/link path.

**Why this is expected.** The compiler *does* propagate `:cmake-deps`
transitively, but only across **declared** `:spices` deps.
`pkg_collect_transitive_cmake_deps`
(`turmeric/src/compiler/pkg.c:1991`) seeds its worklist from the root
manifest's `:spices` entries (resolving workspace siblings, `:path`, and
`:url`/`:ref` forms via `resolve_spice_dep_dir`), then for each one unions its
`:cmake-deps` and enqueues *its* `:spices`. The union is keyed on declared
spice edges, so:

- `tur-httpd`'s `:spices` are `test` and `http`. It does **not** declare
  `json`, so json's `yyjson` dep is never on the walk.
- The one native-bearing spice httpd *does* declare, `http`, ships
  `mbedtls` -- not `yyjson`. So even the transitive walk that runs gives httpd
  mbedtls, never yyjson.

A spice must therefore declare the native dep it actually consumes. This is
the repo's established convention, confirmed by precedent: `tur-ecs-raylib`
declares `raylib` both as a `:spices` (`:path "../raylib"`) **and**
re-declares `raylib` in its own `:cmake-deps`, even though the `raylib` spice
already declares that exact cmake-dep.

**Resolution.** `tur-httpd` re-declares `yyjson` in its own `:cmake-deps`
(mirroring the json manifest's entry). No compiler change.

## Finding 2 -- http/response.tur: `#include <yyjson.h>` inside a function body

**Latent bug.** `response-json` in
`turmeric-spices/spices/http/src/http/response.tur` emitted, inside the
function's inline-C body:

```c
struct __pr *res = malloc(sizeof(*res));
#ifdef __TUR_HTTP_HAVE_YYJSON
#include <yyjson.h>      /* <-- block scope */
...
```

`yyjson.h` is a header-only library: it defines `static inline` functions at
file scope. Pulling it in *inside a function body* lowers those definitions
into block scope, which is illegal C (static-storage / nested function
definitions are not allowed in block scope). The C compiler rejects it.

**Why it stayed dormant.** The include is guarded by
`__has_include(<yyjson.h>)`. No httpd test previously had yyjson on its
include path, so `__has_include` was false, the stub `#else` branch compiled,
and the defect never fired. The moment finding 1's fix put yyjson on httpd's
path, `__has_include` flipped true, the block-scope `#include <yyjson.h>`
fired, and `echo_test` / `concurrent_test` (both `import http/response`)
failed to compile -- not at link time, at *compile* time, on the
static-inline definitions.

**Resolution.** Hoist the `__has_include`-guarded `#include <yyjson.h>` out of
the function body into a **file-scope C block**, so yyjson's static-inline
functions are defined at translation-unit scope where they are legal. The
function body keeps only the `#ifdef __TUR_HTTP_HAVE_YYJSON` use-sites. The
stub-when-absent behavior is unchanged (absent header -> the err-returning
`#else` path), and the http stub path was verified to still compile
standalone. No compiler change.

## Takeaway

- Native deps follow declared `:spices` edges only; consume-site
  re-declaration of `:cmake-deps` is the convention (ecs-raylib/raylib,
  httpd/yyjson), not a turmeric gap.
- Inline-C that brings in a header-only library's static-inline definitions
  must do so from a **file-scope** C block, never from inside a `defn` body --
  otherwise the include is a time bomb that only detonates once the optional
  header becomes visible on some consumer's path.
