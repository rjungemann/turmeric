# ADT allocation ceiling harness

Prices the fixes proposed in
[docs/reported/multi-variant-adts-always-heap-allocate.md](../../docs/reported/multi-variant-adts-always-heap-allocate.md)
before anyone spends compiler time on them: five representations of the same
workload (build an n-binding substitution, walk all n, discard), so the two
proposed fixes can be costed separately and together.

```sh
gcc -O2 -D_POSIX_C_SOURCE=200809L -o /tmp/ceiling benchmarks/adt-alloc/ceiling.c -std=c99
for rep in A B C D E; do /tmp/ceiling $rep 16000; done
```

`A` is what the compiler emits today. Output is `rep,passes,ns_per_op,checksum`;
the checksum must match across all five, which is what keeps a representation
from looking fast by doing less.

**Run each representation in its own process, as the loop above does.** A, C and
E leak by design -- that is the thing being modelled -- so running them in
sequence lets whichever goes first hand its heap to the rest. Doing that
produced a fake 8x "degradation" that took a rewrite of the report to undo.
