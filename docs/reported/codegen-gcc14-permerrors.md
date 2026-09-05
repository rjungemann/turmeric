# Generated C has type errors that GCC >= 14 rejects

**Severity:** medium -- latent today, breaks every `tur build` the moment CI's
compiler crosses GCC 14. Not Windows-specific.

**Status:** open. Worked around, not fixed.

## Summary

The C that `tur emit-c` produces contains genuine type errors:

- `-Wincompatible-pointer-types` -- initializing a `struct <anonymous> *` from a
  different anonymous struct pointer.
- `-Wint-conversion` -- passing an integer where a pointer is expected.

GCC 14 promoted both from warnings to **errors by default**. Every compiler
before that only warned, which is why this has gone unnoticed: Linux CI is on an
older GCC and simply never mentioned it.

It surfaced on Windows only because MSYS2 ships **GCC 16**, where it took out
137 of ~1440 fixtures at the `cc` step. It has nothing to do with Windows --
a Linux box with GCC 14+ fails identically.

## Repro

```sh
tur build tests/fixtures/hkt-do-m/input.tur -o /tmp/h    # with GCC >= 14
```

```
error: initialization of 'struct <anonymous> *' from incompatible pointer type
       'struct <anonymous> *' [-Wincompatible-pointer-types]
error: passing argument 1 of 'ctor_Own' makes pointer from integer without a
       cast [-Wint-conversion]
```

Heavily clustered in the HKT fixtures (`hkt-do-m`, `hkt-functor-*`,
`hkt-monad-*`, `hkt-closures`, ...) and the by-value ADT paths
(`byval-adt-local-owning-field-drop`, `heap-make-struct-roundtrip`).

## Current workaround

`src/main.c` appends, to every `cc` invocation:

```
-Wno-error=incompatible-pointer-types -Wno-error=int-conversion
```

That restores pre-GCC-14 behaviour. With it, the affected fixtures compile
**and produce correct output** -- so the emitted code appears to work despite
the type mismatch, which is exactly why this went unnoticed.

The flags are appended *after* `TUR_CC_FLAGS` so a user override cannot drop
them.

## Root cause (not yet pinned down)

Both diagnostics point at the dictionary/closure lowering: the HKT dict path
appears to hand a carrier integer or a differently-shaped anonymous struct to a
constructor expecting a typed pointer. The generated code happens to be
layout-compatible, which is why it runs -- but the C is not well-typed, and a
compiler is entitled to assume it never happens.

## Fix directions

Make the emitted code well-typed rather than layout-lucky: emit an explicit cast
at the dictionary/ctor boundary, or give the anonymous structs real tags so the
pointer types actually match. Then drop the two `-Wno-error` flags and let the
compiler enforce it.

Worth doing before a CI toolchain bump makes it urgent.
