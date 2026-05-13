# Numeric Types — Implementation Plan for Turmeric

> **Status:** Planned — Post-Phase-19  
> **Prerequisite:** Phase 15 complete (typeclasses, including `Num`/`Eq`/`Ord`/`Show` instances for numeric types)  
> **Related:** [turmeric-plan.md](turmeric-plan.md), [deferred-tasks-phase15-phase19.md](deferred-tasks-phase15-phase19.md)

---

## Executive Summary

Turmeric v1 uses `int64_t` for all integers and `double` for all floats. This plan introduces the full suite of fixed-width integer and float types familiar from C and Rust — `int8` through `uint64` and `float32` — as first-class types in the Turmeric type system. Each new type maps directly to its C counterpart, participates in the existing typeclass hierarchy, and carries its own literal syntax.

---

## Current State

| Turmeric type | C type     | Notes                                    |
|---------------|------------|------------------------------------------|
| `int`         | `int64_t`  | Only integer type in v1                  |
| `float`       | `double`   | 64-bit; only floating-point type in v1   |
| `bool`        | `bool`     | `<stdbool.h>`                            |
| `cstr`        | `const char *` | Borrowed string literal              |

---

## Target Type Roster

### Signed integers

| Turmeric type | C type     | Bits | Range                                         |
|---------------|------------|------|-----------------------------------------------|
| `int8`        | `int8_t`   |  8   | −128 … 127                                    |
| `int16`       | `int16_t`  | 16   | −32 768 … 32 767                              |
| `int32`       | `int32_t`  | 32   | −2 147 483 648 … 2 147 483 647                |
| `int64`       | `int64_t`  | 64   | −9 223 372 036 854 775 808 … 9 223 372 036 854 775 807 |
| `int`         | `int64_t`  | 64   | Alias for `int64`; kept for backwards compat  |

### Unsigned integers

| Turmeric type | C type     | Bits | Range                          |
|---------------|------------|------|--------------------------------|
| `uint8`       | `uint8_t`  |  8   | 0 … 255                        |
| `uint16`      | `uint16_t` | 16   | 0 … 65 535                     |
| `uint32`      | `uint32_t` | 32   | 0 … 4 294 967 295              |
| `uint64`      | `uint64_t` | 64   | 0 … 18 446 744 073 709 551 615 |

### Floating point

| Turmeric type | C type  | Bits | Notes                                        |
|---------------|---------|------|----------------------------------------------|
| `float32`     | `float` | 32   | IEEE 754 single-precision                    |
| `float`       | `double`| 64   | Alias for `float64`; kept for backwards compat |
| `float64`     | `double`| 64   | IEEE 754 double-precision                    |

---

## Design Decisions

### Decision 1: Literal Syntax

**Options:**

- (a) Type-suffix literals: `42u8`, `255u32`, `3.14f32`
- (b) Explicit cast calls: `(as int8 42)`, `(as float32 3.14)`
- (c) Both: suffix syntax desugars to `as` form

**Decision:** (c) — suffix literals desugar to `as` casts. Suffixes are the ergonomic
surface; `as` casts are the canonical elaborated form and are also available directly.

**Suffix table:**

| Suffix | Type    |
|--------|---------|
| `i8`   | `int8`  |
| `i16`  | `int16` |
| `i32`  | `int32` |
| `i64` / *(none)* | `int` / `int64` |
| `u8`   | `uint8` |
| `u16`  | `uint16`|
| `u32`  | `uint32`|
| `u64`  | `uint64`|
| `f32`  | `float32`|
| `f64` / *(none)* | `float` / `float64` |

Unsuffixed integer literals default to `int` (`int64`) and unsuffixed float
literals default to `float` (`float64`) — preserving all existing code.

### Decision 2: Overflow Semantics

**Options:**

- (a) Wrap (C default; silent; matches C `int8_t` wrap-around)
- (b) Checked (abort on overflow; safe but slow)
- (c) Saturating (clamp to min/max; useful for DSP/graphics)
- (d) Wrapping by default, opt-in checked/saturating via stdlib wrappers

**Decision:** (d) — wrap by default (same as C, zero runtime cost), with
`stdlib/checked.tur` providing `checked-add`, `checked-sub`, etc. returning
`option<T>`, and `stdlib/saturating.tur` providing `sat-add`, `sat-sub`, etc.

### Decision 3: Implicit vs. Explicit Coercion

**Options:**

- (a) Implicit widening (like C; `int8` auto-promotes to `int32` in expressions)
- (b) All coercions explicit via `as` — no implicit numeric conversions
- (c) Implicit widening only within the same signedness class

**Decision:** (b) — all conversions are explicit `as` casts. This avoids silent
precision loss and matches the design philosophy of Turmeric's borrow checker
(explicit over implicit). The elaborator emits a diagnostic suggesting `(as T x)`
whenever a numeric type mismatch is detected.

### Decision 4: `int` / `float` Aliases

`int` remains a valid alias for `int64` and `float` remains a valid alias for
`float64`. All existing code compiles without change. The alias is resolved at
parse time; there is no separate `TY_INT_ALIAS` kind.

---

## Implementation Phases

### Phase N0: Type System

**Goal:** Add new `TypeKind` variants and hook them into `type_c_name`, `type_name`,
and copy/move semantics.

**Exit Criterion:** All new `TY_*` variants resolve to their C names without error.

#### `src/types.h`
- [ ] Add `TY_INT8`, `TY_INT16`, `TY_INT32`, `TY_INT64` to `TypeKind` (after `TY_INT`)
- [ ] Add `TY_UINT8`, `TY_UINT16`, `TY_UINT32`, `TY_UINT64` to `TypeKind`
- [ ] Add `TY_FLOAT32`, `TY_FLOAT64` to `TypeKind`
- [ ] All new kinds have `CK_COPY` in `type_copy_kind()`
- [ ] All new kinds have `KIND_STAR` in `type_effective_kind()` (HKT)

#### `src/types.c`
- [ ] `type_c_name()` — add cases:
  - `TY_INT8`  → `"int8_t"`, `TY_INT16` → `"int16_t"`, etc.
  - `TY_UINT8` → `"uint8_t"`, etc.
  - `TY_FLOAT32` → `"float"`, `TY_FLOAT64` → `"double"`
- [ ] `type_name()` — add cases returning the Turmeric surface names (`"int8"`, `"uint8"`, `"float32"`, etc.)
- [ ] `type_from_kind()` — handle new kinds
- [ ] `type_int8()`, `type_int16()`, … `type_uint64()`, `type_float32()`, `type_float64()` convenience constructors (mirroring `type_int()`, `type_float()`)

#### Fixtures
- [ ] `tests/fixtures/numeric-types/types-exist/` — verify each new type elaborates without error

---

### Phase N1: Lexer & Parser — Literal Suffixes

**Goal:** Allow integer and float literals with type suffixes. Unsuffixed literals
keep their existing behaviour.

**Exit Criterion:** Suffixed literals lex and parse to a typed literal node.

#### `src/reader.c`
- [ ] Extend integer literal scanning: after digits, scan an optional suffix
  (`i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`)
- [ ] Extend float literal scanning: after digits/exponent, scan an optional
  suffix (`f32`, `f64`)
- [ ] On `Form` creation, store the resolved `TypeKind` alongside the literal
  value (or attach as an annotation tag; see below)
- [ ] Emit a `diag_error` if the literal value overflows the requested type
  at lex time (e.g., `256u8` is a lex error)

#### `src/forms.h`
- [ ] Add `TypeKind lit_kind` field (or a nullable suffix enum) to `Form` for
  `F_INT` and `F_FLOAT` nodes, defaulting to `TY_UNKNOWN` for unsuffixed

#### Fixtures
- [ ] `tests/fixtures/numeric-types/literal-suffix/` — one sub-fixture per suffix
- [ ] `tests/fixtures/errors/literal-overflow/` — `256u8`, `-129i8`, etc. produce
  diagnostics

---

### Phase N2: Elaborator — Typing and `as` Casts

**Goal:** The elaborator resolves suffixed literals to their declared type and
handles `(as TargetType expr)` explicit coercions.

**Exit Criterion:** Suffixed literals have the correct `Type` in the elaborated
`Expr`; `as` casts typecheck; mismatches produce clear diagnostics.

#### `src/elab.c`
- [ ] Resolve keyword-named types in `elab_type_expr()`:
  - `int8` … `uint64`, `float32`, `float64` → corresponding `TY_*` kind
  - `int` → `TY_INT` (unchanged), `float` → `TY_FLOAT` (unchanged)
- [ ] `elab_literal_int()` — if `lit_kind != TY_UNKNOWN`, use it; otherwise `TY_INT`
- [ ] `elab_literal_float()` — if `lit_kind != TY_UNKNOWN`, use it; otherwise `TY_FLOAT`
- [ ] Add `EX_CAST` expr node (or reuse `EX_BUILTIN`) for `(as T expr)`:
  - If source and target are both numeric, it is a legal narrowing/widening cast
  - If types differ and neither subsumes the other, emit a diagnostic
- [ ] Type mismatch on numeric argument: suggest `(as TargetType x)` in the
  diagnostic message

#### `src/expr.h`
- [ ] Add `EX_CAST` node (or reuse existing, see codegen impact below)

#### Fixtures
- [ ] `tests/fixtures/numeric-types/elab-types/` — type annotations accepted
- [ ] `tests/fixtures/numeric-types/as-cast/` — `(as int32 x)` elaborates correctly
- [ ] `tests/fixtures/errors/numeric-mismatch/` — passing `int8` where `int32` expected

---

### Phase N3: Codegen

**Goal:** Emit correct C for new numeric types: literals, casts, arithmetic.

**Exit Criterion:** Generated C compiles without warnings under `-Wall`.

#### `src/emit.c`
- [ ] `atom_int()` — emit typed literal macros:
  - `INT8_C(n)`, `INT16_C(n)`, `INT32_C(n)`, `INT64_C(n)` (already used for `TY_INT`)
  - `UINT8_C(n)`, `UINT16_C(n)`, `UINT32_C(n)`, `UINT64_C(n)`
- [ ] `atom_float()` — emit `(float)(n)` for `TY_FLOAT32`, plain double literal for `TY_FLOAT64`
- [ ] `emit_value()` — handle `EX_CAST`: emit `(target_c_type)(expr)`
- [ ] Arithmetic operators (`+`, `-`, `*`, `/`, `%`) — ensure `emit_binop()` uses
  the declared type for both operands; no implicit C promotion surprises
- [ ] Generated headers (`#include <stdint.h>`) — already present; verify `float` / `uint*` headers are included

#### `atom_int` suffix dispatch table (example sketch):
```c
static char *atom_numeric(TypeKind k, int64_t i) {
    char buf[64];
    switch (k) {
        case TY_INT8:   snprintf(buf, sizeof buf, "INT8_C(%lld)",   (long long)i); break;
        case TY_INT16:  snprintf(buf, sizeof buf, "INT16_C(%lld)",  (long long)i); break;
        case TY_INT32:  snprintf(buf, sizeof buf, "INT32_C(%lld)",  (long long)i); break;
        case TY_INT64:  /* fall through */
        case TY_INT:    snprintf(buf, sizeof buf, "INT64_C(%lld)",  (long long)i); break;
        case TY_UINT8:  snprintf(buf, sizeof buf, "UINT8_C(%llu)",  (unsigned long long)i); break;
        case TY_UINT16: snprintf(buf, sizeof buf, "UINT16_C(%llu)", (unsigned long long)i); break;
        case TY_UINT32: snprintf(buf, sizeof buf, "UINT32_C(%llu)", (unsigned long long)i); break;
        case TY_UINT64: snprintf(buf, sizeof buf, "UINT64_C(%llu)", (unsigned long long)i); break;
        default: snprintf(buf, sizeof buf, "INT64_C(%lld)", (long long)i); break;
    }
    return strdup(buf);
}
```

#### Fixtures (golden C output)
- [ ] `tests/fixtures/numeric-types/codegen-int/` — int8 … uint64 arithmetic
- [ ] `tests/fixtures/numeric-types/codegen-float/` — float32 / float64
- [ ] `tests/fixtures/numeric-types/codegen-cast/` — `as` casts emit C casts

---

### Phase N4: Stdlib — Typeclass Instances

**Goal:** All new numeric types have complete typeclass instances for `Eq`, `Ord`,
`Show`, `Num` (Add/Sub/Mul/Div), and `Clone`.

**Exit Criterion:** All typeclass methods resolve for each new type; no missing
instance diagnostics.

#### `stdlib/typeclass.tur`
- [ ] `Eq` instances: `int8`, `int16`, `int32`, `int64`, `uint8`–`uint64`, `float32`, `float64`
- [ ] `Ord` instances: same list (unsigned types use unsigned comparison)
- [ ] `Show` instances: each type formats with appropriate `printf` specifier
  - `int8`/`int16`/`int32`: `%d` with cast to `int`; `int64`: `%lld`
  - `uint8`/`uint16`/`uint32`: `%u`; `uint64`: `%llu`
  - `float32`: `%.7g`; `float64`: `%.15g` (existing)
- [ ] `Add`/`Sub`/`Mul`/`Div` instances: delegates to C arithmetic operators
- [ ] `Clone` instances: all numeric types are trivially `Clone` (copy by value)

#### `stdlib/checked.tur`  *(new file)*
- [ ] `(checked-add [x : T, y : T] : (option T))` for all integer types
- [ ] `(checked-sub [x : T, y : T] : (option T))` for all integer types
- [ ] `(checked-mul [x : T, y : T] : (option T))` for all integer types
- [ ] Uses `__builtin_add_overflow` / `__builtin_sub_overflow` / `__builtin_mul_overflow`

#### `stdlib/saturating.tur`  *(new file)*
- [ ] `(sat-add [x : T, y : T] : T)` for all integer types
- [ ] `(sat-sub [x : T, y : T] : T)` for all integer types
- [ ] Clamps to `T-min` / `T-max` constants (defined in stdlib)

#### `stdlib/numeric.tur`  *(new file — constants and utilities)*
- [ ] `int8-min`, `int8-max`, … `uint64-max` constants
- [ ] `float32-inf`, `float32-nan`, `float64-inf`, `float64-nan`
- [ ] `(bits [x : T] : uint64)` — reinterpret numeric value as its bit pattern

#### Fixtures
- [ ] `tests/fixtures/numeric-types/typeclass-eq/`
- [ ] `tests/fixtures/numeric-types/typeclass-show/`
- [ ] `tests/fixtures/numeric-types/checked-add/`
- [ ] `tests/fixtures/numeric-types/sat-add/`
- [ ] `tests/fixtures/numeric-types/constants/`

---

### Phase N5: `println` and `print` — Per-Type Dispatch

**Goal:** `println` works for all new numeric types without requiring an explicit
`show` call.

**Exit Criterion:** `(println 42i8)` compiles and prints `42`.

#### `src/builtins.h` / `src/builtins.c`
- [ ] Add `BS_PRINTLN_INT8`…`BS_PRINTLN_UINT64`, `BS_PRINTLN_FLOAT32` shapes
  (or consolidate into a single `BS_PRINTLN_NUMERIC` that selects the format
  string based on `TypeKind`)
- [ ] Wire the builtin table entries for each new type

#### Fixtures
- [ ] `tests/fixtures/numeric-types/println-int8/`
- [ ] `tests/fixtures/numeric-types/println-uint64/`
- [ ] `tests/fixtures/numeric-types/println-float32/`

---

### Phase N6: Pattern Matching and Struct Fields

**Goal:** New numeric types can appear in pattern `match` arms and as struct field types.

**Exit Criterion:** Structs with `uint8` fields elaborate and match correctly.

#### `src/elab.c`
- [ ] `elab_match()` — extend numeric literal pattern arms to check against the
  declared scrutinee type (e.g., pattern `42u8` on a `uint8` scrutinee matches;
  on an `int64` scrutinee emits a type-mismatch diagnostic)

#### `src/emit.c`
- [ ] Struct field codegen — struct fields whose `TypeKind` is a new numeric kind
  emit the correct C field type

#### Fixtures
- [ ] `tests/fixtures/numeric-types/struct-fields/` — struct with `uint8`/`float32` fields
- [ ] `tests/fixtures/numeric-types/pattern-match-numeric/` — match on `int16` literals

---

### Phase N7: FFI Integration

**Goal:** New numeric types can be used at the FFI boundary (`extern-c`).

**Exit Criterion:** `extern-c` declarations using `int32`, `uint8`, `float32` etc. work.

#### Documentation update: `docs/guides/c-integration-guide.md`
- [ ] Extend the type annotation reference table with all new numeric types

#### `src/elab.c`
- [ ] `elab_extern_c()` — accept new type keywords in extern-c signatures

#### Fixtures
- [ ] `tests/fixtures/numeric-types/ffi-int32/` — extern-c returning `int32`
- [ ] `tests/fixtures/numeric-types/ffi-uint8/` — extern-c taking `uint8`

---

### Phase N8: Diagnostics and Error Messages

**Goal:** All type-mismatch diagnostics involving numeric types are clear and
actionable.

**Exit Criterion:** No cryptic "type mismatch" messages; all errors suggest `as`
casts where appropriate.

#### `src/diag.c`
- [ ] Add `"did you mean (as <type> ...)?"` hint to numeric type mismatch errors
- [ ] Warn on literal truncation: `(let [x : int8] 200)` — value out of range

#### Fixtures
- [ ] `tests/fixtures/errors/numeric-mismatch-hint/`
- [ ] `tests/fixtures/errors/literal-truncation/`

---

## Compatibility and Migration

- `int` continues to mean `int64_t`. **No existing code changes.**
- `float` continues to mean `double`. **No existing code changes.**
- `int64` is a new alias for the same type. Existing `int` code can optionally
  migrate to `int64` for explicitness; there is no deadline.
- All new types are additive. No syntax or semantics of existing types is altered.

---

## Affected Files Summary

| File | Change |
|---|---|
| `src/types.h` | New `TY_INT8`…`TY_UINT64`, `TY_FLOAT32`, `TY_FLOAT64` kinds |
| `src/types.c` | `type_c_name`, `type_name`, `type_copy_kind`, new constructors |
| `src/forms.h` | `lit_kind` field on `F_INT` / `F_FLOAT` |
| `src/reader.c` | Literal suffix scanning, overflow check |
| `src/elab.c` | Type keyword resolution, `EX_CAST`, mismatch hints |
| `src/expr.h` | `EX_CAST` node (if new) |
| `src/emit.c` | `atom_numeric()`, cast emission, struct field types |
| `src/builtins.h/c` | `println` shapes for new types |
| `src/diag.c` | `as`-cast hints |
| `stdlib/typeclass.tur` | `Eq`/`Ord`/`Show`/`Num`/`Clone` instances |
| `stdlib/checked.tur` | New — checked arithmetic |
| `stdlib/saturating.tur` | New — saturating arithmetic |
| `stdlib/numeric.tur` | New — constants, `bits` |
| `docs/guides/c-integration-guide.md` | Updated FFI type table |

---

## Success Criteria

### Minimum Viable (N0–N2 complete)
- [ ] All new type keywords elaborate without error
- [ ] Suffixed literals type-check to the correct type
- [ ] `(as int8 x)` casts typecheck

### Feature Complete (N0–N6 complete)
- [ ] All new types work in arithmetic, struct fields, and pattern matching
- [ ] All typeclass instances resolve
- [ ] `println` works for each type

### Production Ready (N0–N8 complete)
- [ ] FFI boundary accepts new numeric types
- [ ] Diagnostics include actionable `as`-cast suggestions
- [ ] Checked and saturating stdlib modules ship

---

## Timeline Estimate

| Phase | Estimated Duration |
|---|---|
| N0: Type System     | 1–2 days |
| N1: Lexer / Parser  | 2–3 days |
| N2: Elaborator      | 3–5 days |
| N3: Codegen         | 2–3 days |
| N4: Stdlib Instances| 3–5 days |
| N5: println dispatch| 1–2 days |
| N6: Match / Structs | 2–3 days |
| N7: FFI             | 1–2 days |
| N8: Diagnostics     | 1–2 days |
| **Total**           | **16–27 days** |
