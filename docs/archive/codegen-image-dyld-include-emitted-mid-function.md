# Codegen: image/hot-reload `#include <mach-o/dyld.h>` emitted inside a function scope

> **RESOLVED (2026-07-22).** The include lived in the inline-C body of
> `image/build-stamp` (`stdlib/image.tur`), inside the `#ifdef _WIN32` /
> `#elif defined(__APPLE__)` blocks -- so `#include <windows.h>` and
> `#include <mach-o/dyld.h>` were emitted mid-function. On Linux the `#else`
> branch has no include, which is why the fixtures passed there and the defect
> was masked; macOS and Windows both hit "function definition is not allowed
> here" from the transitively-defined `static inline` functions.
>
> Fix: both platform includes were moved to the TOP of the inline-C body as
> bare leading `#include` lines (before the block comment). The codegen already
> has a top-include hoister (`tur_hoist_top_includes_scan`) that lifts leading
> bare includes to file scope and strips them from the body, and
> `tur_emit_hoisted_include` wraps each angle include in
> `#if __has_include(<...>)` -- so on Linux the macOS/Windows headers are simply
> skipped, and on their own platform they land at file scope where the
> `static inline` definitions are legal. The `#ifdef`-guarded *code* (which is
> legal in a function body) stays put.
>
> Verified on Linux: `emit-c` now emits `#if __has_include(<mach-o/dyld.h>)` /
> `<windows.h>` at file scope (~line 129) with no `#include` directive left in
> `image_slbuild_hystamp`'s body; the three `image-*` fixtures build and pass;
> full `bash tests/run.sh` green (2268 passed). A minimal Linux repro confirms
> the mechanism: a `static inline`-bearing header `#include`d inside a function
> errors ("invalid storage class for function"), the file-scope placement
> compiles. Original finding below.

**Severity:** Medium -- all three `image-*` (hot-reload/image) fixtures fail
to build on macOS. The dyld header is `#include`d at a point where C forbids
the function definitions it contains.

## Symptom

`image-hooks-tracked`, `image-reload-hook`, `image-roundtrip` -> `tur build
failed`:

```
In file included from ..._input_tur.c:7414:
In file included from /.../mach-o/dyld.h:30:
In file included from /.../mach-o/loader.h:55:
In file included from /.../architecture/byte_order.h:38:
In file included from /.../libkern/OSByteOrder.h:55:
/.../libkern/arm/OSByteOrder.h:70:1: error: function definition is not allowed here
```

`<mach-o/dyld.h>` transitively pulls in headers with `static inline`
function **definitions** (`OSByteOrder`, `byte_order.h`). "function definition
is not allowed here" means the `#include` was expanded **inside another
function body / block scope**, not at file scope.

## Repro

Build any of `tests/fixtures/image-{hooks-tracked,reload-hook,roundtrip}`.
The fixtures themselves contain no `#include` -- the dyld include is injected
by the image/hot-reload runtime preamble (dlopen/dyld image lookup), and it
lands at generated-C line ~7414, mid-function.

## Root cause

The image subsystem's inline-C / runtime support emits `#include
<mach-o/dyld.h>` at the site where the image API is *used* (inside a lowered
`defn` body) rather than hoisting it to the top-of-file include block. Any
system header that defines `static inline` functions is illegal inside a
function scope, so the whole compile fails.

## Fix directions

- Hoist the image/dyld `#include` (and any other system-header include the
  image runtime needs) into the TU's top-level include preamble, alongside
  the other `#include`s, instead of emitting it at the inline-C use site.
- If the include is coming from an `extern-c`/inline-C block associated with
  an image builtin, that block's headers must be collected and emitted at file
  scope during preamble assembly, not inlined into the calling function.
- macOS-only surface (dyld), but the include-placement rule is general: no
  system-header `#include` should ever be emitted inside a function body.
