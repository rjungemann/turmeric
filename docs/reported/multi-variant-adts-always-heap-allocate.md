# Multi-variant ADTs always heap-allocate, and are never freed

**Severity:** medium, and language-wide. Not a correctness bug -- it is a
constant factor on every sum type in the tree, including `Option`, `Result`,
and every `defdata` with more than one variant. Measured at **~85% of executed
instructions** on a representative stdlib workload.

**Status:** OPEN. Diagnosed, not fixed.

## What was measured

`stdlib/logic.tur`'s substitution -- one `SBind` + one `TInt` construction and a
`logic-walk` per operation. Under `callgrind`:

```
14,579,535 (100.0%)  PROGRAM TOTALS
 9,176,082 ( 62.9%)  malloc.c:_int_malloc
 2,970,050 ( 20.4%)  malloc.c:malloc
   776,000 (  5.3%)  subst_hylookup
   368,000 (  2.5%)  logic_hywalk
```

The actual logic is under 8% of the program. Everything else is the allocator.
That `_int_malloc` (glibc's slow path) dominates rather than the tcache fast path
is the signature of an allocator that can never reuse a block.

## Cause 1 -- a multi-variant ADT is always boxed, however small

Single-variant `:copy` ADTs already lower **by value**:

```c
static tur_adt_One ctor_Only(int64_t _0) {     /* (defdata One :copy (Only :int)) */
    tur_adt_One __r;
    __r.as.Only._0 = _0;
    return __r;                                 /* no allocation */
}
```

Add a second variant and the same type heap-allocates:

```c
static int64_t ctor_A(int64_t _0) {   /* (defdata Many :copy (A :int) (B :int) (C)) */
    tur_adt_Many *__r = (tur_adt_Many *)malloc(sizeof(tur_adt_Many));
    __r->tag = 0;
    __r->as.A._0 = _0;
    return (int64_t)(intptr_t)__r;
}
```

`tur_adt_Many` is 16 bytes -- a tag and one `int64_t`. `Term` (4 variants) is 24.
Both are trivially register- or stack-passable, and both are malloc'd on every
construction. The by-value ABI already exists and is proven by the single-variant
path (the B4 by-value-carrier work); it simply stops at sums.

Note `:copy` is not the relevant knob: it controls linearity (may this value be
used twice), not representation. `Term` is already `:copy` and still boxes.

## Cause 2 -- nothing ever frees them

No `free` reachable from the hot path releases a `Term` or a `Subst`, and these
ADTs carry no refcount header, so the rc/GC subsystem does not collect them
either. Every construction leaks.

That is what turns a fixed per-op cost into a growing one. Holding the
substitution size at n=8 and varying only how many are built:

| passes | ns/op |
|---:|---:|
| 250 | 180.4 |
| 1,000 | 168.4 |
| 4,000 | 179.6 |
| 16,000 | 830.5 |
| 64,000 | 1395.9 |

Flat at ~175 ns while the heap is small, then degrading 8x as it grows. Nothing
about the work changed across those rows.

## Fix directions, in order of leverage

1. **Lower a multi-variant ADT by value when the whole value fits a small word
   budget** (2-3 words covers `Option`, `Result`, `Term`, and most stdlib sums).
   This removes the allocation rather than making it cheaper, and the ABI work is
   already done for single-variant types. Highest leverage by a wide margin.
2. **Free or arena-allocate what still boxes.** Even where boxing is unavoidable
   (wide payloads, recursive spines), a per-query arena or a drop at scope exit
   keeps the allocator on its fast path and removes the degradation above.

Either fix alone would help; (1) makes (2) matter much less.

## Method note

Instruction counts come from callgrind, which counts under valgrind rather than
cycles on real hardware, so the wall-clock scaling table above is included as an
independent confirmation -- the two agree.

An earlier attempt to isolate this with a microbenchmark measured nothing: a
tail-recursive loop whose result went unused folded away entirely, reporting
0 ns for 200,000 ADT constructions. That is the same trap SX0(a)'s closure
baseline hit. The measurements here avoid it by consuming every result and by
profiling a program whose output is checked.
