# Inline-C `#include <header>` only works in the first function that uses it

**Status:** Reported
**Severity:** Medium-high. Pre-existing latent codegen issue. Blocks any
spice whose inline-C bodies in two or more functions `#include` the same
system header. Surfaces as `error: use of undeclared identifier '<type>'`
at C compile time, after Turmeric type-checking has already passed --
i.e. a silent failure mode at the language level.
**Discovered:** 2026-06-14, while attempting `tur build` on the sqlite
spice as part of the S2 opaque-handle migration
(`docs/reported/spices-int-stand-in-audit-2026-06-14.md`). Independent
of any S2 changes -- reproduces on a clean tree.
**Scope:** `src/emit*` (codegen layout of inline-C bodies) +
`docs/guides/c-integration-guide.md` (current docs explicitly invite
this pattern; see line 222: "`#include`s for system headers inside a
function scope").

## Summary

When a Turmeric module contains two or more `defn`s whose inline-C body
each begins with `#include <some_system_header>`, only the *first* function
to be emitted compiles successfully. Every subsequent function fails at
C compilation with `use of undeclared identifier` for any type or symbol
declared in that header.

Root cause: an inline-C block is pasted verbatim into the generated C
function's body. A `#include` issued there is a *function-scope* include
in C -- the declarations it brings in are lexically scoped to that
function's block. The first inline-C body that includes the header pulls
in its declarations *and* the header's include-guard macros (e.g.
`SQLITE3_H`, `_RTMIDI_C_H_`). Subsequent functions in the same translation
unit issue the same `#include`; the preprocessor sees the guard already
defined and skips the body entirely. Those functions therefore have **no
visibility** of types like `sqlite3`, `sqlite3_stmt`, or `RtMidiPtr`.

The current spice ecosystem hides this behind missing system headers
(rtmidi, plutovg, postgres, etc. fail earlier with "header not found" in
typical dev sandboxes), so the issue surfaces as soon as a developer
*does* have the header installed -- e.g. sqlite3.h via the macOS SDK.

## Minimal repro

```turmeric
;; /tmp/repro.tur
(defmodule incl-bug
  (export f g)

(defn f [] : int
  ```c
  #include <sqlite3.h>
  sqlite3 *p = NULL;
  return p == NULL ? 0 : 1;
  ```)

(defn g [] : int
  ```c
  #include <sqlite3.h>
  sqlite3 *p = NULL;
  return p == NULL ? 0 : 1;
  ```))
```

```
$ tur build /tmp/repro.tur
error: use of undeclared identifier 'sqlite3'
   4480 |   sqlite3 *p = NULL;
        |   ^
```

`f` compiles. `g` does not. Same header, same body, second occurrence loses
the declarations to the include guard.

`stdio.h` happens to work in the same shape because its declarations live
in headers that the system SDK pulls in via a different chain (compiler-
builtin `__has_include` shortcuts, `<sys/_types/_FILE.h>` exports, etc.).
A header whose entire body is guarded with a normal `#ifndef HEADER_H`
wrapper -- which is *most* headers -- exhibits the bug.

## Observed vs expected

- **Observed:** `tur build` succeeds on a module with one inline-C user of
  the header; fails as soon as a second function in the same module
  includes the same header.
- **Expected:** Either (a) emit `#include`s at file scope so all functions
  see the declarations, or (b) document this limitation and provide a
  module-level "C preamble" form so users can hoist includes themselves.

The Turmeric type checker is unaware of either form, so the failure
happens at the C-compile stage with no Turmeric-level diagnostic. This
is the silent-failure characteristic that elevates the severity --
"compiles fine, fails at link" is exactly the kind of footgun the
language is supposed to avoid.

## Root-cause pointers

- Inline-C bodies are pasted verbatim into the function body in
  `src/emit*.c` (`emit_inline_c` / `emit_function_body`). The paste does
  no lexical analysis of `#include` directives.
- No module-level preamble pass exists today. The generated `.c` only
  carries a top-level `#include "module.h"` plus the runtime-closure
  preamble.
- `docs/guides/c-integration-guide.md:222` explicitly says function-scope
  includes are an intended escape hatch, so users will keep writing
  them. Without a fix, that line of the docs is wrong in practice.

## Proposed fix directions

### Option A -- hoist `#include` directives to file scope at emit time

In `emit_inline_c`, scan each inline-C block for top-of-block
`#include <...>` and `#include "..."` lines (i.e. directives that appear
before the first non-blank, non-comment, non-`#include` line). Strip them
from the function body and accumulate them into a per-module include set.
Emit the deduplicated set at the top of the generated `.c`, after the
existing `#include "module.h"` line.

Pros:
- Backward compatible: existing inline-C code keeps compiling.
- Eliminates the include-guard interaction entirely.
- One pass, no syntax change.

Cons:
- Behavioural shift: an `#include` that *intentionally* depended on
  function-scope visibility (none exist in-tree that I can find) would
  change semantics. Document the new behaviour.
- The scanner must be conservative -- only lift top-of-block includes; do
  not lift includes that follow real C code, since those may be guarded
  by surrounding `#ifdef` blocks meant to be local.

### Option B -- introduce a module-level `c-prelude` form

Add a top-level `(c-prelude ```c #include <sqlite3.h> ``` )` form that
emits its body once at file scope. Document that all inline-C bodies in
the module should *not* re-include the same header.

Pros:
- Explicit; no behavioural ambiguity.
- Composes with `(extern-c ...)` declarations the same way.

Cons:
- Requires an audit of every existing spice and a migration pass --
  every inline-C `#include` becomes either a module preamble entry or
  a noop.
- Bigger surface; new reader form.

**Recommendation:** Option A. It is the smallest change that makes
existing code (and the existing documented pattern) actually work, and
nothing in-tree relies on function-scope include semantics.

## Validation

- Add a fixture under `tests/fixtures/inline-c-multi-include/` with two
  `defn`s in one module that both `#include <stdint.h>` (a header with a
  normal include guard) and use a type it declares. The fixture passes
  after the fix.
- Add a fixture that includes `<sqlite3.h>` and uses `sqlite3 *`; gate it
  behind `requires.sqlite3` so CI runs it where the header is available
  and skips it otherwise.
- Re-run `tur build spices/sqlite` against the sibling repo -- after the
  S2 opaque migration (branch `s2-sqlite-defopaque-2026-06-14`), it
  should produce `build/bin/libsqlite.a` cleanly.

## Cross-references

- `docs/guides/c-integration-guide.md:222` -- the line that documents
  function-scope inline-C includes as the supported escape hatch.
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- audit;
  surfaced this issue while attempting the Phase-4 sqlite slice.
- `../turmeric-spices` branch `s2-sqlite-defopaque-2026-06-14` -- the
  S2 sqlite work whose end-to-end build is gated on this fix.
