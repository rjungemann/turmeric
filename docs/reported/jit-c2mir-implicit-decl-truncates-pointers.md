# c2mir silently truncates pointers returned by three CRT functions

**Severity: high on the JIT path, Windows only.** A pointer returned by
`strtok`, `strpbrk` or `memchr` comes back cut to 32 bits and sign-extended, so
it is non-NULL and unusable. Code that only counts results looks correct; the
first dereference crashes.

Found 2026-09-06 chasing `path-string`, one of the two remaining `run-jit.sh`
failures on Windows. Fixed in the same change -- this records the shape, because
the failure mode is the kind that hides.

## What it looks like

`tests/fixtures/path-string` segfaults under `tur jit` (all three generation
tiers, including `interp`, so it is not codegen) and passes under `tur run`. It
also passes under gdb, which sent the first investigation after a heap bug that
was not there.

Reduced to six lines of inline C, the fault is exact:

```
  n2 work=/a/./b/../c abs=1
  n3 segs=000001FC026A0930
  n4 tok=                     <- strtok returned an unusable pointer
  <segfault>
```

Printing the raw value against a pointer known to be good settles it:

```
plain    ptr=FFFFFFFFDA7FF7C1  buf=00000053DA7FF7C0   <- sign-extended from 32 bits
declared ptr=00000053DA7FF7D1  buf=00000053DA7FF7D0   tok=[a]
```

An `extern char *strtok(char *, const char *);` in the same inline-C body makes
it correct. So c2mir does not have a prototype, treats the call as implicitly
returning `int`, and the high half of the address is lost.

## Which functions

Measured in one translation unit, comparing the returned pointer's high 32 bits
against a buffer whose address is known:

| intact | truncated |
| --- | --- |
| `strstr` `strchr` `strrchr` | `strtok` |
| `malloc` `calloc` `strdup` `getenv` | `strpbrk` |
| | `memchr` |

## What is NOT established

Why these three and not their neighbours. `strpbrk` is declared in
`string.h` (line 86) in exactly the same form as `strrchr` (line 87) --
`_CONST_RETURN char *__cdecl` -- and one truncates while the other does not.
`strtok` additionally carries `__MINGW_ATTRIB_DEPRECATED_SEC_WARN`, which looked
like the differentiator until `strpbrk` ruled it out, and which expands to
nothing anyway unless `__MINGW_MSVC_COMPAT_WARNINGS` is defined.

A plausible remaining theory is that c2mir carries builtin prototypes for some
of these and is not reading the header declarations for any of them -- which
would make the intact column a list of c2mir builtins rather than a list of
declarations that parsed. Not tested.

The fix does not depend on the answer, but the answer would say whether the list
of three is complete. **Treat it as a lower bound.**

## The fix

`emit_module.c` emits prototypes for the three, next to `#include <string.h>`:

```c
#if defined(_WIN32) && !defined(__GNUC__)
extern char *strtok(char *, const char *);
extern char *strpbrk(const char *, const char *);
extern void *memchr(const void *, int, size_t);
#endif
```

Guarded on `__GNUC__` because c2mir does not define it -- the same discriminator
the DK prelude uses -- so a real toolchain never sees a redeclaration and never
warns about the missing dllimport attribute. Declarations only, so the two
halves of an S2 split need not agree on anything here, and the emitted text is
identical on every host, which keeps the split hash stable.

`path/normalize` additionally drops `strtok` for a hand-rolled split. That is
not required by the fix above, but `strtok`'s static cursor was never
thread-safe and the loop reads no worse without it.

## Exposure before the fix

- `stdlib/path.tur` -- `path/normalize`, via `strtok`. This is the one that
  surfaced.
- `stdlib/httpd.tur` -- `strpbrk` and `memchr`. Not separately reproduced;
  worth a look alongside
  [windows-httpd-async-limit-hangs-on-ci](windows-httpd-async-limit-hangs-on-ci.md),
  which is also Windows and also httpd.
- Any user inline-C calling the three. This is the part that argues for fixing
  it in the preamble rather than at the call sites.

`src/compiler/emit_core.c` also calls `memchr`, but that is compiler-internal C
built by the host toolchain, not emitted, so it is unaffected.

## Why it took a while

Three false starts, each cheap to rule out and worth recording because they will
look attractive again:

1. **It passes under gdb.** That suggests a heap bug masked by the Windows debug
   heap. `_NO_DEBUG_HEAP=1` under gdb still passed, so it was never the heap.
2. **`interp` fails too**, which rules out generated code and points at c2mir or
   the runtime rather than the back end. Worth checking early; it is one env var.
3. **An early `strtok` probe passed** -- because it counted tokens rather than
   dereferencing them. Non-NULL is not the same as valid, and a truncated
   pointer is non-NULL.
