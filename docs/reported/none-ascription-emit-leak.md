---
title: `(none) : (Option T)` type-ascription leaks ~88 bytes in the emit path
category: Emit / memory -- compiler-side leak (ASan)
severity: Low. Compiler-process leak only (88 bytes, process-lifetime), not a
  miscompile and not in the generated program. The default leak-checked
  `tur build` / `tur emit-c` of a program that ascribes a bare `(none)` to a
  concrete `(Option T)` fails under LSan, but no current fixture exercises the
  pattern so `bash tests/run.sh` stays green. Found incidentally while fixing
  docs/archive/constrained-instance-element-dispatch.md.
status: OPEN
---

# `(none) : (Option T)` ascription leaks in the emit path

## One-line summary

Emitting a program that ascribes a bare `(none)` to a concrete `(Option T)`
(e.g. `((none) : (Option int))`) leaks ~88 bytes (a `buf.c` grow + a
`tur_strdup`) in the compiler. Independent of typeclasses.

## Minimal repro

```turmeric
(defn main [] : int
  (let [x ((none) : (Option int))]
    (if (.is-some x) 1 0)))
```

```
$ ./build/tur emit-c /tmp/r6.tur >/dev/null
==...==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 64 byte(s) in grow (src/runtime/buf.c:9)
Direct leak of 24 byte(s) in tur_strdup (src/runtime/buf.c:85)
SUMMARY: AddressSanitizer: 88 byte(s) leaked in 2 allocation(s).
```

Both `emit-c` and `build` reproduce it; exit code is 1 (LSan failure). A `(some
...)` value, or a `(none)` whose type is inferred from a sibling `(some ...)`,
does not leak -- it is the explicit ascription on a bare `(none)` that triggers
it.

## Root cause (not yet pinned)

ASan only surfaces the allocation leaf (`buf.c` grow / `tur_strdup`); the
deeper frames are inlined out in the Debug build's release-ish opt level. The
allocation is a transient string buffer built while lowering the ascribed
`none` construction (the monomorphized `none__spec__Option__int` clone name /
its env), dropped without being freed. Pin with a `-O0 -fno-inline` build to
recover the calling frame.

## Fix directions

Locate the `Buf`/`strdup` minted on the `(none)`-ascription emit path (the
`#{Construct}` lowering for `none` ascribed to a concrete `(Option T)`) and
free it after use, mirroring the `free_struct_app_type` / `free(...)` cleanup
the neighboring construct paths already perform.
