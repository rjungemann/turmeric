# WASM interpreter hangs on `(if x _ _)` inside `(definstance Hash [bool] ...)`

**Status:** RESOLVED 2026-07-08 -- Emscripten stack-size bump (see
"Resolution" below).

**Severity:** high — blocks the browser REPL / `Try Turmeric` from ever
finishing `turi_wasm_init` once the stdlib preload chain (fbf138669) reaches
`stdlib/typeclass-hash.tur`. User-visible symptom: `Loading Turmeric WASM
module...` overlay never dismisses; console shows the worker's factory
resolved and `_turi_wasm_init` was called, but the sync call never returns.

## Repro

Environment: `tur run web-dev`; browser at `http://localhost:3000/try/`.
Native `./build/tur repl` runs the same preload chain in ~92 ms and evaluates
`#map{}` correctly — this bug is WASM-interpreter-specific.

Minimal `stdlib/typeclass-hash.tur` for repro (rebuild wasm, reload):

```turmeric
(defclass Hash [a] (hash [x] : int))
(definstance Hash [bool] (hash [x] (if x 1 0)))
```

`_turi_wasm_init` hangs inside the elaboration of the second form.

Replacing the body with a constant loads successfully:

```turmeric
(definstance Hash [bool] (hash [x] 0))    ; loads fine
```

So the trigger is the `(if x 1 0)` body — specifically the `if` special form
inside a `definstance` method body under WASM. The instance parameter is
`:bool`, method return is `:int`.

Removing the `Hash [int]` instance from typeclass-hash.tur first, then
adding just the `Hash [bool]` form, still hangs — this is not a
multi-instance interaction.

## Bisect summary

| stdlib/typeclass-hash.tur body under test | WASM outcome |
| --- | --- |
| `(defclass Hash [a] (hash [x] : int))` + `(definstance Hash [int] (hash [x] x))` | loads |
| ...adding `(definstance Hash [bool] (hash [x] (if x 1 0)))` | **hangs** |
| Only `(definstance Hash [bool] (hash [x] (if x 1 0)))` (no Hash[int]) | **hangs** |
| Only `(definstance Hash [bool] (hash [x] 0))` | loads |

## Root cause (best guess)

Something in the `if` special form's elaboration inside a `definstance`
method body recurses without terminating under the WASM build. Possibilities
to investigate:

- Emscripten default stack (64 KB) blown by a recursion the native ~8 MB
  stack absorbs — would present as a hang if the trap loops the fiber
  scheduler instead of aborting.
- A typeclass dispatch on `:bool` (e.g. `Eq[bool]` for the `if` condition)
  that lands in a WASM-only path — but note `if` is a special form; there
  should be no dispatch on the condition.
- An `#fx` / effect-row inference that iterates until fixed point and
  diverges in the WASM interpreter.

## Fix directions

1. Inspect the code path in `src/turi/eval.c` that handles the `if` form
   inside `definstance` method elaboration; compare native vs WASM.
   `grep -n "definstance\\|F_IF\\|if_form" src/turi/eval.c` for the
   entrypoints.
2. Bump the Emscripten stack (`-sSTACK_SIZE=<n>` in the emcc link in
   `src/CMakeLists.txt` around line 823) to rule out stack overflow. Default
   is 64 KB; try 4 MB.
3. Add a recursion-depth cap to the interpreter's expression eval so hangs
   surface as `TUR-E0...` instead of stalling the browser tab forever.

## Impact

The commit that introduced this discovery (fbf138669, "Preload stdlib in the
WASM REPL and tur repl") is correct in intent — without it, `#map{...}`
literals in the browser REPL error with `unknown function or operator
'hamt-of'`. The preload chain exposes this pre-existing WASM interpreter
bug. Reverting the preload restores the old broken-`#map{}` behavior but
lets `_turi_wasm_init` return.

## Resolution

Emscripten's default stack was 64 KB; the interpreter's recursion during
`definstance` method elaboration (specifically the `if` special form path)
overflowed it. Under a pthreads WASM build the overflow presented as a hang
rather than a clean abort -- the runtime traps into a state the browser
worker can't surface.

Fix landed in `src/CMakeLists.txt`, emcc link flags:

- `-sSTACK_SIZE=16777216` (16 MB) -- primary knob for the main thread stack.
- `-sDEFAULT_PTHREAD_STACK_SIZE=8388608` (8 MB) -- pthreads spawned by
  emscripten inherit this; 8 MB matches typical native `ulimit -s`.
- `-sINITIAL_MEMORY=67108864` (64 MB) -- required to link with the larger
  stack; the link fails with `initial memory too small` otherwise.

Verified: `Try Turmeric` at `http://localhost:3000/try/` initializes and
`#map{}` evaluates. The `set.tur` OOB tracked in
[`wasm-interp-set-tur-oob-trap.md`](wasm-interp-set-tur-oob-trap.md) was
the same root cause and cleared with the same bump.

**Follow-ups worth doing** (not gating v1):

1. Right-size the numbers. 16 MB stack is generous; the actual peak depth
   during preload is likely well under 4 MB. Reducing the initial memory
   allocation shrinks the WASM cold-start footprint. Measure with
   `emscripten_stack_get_current` at the deepest known point, then tune.
2. Add a recursion-depth cap to the interpreter's expression eval so a
   future runaway surfaces as a `TUR-E0...` diagnostic instead of stalling
   the browser tab.
3. Bake the stack knobs into the wasm-spices-plan follow-ups so any
   future build target inherits them.
