# Fix: capturing closures in fn-typed fields -- boxing widened, force paths agree, thin fields reject

Resolves cases 1 and 2 of
[closure-in-defdata-field](../closure-in-defdata-field.md) (case 3 was fixed
earlier, same day). Picked up when SX2's driver work needed CPS closures and
the open crash sat directly on that path.

## What was actually wrong -- two staleness bugs, not a representation hole

The fat-closure machinery for fn-typed fields already existed end to end:
`resolve_ctor_field` boxes a concrete `(fn ...)` field, the ctor-arg loop
shims thin fns into fat handles, drop glue frees the box, and the emit side
dispatches boxed calls through `TUR_APPLY<N>_T`. It failed in exactly two
places where one site's assumptions had drifted from another's:

1. **Arity 0 was excluded from boxing** (`resolve_ctor_field`, bound
   `1..4`), on the recorded reasoning that the fat dispatch "covers N<=4" and
   the store shim "shims arity >=1". `TUR_APPLY0_T` exists in the preamble
   and the dispatch emission is generic over N -- the exclusion was stale
   conservatism. A THUNK field is the lazy-stream shape the original report
   was filed about, so the excluded case was the motivating one.

2. **The match-arm extraction consulted a different fact than the store.**
   `elab_structs.c` marked a match binding `is_fat` only for TYVAR fields;
   its comment still said "a concrete (non-tyvar) TY_FN field is NOT
   auto-boxed at construction, so it stays thin" -- which stopped being true
   the day `resolve_ctor_field` started boxing concrete fields. Result: for a
   `defdata`, construction stored a fat env block and the match arm called it
   as thin code. This is why the matrix looked baffling -- defstruct
   field-reads worked at arity 1..4 while defdata crashed at every arity: the
   two FORCE paths disagreed, not the two containers.

The emitted C made the disagreement visible in one screen: `delay-cons`
building `__env` and handing it to `ctor_SInc`, and `force-one` casting the
same value to `int64_t (*)(int64_t)` and calling it.

## What landed

- `resolve_ctor_field`: boxing bound `1..4` -> `0..4`; the store shim's bound
  admits arity 0.
- Match-arm `is_fat`: also set when the field's `full_type` is a boxed
  `TY_FN`.
- **Thin fields reject capturing stores.** A bare `:fn` field has no
  signature to box, and a >4-arity field has no `TUR_APPLY<N>_T`; nothing
  records which representation such a slot holds, so the crash cannot be
  fixed representationally. Storing a capturing closure into one is now a
  diagnostic at the ctor-arg seam, naming both working routes (a spelled-out
  signature of arity <= 4, or the `defopaque :ptr<void>` carrier). Captureless
  lambdas keep working -- every in-tree `:fn` field stores exactly those.
- **The warnings are gone at the same seam.** `field_is_carrier` (the
  pointer-to-carrier cast at ctor args) now also holds for a boxed fn field
  and for a thin `:fn` slot, with an already-cast guard so the two fixtures
  whose args were cast upstream do not get a redundant double wrap.
- The stale `types.h` comment ("B-0 only plumbs the bit; nothing sets it true
  yet") now reflects reality (B-0..B-4 shipped; set at ~10 sites).

## Verification

- `closure-field-boxed-all-shapes`: all four container/arity shapes that used
  to SIGSEGV -- defdata thunk, defdata arity-1, defstruct thunk, defstruct
  arity-1 -- each storing a CAPTURING closure and forcing it. Clean compile,
  right answers.
- `errors/fn-field-capturing-closure-rejected`: the bare-`:fn` diagnostic.
- Case 2's probe (captureless into bare `:fn`): compiles warning-free, prints
  42.
- Suite: **2700 passed, 0 failed** (2698 baseline + the two new fixtures).
  **Zero snapshot drift** -- an intermediate version of the cast produced a
  redundant double wrap in two snapshots, caught by the drift check and
  suppressed rather than regenerated around.

## What this unblocks

The SX2 depth-first driver probe that triggered this detour: CPS goal
combinators (fat closures taking fat closures) work, and now ADT-held thunks
do too, so both driver designs are open. `stdlib/logic.tur`'s `Goal` could
also migrate from its `defopaque :ptr<void>` workaround to a spelled-out
field type, though nothing forces that.
