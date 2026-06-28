# Verification trail: lowered-adt-fn-field-subint64-pointer-truncation

**Resolved / archived:** 2026-06-28

## What the report described

Under the `defstruct-as-defadt` lowering, a `defstruct` fn-field whose function
type has a SUB-int64 result -- `(defstruct Callback :copy [op (fn [int32] int32)])`
-- had its 64-bit function-pointer value read through a truncating `(int32_t)`
cast (the fn RESULT type's C name) before being re-widened and called, so the
call jumped to a corrupted address and segfaulted at runtime. Reproduced only
under the force-lower probe; the report flagged it as a `defstruct-as-defadt`
graduation blocker.

## Why it is now resolved

The `defstruct-as-defadt` experiment GRADUATED 2026-06-28 (every `defstruct`
now lowers to a single-variant record `defadt` unconditionally; gate in
`defstruct_lowers_to_adt`, `src/compiler/elab_structs.c`, per the note in
`src/runtime/experiments.c`). The graduation carried the field-read fix
described as "Fix direction 1" in the report: the `EX_GET_FIELD` ADT branch now
casts a `TY_FN` field through the carrier width (`int64_t`) rather than the fn
result scalar.

## How resolution was confirmed

1. Built Debug `tur` from a clean tree; full gate run
   `bash tests/run.sh` -> `summary: 1870 passed, 0 failed`.
   `fn-field-unboxed` reported `PASS`.
2. Ran the report's exact repro fixture on the **default** path (no env var):
   `./build/tur build tests/fixtures/fn-field-unboxed/input.tur` then executed
   the binary -> printed `9` then `49`, exit 0, no segfault.
3. Inspected the emitted C (`./build/tur emit-c .../input.tur`). The field read
   is now:
   ```c
   ((int32_t (*)(int32_t))(intptr_t)((int64_t)(cb_1268).op))(INT32_C(3))
   ```
   The field is declared `int64_t op;` and read back as `(int64_t)(cb).op` --
   full 64-bit width preserved before the cast to the typed function pointer.
   The truncating `(int32_t)(cb).op` cast shown in the report's Evidence section
   is gone.

Behavior, codegen, and the gate suite all agree the truncation is fixed.
