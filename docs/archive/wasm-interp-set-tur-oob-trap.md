# `(load "stdlib/set.tur")` traps with `RuntimeError: index out of bounds` under WASM

**Status:** RESOLVED 2026-07-08 -- same root cause as
[wasm-interp-hang-if-in-definstance-bool.md](wasm-interp-hang-if-in-definstance-bool.md);
the Emscripten stack bump cleared this trap too. Not a real memory-safety
bug, a stack overflow presenting as an OOB access.

**Severity:** medium — surfaces once the browser REPL's stdlib preload
reaches `set.tur`. If [[wasm-interp-hang-if-in-definstance-bool]] is fixed
first, `set.tur` becomes the next blocker.

## Repro

Environment: `tur run web-dev`; browser at `http://localhost:3000/try/`.

With a minimal `stdlib/typeclass-hash.tur` that omits `Hash[bool]`,
`Hash[cstr]`, `Hash[float32]`, `Hash[float]` (leaving only `defclass Hash`
+ `Hash[int]`), the preload advances past `typeclass-hash.tur` but throws
during elaboration of `stdlib/set.tur`:

```
[preload] > set.tur
Uncaught RuntimeError: index out of bounds
```

The trap comes out of the WASM module and rejects the factory promise —
`_turi_wasm_init` never returns 0. Same file loads in ~ms under native
`./build/tur repl`.

## Likely trigger

`stdlib/set.tur` transitively `(load "stdlib/typeclass-hash.tur")` and then
elaborates `(defstruct Set :heap [A] (hamt :ptr<void>))` plus its typeclass
instances. When the required `Hash [cstr]` / `Hash [float]` instances are
missing (bisect scenario above), the interpreter appears to dispatch into an
unbound method slot and dereference an out-of-range table index. In the
production preload chain, the instances are present — so the OOB may
require the exact bisect setup to reproduce, or it may lurk behind the
[[wasm-interp-hang-if-in-definstance-bool]] hang and only reveal itself
once that hang is fixed.

## Fix directions

1. Add bounds guards around the typeclass method-dispatch table lookup in
   the interpreter (search `elab_dict`, `resolve_instance`, or similar in
   `src/turi/eval.c` and `src/compiler/elab_call.c`).
2. Ensure the WASM build has the same debug-abort behavior as native so
   the OOB surfaces a Turmeric diagnostic instead of a bare WASM trap.
3. Reproduce natively with the same stripped `typeclass-hash.tur` to
   confirm whether the trap is universal or WASM-only.

## Impact

Second wall in the `Try Turmeric` browser REPL path after the `if`-in-
`definstance` hang. Both need to be resolved before the preload chain from
fbf138669 can complete under WASM.
