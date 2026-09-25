# A collector for compiled `#lang r7rs` programs (`--enable=r7rs-gc`)

Status: **prototype, behind `--enable=r7rs-gc`** (EXPERIMENTS row `r7rs-gc`,
introduced 0.52.0). Answers
[r7rs-heap-data-never-reclaimed](../reported/r7rs-heap-data-never-reclaimed.md)
for the compiled back end, and most of
[r7rs-callcc-memory-never-freed](../reported/r7rs-callcc-memory-never-freed.md).
Both reports stay open until this graduates.

## 1. The problem

A Scheme value is a `:heap` box, and the memory model never frees one
(docs/guides/gc-guide.md: "a bare non-`rc<T>` box is never auto-freed on
*any* path"). A Scheme program's data is shared, mutable and cyclic, and the
language gives the programmer no way to say who owns it. So a long-running
Scheme program's memory only grows. A loop that builds a dead four-element
list a million times peaked at 429 MB (r7rs-lang-plan T8).

Reference counting plus the Bacon-Rajan cycle collector (the `rc<T>` path) was
the other candidate. It would need a retain and a release at every copy of an
`any` word in emitted code: in every dialect's dynamic path, in the prelude's
inline C, in the DK runtime. A tracing collector needs none of that.

## 2. The design

**Conservative mark-sweep, for one program's own allocations.**

- **The heap.** `src/runtime/r7gc.c`: 64 KiB chunks from `mmap`, each holding
  one size class (36 classes, 16 bytes to 32 KiB), and larger objects as
  chunk-aligned runs. A hash table maps each chunk to its descriptor, so any
  address -- including a pointer INTO an object, or one with a low tag bit --
  resolves to the object's start. All metadata lives in `mmap`'d memory, never
  in the data segment the collector scans.
- **How a program uses it.** Under the flag, the emitter pastes `r7gc.c` into
  the program's translation unit ahead of the preamble proper, then redirects
  that unit's allocator with object-like macros: `malloc`, `calloc`,
  `realloc`, `free`, `strdup`, `strndup`, and the region allocator's malloc
  fallback and its free (`emit_r7rs_gc_prologue`, emit_module.c). Everything
  the program allocates -- Scheme data, closure environments, DK frames,
  continuation images, vector buffers, the prelude's C -- is on the heap.
  Region runtime sources pasted into the same unit are compiled outside the
  macros (their tables are libc's).
- **Roots**, scanned word by word:
  - the C stack, from the collector's frame to the thread's stack base, with
    the callee-saved registers spilled into a `jmp_buf` on it;
  - the executable's data and bss (`__data_start` to `_end`). The emitted
    runtime keeps its per-thread state in `TUR_THREAD_LOCAL` variables, which
    the flag turns into plain statics, so they are in this range;
  - the used bytes of every live and retired region generation
    (`tur_region_each_used`, region.c), for a Turmeric caller that builds
    nodes in a bracket.
- **Marking and sweeping.** Objects are scanned whole, conservatively. The
  sweep rebuilds each class's free list; a large object's mapping is
  released.
- **When it runs.** When the bytes allocated since the last collection pass a
  threshold: 8 MiB, or twice the live size after the last collection, whichever
  is larger.
- **`free` still works.** An explicit `free` of a heap object returns it at
  once; a pointer the collector does not own goes to libc.
- **Continuations.** A `call/cc` image is now allocated after the macros
  (`r7k_measure`, prelude), so it is a heap object and scanned: a continuation
  keeps everything its stack points at, and is itself reclaimed once nothing
  refers to it. The DK frames T5 pins are reclaimed the same way.

## 3. Measured (2026-09-25, Linux x86-64, gcc 13, default `-O2`)

| program | without | with `--enable=r7rs-gc` |
|---|---|---|
| a dead 4-element list, 10^6 times | 429 MB, 0.39 s | 10 MB, 0.23 s |
| 100,000 escaping `call/cc`s | 527 MB, 0.40 s | 10 MB, 0.13 s |
| a `delay-force` stream 10^6 deep | 108 MB | 10 MB |
| `(map + a b)` over 10^6-element lists (live data) | 590 MB, 0.75 s | 203 MB, 0.89 s |
| `read` of a 10^6-element list | 175 MB, 0.52 s | 75 MB, 0.94 s |

Garbage-heavy programs get faster, since they stop touching fresh memory.
Programs that keep a large live set pay for re-marking it: about 1.2x-1.8x
here.

**Correctness.**
- Every `#lang r7rs` fixture passes built with the flag and run with
  `TUR_GC_TORTURE=1`, a collection on EVERY allocation, except
  `r7rs-write-labels`, which is quadratic that way and passes at every 50th.
  Continuations, `eval`, the ports and the numeric tower are all among them.
- chibi's R7RS suite: 1223 passed, 2 settled, 0 failed with the flag, and the
  same under `TUR_GC_TORTURE=50`.
- The same fixtures pass built with ASan and UBSan as well
  (`TUR_GC_TORTURE=20`).
- `region-escape-via-callcc` (a Turmeric entry, a region, a Scheme library,
  `call/cc`) passes with the flag, under torture, and with `TUR_REGIONS=0`.
- The instrument: a C test of the collector on its own built two million list
  nodes in a 10 MB process.

**The gate.** `tests/run-r7rs-gc.sh` (ctest `tur_r7rs_gc`) builds every Scheme
fixture with the flag and runs it with a collection every 31 allocations
(`R7RS_GC_TORTURE=1` for the deep run). It also runs a churn loop under a
256 MiB address-space limit, which must fit with the collector and must NOT fit
without it. `tests/fixtures/r7rs-gc-basic` runs with the flag in the ordinary
suite.

## 4. Limits

A conservative collector's one real failure is a **missing root**: memory it
does not scan holding the only pointer to one of its objects. What it does not
scan:

- **Memory the runtime archive or libc allocate.** An `rc<T>` block, a HAMT
  node (`stdlib/map`), a buffer libc keeps. A pure Scheme program puts no
  pointer to its data there. A Scheme program that stores Scheme values in a
  Turmeric `(Map K V)` through the `(turmeric ...)` seam does, and those values
  can be freed while the map still holds them. The first fix is to route the
  archive's HAMT and rc allocations through a hook the collector sets.
- **Threads.** The collector stops no other thread, and TLS becomes static
  storage under the flag. A program that starts threads (Turmeric
  concurrency through the seam) must not use it.
- **Other translation units.** Only a single-unit build uses it. A `--shared`
  or project build ignores the flag (each unit would have its own heap).
- **The interpreter.** `tur --interpret` keeps its values for the life of the
  process by design (gc-guide); the flag changes nothing there.
- **The JIT.** Under `tur jit` the collector compiles to plain libc: a
  JIT'd program's globals live in memory MIR allocates, not in the data
  segment the root scan reads (`TUR_JIT_ENGINE`, set by the engine's prelude).
- **Hoisted inline C** that allocates at file scope runs before the macros,
  so its memory is libc's and not scanned. The prelude's own `call/cc` helper
  was moved for this reason; a user's inline C would have to be too.
- **Linux/glibc only.** Elsewhere the entry points are plain libc and nothing
  is collected. macOS needs its data-segment bounds (`getsectiondata`) and
  stack base (`pthread_get_stackaddr_np`), which T5 already uses.
- Conservatism retains a little: a stale stack word or an integer that happens
  to look like a heap address keeps its object alive.

## 5. Graduation

- The archive-allocation hook, so a Scheme program may keep Scheme values in
  Turmeric maps and `rc<T>` cells.
- macOS.
- A decision about threads: refuse the flag in a program that spawns them, or
  stop the world.
- Default on for `#lang r7rs` compiled programs, and the two reports archived.
