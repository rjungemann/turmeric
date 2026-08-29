# Hoisted inline-C includes are wrapped in `#if __has_include`, so a missing header is silent

**Severity: high** (turns "your include path is wrong" into a pile of unrelated
type errors thousands of lines away). Found 2026-08-28 getting
`turmeric-spices` CI green, against `tur v0.40.0` / turmeric `5c9d533`.

**Verified 2026-08-28** on a freshly built `v0.40.0` (macOS arm64 / Apple
clang).

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
