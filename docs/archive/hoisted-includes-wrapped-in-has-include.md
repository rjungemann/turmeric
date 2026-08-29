# Hoisted inline-C includes are wrapped in `#if __has_include`, so a missing header is silent

**Severity: high** (turns "your include path is wrong" into a pile of unrelated
type errors thousands of lines away). Found 2026-08-28 getting
`turmeric-spices` CI green, against `tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang).

**Status: RESOLVED 2026-08-29.** Neither fix direction below was safe as
written -- the wrap turned out to be load-bearing and deliberately used. See
[Resolution](#resolution) at the end.

## Repro

```turmeric
(defmodule mh (export)
  (defn call-it [] : int
    ```c
    #include <definitely_not_a_real_header.h>
    return some_undeclared_library_call();
    ```)
  (defn main [] : int (call-it)))
```

## Observed

`tur check` is silent. The hoisted include is wrapped:

```c
/* ==== tur: end of fixed runtime preamble ==== */
#if __has_include(<definitely_not_a_real_header.h>)
#include <definitely_not_a_real_header.h>
#endif
```

and the body then compiles against nothing:

```
error: call to undeclared function 'some_undeclared_library_call';
       ISO C99 and later do not support implicit function declarations
tur: cc invocation failed (status 256)
```

(On gcc, where the implicit declaration is a warning rather than an error, this
degrades further: it links-fails at `collect2` instead, with the header never
mentioned.)

## Expected

A header the author explicitly asked for and did not get should be an error at
that point, naming the header. At minimum a warning naming it.

`__has_include` is the right tool for an *optional* header -- one the emitter
itself probes for, with a fallback path behind `#else`. It is the wrong tool for
a header the user wrote in an inline-C block, because there is no fallback: the
`#endif` is followed directly by code that assumed the include succeeded. The
guard therefore never prevents a failure, it only relocates it.

## Where it bit

`spices/raygui` had `-I` pointing at raygui's repo root while the header lives
in `src/`. Because the miss was silent, every `Gui*` call fell through to an
implicit declaration returning `int`, and the failures surfaced as:

```
error: incompatible types when assigning to type 'Font' from type 'int'   (GuiGetFont)
error: invalid initializer                                                (GuiGrid returns Vector2)
```

Both look like binding bugs in the spice -- wrong return type on the Turmeric
side, a struct/int confusion in the FFI layer. Neither was. The spice had in
fact never linked at all.

The cost compounds: someone previously "fixed" this far enough to commit **14
files of `tur emit-c` output** capturing the broken elaboration. Those
checked-in snapshots then encoded the wrong answer as the expected one.

## Fix direction

Two options, and they are not exclusive:

1. **Stop wrapping user-written includes.** Hoist them verbatim. `cc` then
   produces `fatal error: definitely_not_a_real_header.h: No such file or
   directory` with the header name and a line number, which is exactly the
   right diagnostic and costs nothing to produce. If the wrap exists to keep
   *emitter-generated* optional includes working, narrow it to those -- the
   emitter knows which includes it synthesized and which came out of a
   ` ```c ` block.

2. **Keep the wrap, add an `#else`.** If some caller genuinely depends on the
   tolerant behavior, make the failure loud instead of silent:

   ```c
   #if __has_include(<foo.h>)
   #include <foo.h>
   #else
   #error "tur: inline-C requested <foo.h>, not found on the include path"
   #endif
   ```

   This preserves the existing structure and still fails at the right line with
   the right name.

(1) is cleaner and is probably what was intended; (2) is the safe change if the
wrap turns out to be load-bearing for something. Before picking, find out why
the wrap is there -- if it has no current justification, delete it.

Worth checking `-I` diagnostics at the same time: `tur` could warn when a
`:c-flags`/`-I` directory does not exist, which would have caught the raygui
case one layer earlier.

## Guides to update when fixed

- docs/guides/c-integration-guide.md -- document how includes in a ` ```c `
  block are hoisted, and what happens when one is not found.

---

## Resolution

Fixed 2026-08-29. The symptom and the reasoning in this report were exactly
right -- "the guard therefore never prevents a failure, it only relocates it"
is the correct one-line statement of the bug. But the report's instruction
"find out why the wrap is there -- if it has no current justification, delete
it" turned up a justification, and it is stronger than expected: **both listed
fix directions would have broken the build.**

### Why the wrap exists (and why neither option worked)

The wrap's own comment gave a reason that is false for this scanner: "a hoisted
`#include <X>` loses its surrounding `#ifdef`." It cannot -- `tur_hoist_top_includes_scan`
stops at the first line that is not a blank/comment/`#include`/`#define`, so an
include inside an `#ifdef` is never hoisted at all. That misleading comment is
probably why the wrap looked unjustified.

The real reason is the opposite of losing a guard: **stdlib deliberately writes
platform headers bare, outside their `#ifdef`, specifically to be hoisted.**
`stdlib/fs.tur`'s `fs/mkdir` carries a 10-line comment saying so:

```c
#include <sys/stat.h>
#include <direct.h>
/* direct.h is included at the TOP of the body, outside the #ifdef, on purpose.
   ... Hoisting it bare is safe on POSIX: tur_emit_hoisted_include wraps angle
   includes in `#if __has_include(<...>)`, so a platform without direct.h just
   skips it. */
#ifdef _WIN32
  return (int64_t)_mkdir(path);
#else
  return (int64_t)mkdir(path, 0755);
#endif
```

The reason is the include-guard problem the hoister exists to solve: an
`#include` left inside the `#ifdef` is function-scoped, and the header's own
guard makes it expand to nothing in the *second* function that needs it.
`stdlib/term.tur` (`<io.h>`) and `stdlib/image.tur` (`<windows.h>`,
`<mach-o/dyld.h>`) do the same, each with its own explanatory comment.

So **fix direction 1** (hoist verbatim) breaks `fs/mkdir` on every POSIX build,
and **fix direction 2** (`#else #error`) breaks it too -- `<direct.h>`
legitimately is not there. An audit of every leading angle include in
`stdlib/`, `tests/fixtures/` and `examples/` found **12 of 18 are
platform-conditional** (`sys/wait.h`, `termios.h`, `dirent.h`, `arpa/inet.h`,
`mach-o/dyld.h`, ...). Tolerance is the norm here, not the exception, so a
hard error by default was never on the table.

### The fix

Keep the skip; stop it being anonymous. The emitter cannot tell "this header is
deliberately absent here" from "your `-I` is wrong" -- but the author can, so
the tolerance became opt-in and the default became loud:

- **Unmarked** angle include, missing: the `#else` emits a `#pragma message`
  naming the header. `tur: inline-C requested <raygui.h>, not found on the
  include path ...` now appears at the top of the cc output, ahead of the
  implicit-declaration and link errors it causes.
- **Marked** `/* tur:optional */` on the include line: silent, as before. The
  four deliberate stdlib sites carry it now, so they produce no noise.

`#pragma message` rather than `#warning` or `#error`: it is supported by every
toolchain in the contract (GCC/Clang/MSVC/Emscripten, unlike `#warning` on
MSVC < VS2022), and emitted C is compiled `-Wall` and **never** `-Werror`
(`src/main.c`), so it can name the problem without ever being the thing that
breaks a build. Given how much of the tree relies on the skip, that mattered
more than the stronger diagnostic this report asked for.

Dedup is by the directive itself, so a header written bare at one site and
marked at another is one entry, and the optional marking wins.

Touched: `tur_emit_hoisted_include`, `tur_hoist_include_add_ex` and the
scanner's marker detection in `src/compiler/emit_core.c`; the four stdlib
sites; `src/compiler/emit_internal.h`.

### Regression

`tests/fixtures/inline-c-optional-hoisted-include/` pins the opt-out path --
the `fs/mkdir` shape, with an absent header marked optional and the body's
`#ifdef` taking the fallback; it must skip silently and still run. The unmarked
path is snapshot-covered by `inline-c-result-builder/expected.c` and
`inline-c-typed-result-option/expected.c`, both regenerated here. Suite: 2720
passed, 0 failed.

### Not done

The report's closing suggestion -- warn when a `:c-flags`/`-I` directory does
not exist -- is untouched and still worth doing. It would have caught the
raygui case one layer earlier, at the manifest rather than at cc. Filed
nowhere; pick it up with the next `-I` work.

### Guide

`docs/guides/c-integration-guide.md` gains a "Leading `#include` lines are
hoisted automatically" section covering what the scan consumes and where it
stops, plus "What happens when a hoisted header is not found" -- the marked and
unmarked branches, and why quoted includes are never wrapped.
