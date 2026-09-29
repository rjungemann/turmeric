# `#lang r7rs`: under the prelude split on macOS, a value kept in a Turmeric map is collected

**RESOLVED 2026-09-28.** Root cause confirmed on a Mac: it was the constructor
order, the first of the two guesses below. Mach-O ignores a constructor's
priority **across** object files -- ld64 runs initializers in link order -- so
the collector's `constructor(101)` in the library unit ran *after* the program
unit's `__tur_static_init_ctor`, which allocates archive memory. The allocator
hook is now installed from `__tur_static_init` as well, which is ahead of every
band on every platform, and the split is **on by default on macOS**.
`tests/check-r7rs-prelude-split.sh` runs there too.

**Severity when open:** low, because the split was off there:
`prelude_split_applies` (src/main.c) declined unless `TUR_PRELUDE_SPLIT=1`, so
macOS builds kept the one-unit build and its 3 s-per-program cost. The split
itself (docs/archive/r7rs-programs-compile-slowly.md) cuts that to about 1 s.

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

## Root cause -- CONFIRMED 2026-09-28 (the first guess below)

Measured on macOS 27.0 / Apple clang 21, Debug `tur`, with the fixture's own
two units taken off the `TUR_SHOW_CC` link line.

**1. The weak reference was not the problem.** `tur_rt_set_allocator` binds:
`nm -m` on the linked program shows it `external` in `__TEXT,__text`, resolved
out of the archive. `hamt.o` references `tur_rt_malloc`, which loads
`rt_alloc.o`, which defines the setter -- so ld64 has a definition for the weak
import by the time it looks.

**2. The collector's state is not forked either.** The split renames the three
mutable file-scope statics in `r7gc.c` and defines them once:
`_tur_sl_tur_gc_G` and `_tur_sl_tur_gc_threaded` are common symbols in the
library object and undefined in the program unit, `_tur_sl_tur_gc_self` is its
TLS. Both units' pasted copies of `tur_gc_malloc` work the same heap.

**3. It is the constructor order.** The program is linked
`<program unit>.c <library object>.o -lturt_runtime`, and the two objects carry
exactly one initializer each. Decoding `__init_offsets` in the linked binary:

```
init offsets: 0x1874 0x4e8c
0000000100001874 t ___tur_static_init_ctor     <- program unit, no priority
0000000100004e8c t _tur_gc_ctor                <- library unit, constructor(101)
```

`__tur_static_init_ctor` runs **first**, priority 101 notwithstanding: Mach-O
has no cross-object initializer priority, so link order decides. Two
breakpoints show what it costs, both before `tur_gc_ctor`:

```
tur_rt_calloc(n=64, m=8)
  sym_tab_grow at symbols.c:48
  tur_sym_register at symbols.c:74
  __tur_static_init + 216

tur_rt_malloc(n=64)
  hamt_malloc at hamt.c:148
  hamt_alloc_empty at hamt.c:175
  tur_hamt_new at hamt.c:923
  __tur_static_init + 712
```

So the symbol table and an empty HAMT were libc blocks the collector could not
trace through, and a value reachable only through one of them was freed under
it. Flipping the two objects on the link line -- nothing else -- made the
fixture pass, which isolates the variable to initializer order.

## The guesses this report was filed with, as filed

The first was right, the second was not. Kept for the record, and because
the reasoning in between is what the measurements above answer.

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

## The fix

Link order was not made the answer, because then correctness would rest on it.
Instead the ordering dependency is removed: the install is factored out of the
constructor and also called where nothing can precede it.

- `src/runtime/r7gc.c` -- `tur_gc_install_rt_allocator()`, idempotent, called
  by `tur_gc_ctor` as before. Safe before `tur_gc_init`, because
  `tur_gc_malloc` initializes the collector on its first call. A no-op stub in
  the `!TUR_GC_ON` arm, where the entry points already *are* libc. Its
  allocator table is filled field by field into a plain local, not built as a
  `static const` -- see "the trap that cost a CI round" below.
- `src/compiler/emit_core.c` (`static_init_emit`) -- emits that call as the
  first statement of `__tur_static_init`, behind its idempotent guard and ahead
  of the atexit band and `__tur_split_lib_init`. Gated on a new
  `gc_collector_pasted` argument: the program emitter passes
  `r7rs_gc_active(false)`, the separate-compilation emitter passes `false`
  because it never pastes the collector.
- `src/main.c` -- `prelude_split_applies` no longer excludes `__APPLE__`.
- `tests/check-r7rs-prelude-split.sh` -- runs on Darwin.

Where the collector compiles out the call reaches the no-op stub and costs
nothing: Windows, and `tur jit`, whose `TUR_JIT_ENGINE` clears `TUR_GC_ON`
because a JIT'd program's globals do not live in the data segment the root scan
walks.

## What it took to make the check script's Mach-O arm work

Its duplicate-state check had a Mach-O branch written in advance and never
executed. It fails on Darwin as written, with nothing wrong: `nm`'s one-letter
class separates writable data from read-only on ELF (`R`/`r` being rodata) but
**not** on Mach-O, where every non-text local is `s` whatever section it is in.
The collector pasted into both units has a read-only static
(`tur_gc_class_size`), and it was reported as forked state. `defined_data` now
asks `nm -m` for the section instead and takes only the writable ones --
`__DATA,__data`, `__bss`, `__common`, plus `__thread_vars` / `__thread_bss`.

## The trap that cost a CI round, one section over

The first version of the fix wrote the allocator table the way `tur_gc_ctor`
already did, as a function-scope `static const tur_gc_rt_allocator ours`. That
passed everything locally and **failed `tur_r7rs_prelude_split` on
ubuntu-latest**, reporting `ours` as state defined in both units.

It is the same blind spot as above, on the other platform. A const table of
function POINTERS needs relocating, so ELF places it in `.data.rel.ro` --
read-only after load, but `nm` classes it `d`, exactly like `.data`. It had
never shown up before because in the client unit `tur_gc_ctor` is renamed
`unused` and never called, so the compiler dropped the function and its static
with it; `tur_gc_install_rt_allocator` is called in both units, so the table
existed in both.

The table is now filled field by field into a plain local. `tur_rt_set_allocator`
copies it, so it never needed to outlive the call, and four stores to stack
slots leave no data object to misclassify -- on either platform. Verified: no
`ours` symbol of any kind in either unit.

An aggregate initializer was not enough on its own. Written as
`const tur_gc_rt_allocator ours = { ... }`, clang still emitted a read-only
template (`l___const.tur_gc_install_rt_allocator.ours` in `__DATA,__const`);
that particular spelling is assembler-local and would have passed both arms,
but it is the compiler's choice, not something to depend on.

## Verified

On macOS 27.0 / Apple clang 21 (arm64), Debug `tur`, `-DTUR_JIT=ON`:

- `r7rs-gc-seam` under the split, and again under `TUR_GC_TORTURE=1`.
- `bash tests/check-r7rs-prelude-split.sh` -> `PASS`, all seven of its
  programs, on Darwin for the first time.
- `bash tests/run.sh` -> `3351 passed, 0 failed` with the split on by default,
  and again with `TUR_FORCE=1` so nothing came from the stamp cache.
- `tur run regen-snapshots --check` -> 155 up to date (no snapshot fixture
  pastes the collector, so the new line moves none of them).
- `bash tests/run-fmt.sh` -> 34 passed, `bash tests/check-reported-index.sh`
  -> PASS.
- `bash tests/run-jit.sh` -> 3235 passed, 0 failed, 55 skipped. (Under
  `tur jit` the new call is the no-op stub; the run is a regression check on
  the emitter change, not on the collector.)
