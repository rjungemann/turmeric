---
title: make-struct codegen lowers struct to int64_t when a struct type parameter does not appear directly as a field type
category: Codegen bug
severity: Medium. Blocks ergonomic construction of phantom-indexed structs (`(defstruct World [m A B] (pos (Dense m A)) (vel (Dense m B)))`). Reproduces without -Xsized-types, so it predates the SZ8 work; surfaced when writing the P2 fixture for docs/reported/ecs-e2c-sized-dense-needs-bounded-world.md.
description: A defstruct whose type parameter `m` is used only *indirectly* in field types -- threaded through a type application like `(Dense m A)` rather than as a bare field type -- gets mis-lowered at make-struct time. The emitted C names the binding's type correctly (`World__int__int__int w;`) but produces the RHS as a designated initializer cast to `int64_t`: `(int64_t){.pos = ..., .vel = ...}`. The C compiler then rejects the designated initializer on a non-aggregate. The same struct compiles cleanly when every type parameter appears directly as a field type (e.g. `(defstruct PairZ [A B] (fst A) (snd B))`), so the bug is specific to "phantom-via-type-app" type parameters.
status: OPEN. Pre-existing codegen bug; not introduced by SZ8 work. Recommended next step: in the make-struct C emit path (src/compiler/emit*.c), key the RHS C type off the struct def directly rather than off the type-args result kind.
---

# make-struct emits `(int64_t){.field = ...}` for a struct with a phantom type parameter

## Minimal repro

```turmeric
(defopaque Dense [n A] :int)
(defstruct PairD [m A] (fst (Dense m A)) (snd (Dense m A)))
(defn dn [n A] [] : (Dense n A) (:: 0 :Dense))
(defn main [] : int
  (let [p (make-struct PairD (dn) (dn))]
    0))
```

`./build/tur run` (no flags needed):

```
.../tests_fixtures_*.c:NN:29: error: initialization of non-aggregate type
'int64_t' (aka 'long long') with a designated initializer list
    int64_t p_NNN = (int64_t){.fst = dn(), .snd = dn()};
```

Reproduces with or without `-Xsized-types`.

## What's specific to the failure

- A defstruct with all type parameters appearing directly as field
  types compiles fine: `(defstruct PairZ [A B] (fst A) (snd B))`.
- The bug fires when a type parameter is used *inside* a field's type
  application, like `m` inside `(Dense m A)`. From the struct's point
  of view, the bare `m` never appears as a field type -- only the
  applied `(Dense m A)` does.
- The binding's C type is correctly named
  (`PairD__int__int p_NNN = ...`) but the initializer is cast to
  `int64_t`, not `PairD__int__int`.

The same defstruct type-checks correctly through the elaborator and
the projection `(.fst p)` returns the right substituted type; the bug
is strictly at the make-struct C emit step.

## Hypothesised root cause

The make-struct emitter likely derives the initializer cast from a
result-kind path that collapses to `TY_INT` when all field carriers
are `:int` (Dense is int-carried via the defopaque). The check should
instead key off the struct def's C type name, which the emitter
already computes correctly for the binding (`PairD__int__int`).

A grep for `(int64_t){\\.` in `src/compiler/emit*.c` should find the
offending format string; the fix is to thread the struct's mangled C
type name (or pull it from the binding's declared type) at the cast
site instead of using the result-kind shortcut.

## Workaround

Avoid constructing such structs until the fix lands. Type-only
operations on them (projection inside an uncalled defn, signature
threading) work fine -- e.g. the P2 accept fixture
`tests/fixtures/sized-struct-field-share-accept` exercises projection
and signature threading without ever calling `make-struct`.

## Validation of a fix

- The minimal repro above compiles and runs.
- The P2 accept fixture
  (`tests/fixtures/sized-struct-field-share-accept`) is extended to
  actually construct a `World` value (currently elided to dodge this
  bug).
- No existing fixture using `defstruct` regresses.

## Related

- [sz8-projection-size-recovery-gap.md](sz8-projection-size-recovery-gap.md)
  (the SZ8 follow-up that prompted this report)
- [ecs-e2c-sized-dense-needs-bounded-world.md](ecs-e2c-sized-dense-needs-bounded-world.md)
  (P2 -- the parent doc whose fixture surfaced this bug)
