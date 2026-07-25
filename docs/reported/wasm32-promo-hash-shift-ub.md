# `promo_hash` shifts a 32-bit `uintptr_t` by 33 (UB on the WASM build)

**Severity:** low-medium -- undefined behaviour in the shipped web bundle;
degraded hash, not a wrong answer.

`src/turi/eval.c:9392`:

```c
static uint32_t promo_hash(const void *p) {
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}
```

This is the 64-bit MurmurHash3 finalizer applied to a `uintptr_t`. On the
native build `uintptr_t` is 64 bits and it is correct. **On wasm32 it is 32
bits**, so `x >> 33` shifts by more than the operand width -- undefined
behaviour -- and the multiply by a 64-bit constant truncates to 32 bits, so
the avalanche step does not do what it was written to do.

clang says so during the WASM build:

```
turi/eval.c:9392:12: warning: shift count >= width of type [-Wshift-count-overflow]
turi/eval.c:9392:54: warning: shift count >= width of type [-Wshift-count-overflow]
```

`turi/eval.c` is the interpreter, which IS the web REPL, so this UB is in the
shipped bundle rather than a dev-only path.

## Impact

`promo_map_*` is a pointer-keyed hash map; a poor hash costs collisions, not
correctness, and a map lookup still compares keys. The concrete risk is the UB
itself (a compiler is entitled to do anything with an over-wide shift) plus
worse-than-intended distribution on 32-bit targets.

`src/runtime/hamt.c:42` does the same `>> 33` but on a `uint64_t`, which is
correct everywhere. Only the `eval.c` site is width-dependent.

## Fix direction

Do the mixing at a fixed 64-bit width regardless of pointer size:

```c
static uint32_t promo_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}
```

Not applied here on purpose: it changes hash values and therefore the
interpreter's promo-map iteration order, which is a behaviour change in
unrelated code that deserves its own suite run rather than riding along with
an unrelated slice.

## How it surfaced

RT5a (confirm the refinement solver builds under Emscripten,
`docs/upcoming/v1/refinement-types-plan.md`). The refine sources are clean; the
warning came from the rest of the module in the same build.
