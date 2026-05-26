# Plan: Optional delimiter support in stdlib/csv.tur

> **Status:** Proposed
> **Last Updated:** 2026-05-26
> **Type:** Stdlib enhancement

---

## Overview

`stdlib/csv.tur` currently hardcodes comma (`,`) as the field delimiter in
all parse and emit paths. This plan adds an optional delimiter parameter so
callers can handle TSV and other delimiter-separated formats without changing
existing code.

Primary target formats:

- CSV (comma): `,` (default)
- TSV (tab): `\t`
- Pipe-delimited: `|`
- Semicolon-delimited: `;`

The enhancement is additive: existing function names and behavior stay intact,
while new `*-with-delim` variants accept a delimiter argument.

---

## Current State

In `stdlib/csv.tur`, parse and emit logic compares and writes literal `,`
across:

- Row parse logic in `csv/parse-row`
- Multi-line parse logic in `csv/parse`
- Row emit logic in `csv/emit-row`
- Multi-row emit logic in `csv/emit`
- File helpers `csv/read-file` and `csv/write-file`

Quoted field rules already work and should remain unchanged:

- Quoted fields begin/end with `"`
- Escaped quotes are doubled (`""`)
- Newlines inside quoted fields are allowed

---

## Goals

1. Add delimiter configurability without breaking current APIs.
2. Keep RFC-4180-style quote escaping behavior.
3. Keep comma as the default delimiter when no delimiter is provided.
4. Support practical delimited text formats (TSV, pipe, semicolon).
5. Add tests that prove default compatibility and custom delimiter behavior.

## Non-goals

- Multi-character delimiters.
- Full dialect system (quote char changes, escape char changes, header policy).
- Automatic delimiter detection.

---

## Proposed API

Add new delimiter-aware variants and keep existing functions as wrappers.

### New functions

- `csv/parse-row-with-delim [line :cstr delim :int] :int`
- `csv/parse-with-delim [s :cstr delim :int] :int`
- `csv/emit-row-with-delim [v :int delim :int] :cstr`
- `csv/emit-with-delim [rows :int delim :int] :cstr`
- `csv/read-file-with-delim [path :cstr delim :int] :int`
- `csv/write-file-with-delim [path :cstr rows :int delim :int] :int`

`delim` is an integer character code (ASCII byte). Example values:

- comma: `44`
- tab: `9`
- pipe: `124`
- semicolon: `59`

### Existing functions remain

Existing functions keep current signatures and behavior:

- `csv/parse-row`
- `csv/parse`
- `csv/emit-row`
- `csv/emit`
- `csv/read-file`
- `csv/write-file`

Implementation strategy: each existing function delegates to the new
`*-with-delim` version with comma (`44`).

---

## Parsing and Emission Rules

### Delimiter semantics

- Separator checks replace literal `,` with delimiter byte.
- Quoting logic still uses `"` only.
- A delimiter inside a quoted field is treated as text.
- An unquoted delimiter ends the current field.

### Validation

Reject invalid delimiter values at runtime in `*-with-delim` functions:

- `0` (NUL)
- `"` (quote char)
- `\n` and `\r`

On invalid delimiter:

- Parse functions return `0` (error sentinel consistent with current style).
- Write/emit functions return `0` or `-1` according to current function family
  conventions (`csv/write-file-with-delim` returns `-1`).

---

## Implementation Plan

### Phase D1: Internal delimiter-aware helpers

- Factor duplicated logic in `csv.tur` into internal C helpers within each
  function body pattern (no cross-defn C calls needed).
- Replace delimiter literals with a local `char sep = (char)delim;`.

### Phase D2: Add public `*-with-delim` APIs

- Introduce six new delimiter-aware functions.
- Add docstrings using project `;;;` format.

### Phase D3: Backward-compatible wrappers

- Update existing public functions to delegate with comma (`44`).
- Verify behavior parity for existing CSV tests/fixtures.

### Phase D4: Tests and fixtures

Add coverage for both parse and emit paths:

- Default delimiter parity tests (comma path unchanged)
- TSV parse/emit (`delim = 9`)
- Pipe-delimited parse/emit (`delim = 124`)
- Quoted field containing delimiter
- Invalid delimiter rejection
- File read/write round-trip for custom delimiter

### Phase D5: Docs updates

- Update stdlib docs where CSV functions are listed.
- Add one small example for TSV usage.

---

## Example Usage (target)

```turmeric
;;; Parse TSV text
(let [rows (csv/parse-with-delim "name\tage\nAda\t37\n" 9)]
  ...)

;;; Emit semicolon-delimited text
(let [txt (csv/emit-with-delim rows 59)]
  ...)

;;; Keep existing behavior unchanged
(let [rows (csv/parse "a,b\n1,2\n")]
  ...)
```

---

## Risks and Mitigations

1. Risk: Behavior drift in default comma mode.
   Mitigation: Keep old functions as wrappers and assert parity tests.

2. Risk: Edge cases in duplicated inline C parse code.
   Mitigation: Consolidate delimiter handling patterns and add quote-focused tests.

3. Risk: Confusion around delimiter parameter type.
   Mitigation: Document as ASCII code and provide common constants in examples.

---

## Acceptance Criteria

1. Existing CSV API callers require no changes and pass unchanged tests.
2. New delimiter-aware API handles tab, pipe, and semicolon delimiters.
3. Quoted delimiter behavior matches current quoted-comma behavior.
4. Invalid delimiters are rejected predictably.
5. `just test` remains green after adding tests.
