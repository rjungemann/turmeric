# `#lang r7rs`: under the prelude split on macOS, a value kept in a Turmeric map is collected

**Severity:** low while the split is off there, and it is: on macOS,
`prelude_split_applies` (src/main.c) declines unless `TUR_PRELUDE_SPLIT=1`.
macOS builds keep the one-unit build and its 3 s-per-program cost. The split
itself (docs/archive/r7rs-programs-compile-slowly.md) cuts that to about 1 s
on Linux.

## Repro

macos-latest, Apple clang, Debug `tur`. This is from the CI run on
rjungemann/turmeric#952 (`Test (macos-latest)`, `85fa7e20`), where
`r7rs-gc-seam` was the one fixture that failed:

```sh
TUR_PRELUDE_SPLIT=1 ./build/tur run tests/fixtures/r7rs-gc-seam/input.tur
```

```
-(1 2 3 "four" #(5 6))
+(16249)
 "hello world"
-(1 2 3 "four" #(5 6))
+(6534 . #<unknown>)
```

The list lives only in a Turmeric persistent map, whose HAMT nodes come from
the runtime archive (`hamt.c`). By the time it is read back, the collector
has freed it and the memory has been reused. The same fixture passes on Linux
with the split, under gcc and clang alike, and also under
`TUR_GC_TORTURE=1`. It passes on macOS as one unit.

## Root cause (not confirmed; no macOS host to test on)

The fixture exists for this exact failure: HAMT nodes allocated with libc
rather than through the collector. The fix for that is the archive's
allocator hook (`src/runtime/rt_alloc.h`). `r7gc.c` installs it from a
constructor, through a weak reference to `tur_rt_set_allocator`, at priority
101, "so the archive's first allocation already goes through us". So the
likeliest story is that the hook is not in place when the map's nodes are
allocated. Two macOS differences could cause that:

- **Constructor order.** Mach-O ignores constructor priorities across object
  files: initializers run in link order. The link line is `<program unit>
  <library object> <archives>`. The pasted collector, and its constructor,
  now live in the library object, so any constructor of the program unit
  runs before the hook is installed.
- **The weak reference.** It now sits in the library object instead of the
  one unit. ld64 binds a weak import only to a definition that some other
  reference loaded from the archive.

## Fix directions

- On a Mac, confirm first: break on `tur_rt_set_allocator` and on the HAMT's
  first `tur_rt_malloc`, split and one unit, and compare.
- If it is the order, install the hook from the program unit's startup
  (`__tur_split_lib_init` runs first in `main`, but that is after the
  constructors), or make the collector's first allocation install it lazily.
- If it is the weak reference, make it a strong one whenever the program
  links the archive. The driver knows when it does.
- Then turn the split on for macOS in `prelude_split_applies`, and let
  `tests/check-r7rs-prelude-split.sh` run there. Its Mach-O handling is
  already written.
