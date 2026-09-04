# `promo_hash` shifts a 32-bit `uintptr_t` by 33 (UB on the WASM build)

**Severity:** low-medium -- undefined behaviour in the shipped web bundle;
degraded hash, not a wrong answer.

**Status:** RESOLVED (2026-07-26). Fix applied at `src/turi/eval.c:9390`; see
[Resolution](#resolution) below. The "deserves its own suite run" caveat in the
fix direction turned out to overstate the blast radius -- the change is a no-op
on every 64-bit target.

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

## Resolution

Applied exactly as written in the fix direction, plus a comment recording why
the width is pinned so the next reader does not "simplify" it back to
`uintptr_t`.

The caution attached to the fix direction was stronger than the change warrants,
for two reasons found while applying it:

**1. It is bit-identical on LP64.** Where `uintptr_t` is already 64 bits, the
rewritten expression is the same arithmetic on the same type -- the cast
`(uint64_t)(uintptr_t)p` is a no-op widening. Native hash values, and therefore
native promo-map layout, do not move at all. Only wasm32 (and any other 32-bit
target) sees a different hash, which is the entire point: there the old values
came out of an over-wide shift.

**2. There is no observable iteration order to change.** `PromoMap` is used
only as a pointer-keyed seen-set / forwarding table by `promo_check*` and
`promo_copy*` (`src/turi/eval.c:9450`-`9641`) -- every access is `promo_map_get`
/ `promo_map_put` on a specific key. Nothing walks `m->slots` except
`promo_map_grow`, which rehashes into a fresh table. Slot order is not visible
to any caller, so a different hash costs at most a different collision pattern.

### Verification

The over-wide shift is gone on wasm32. Compiling both forms side by side with
`clang --target=wasm32 -Wshift-count-overflow` reports both warnings against
the old body and none against the new one:

```
h.c:4:12: warning: shift count >= width of type [-Wshift-count-overflow]
h.c:4:54: warning: shift count >= width of type [-Wshift-count-overflow]
2 warnings generated.          <- line 4 is old_hash; new_hash (line 10) is clean
```

(Checked against an isolated repro rather than the full translation unit -- this
container has clang's wasm32 target but no wasm sysroot, so `eval.c` itself
cannot be preprocessed here. The warning is a property of the two shift
expressions alone, which the repro reproduces verbatim.)

Native: Debug build clean, `bash tests/run.sh` **2359 passed, 0 failed**.

## How it surfaced

RT5a (confirm the refinement solver builds under Emscripten,
`docs/archive/refinement-types-plan.md`). The refine sources are clean; the
warning came from the rest of the module in the same build.
