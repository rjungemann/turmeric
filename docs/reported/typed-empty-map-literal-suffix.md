# #map{} has no typed-empty suffix although []:T and #set{}:T exist

**Severity: low**. Found in the 2026-08-20 docs audit.

## Repro

`[]:T` and `#set{}:T` pin empty-literal element types, but an empty `#map{}`
requires ascribing the full `(Map K V)`.

## Root cause

The literal-suffix handling in src/compiler/reader.c (~line 1144) covers vec
and set closers only.

## Fix direction

Accept `#map{}:(K V)` lowering to `(:: (hamt-of) (Map K V))`.

## Guides to update when fixed

- docs/guides/data-literals-guide.md (final Rules bullet)
