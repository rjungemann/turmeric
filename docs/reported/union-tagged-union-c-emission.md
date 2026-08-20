# Unions never get the documented per-member C union emission

**Severity: low** -- works correctly, but every `(A | B)` rides the generic
`tur_tagged_t` `{int64_t tag; int64_t val}`, so wide by-value members must
heap-box into the 64-bit `val` slot (an allocation + indirection the docs said
would not happen). Found in the 2026-08-20 docs audit.

## Repro

`(defn f [x : (int | cstr)] : unit ...)` -- emitted C declares
`tur_tagged_t x`, never a member-typed
`struct { int tag; union { ... } data; }`.

## Root cause

src/compiler/types.c:3386 (`case TY_UNION: return "tur_tagged_t";`); boxing at
src/compiler/emit_expr.c:3826-3838 (union_inject heap-boxes by-value
aggregates).

## Fix direction

Emit a per-union monomorph typedef with a real member union when all members
have concrete codegen layout, keeping `tur_tagged_t` as the fallback;
unbox/box at the same `EX_ANY_CAST`/inject seams.

## Guides to update when fixed

- docs/guides/union-intersection-types-guide.md (representation paragraphs,
  Known Limitations, Deferred table)
