# Plan: Arbitrary-arity kinds (lift the Tuple5 / KIND_ARROW5 cap)

> **Status:** Draft Plan
> **Last Updated:** 2026-05-29
> **Type:** Compiler / type system
> **Related:** `stdlib/tuple.tur` (Tuple2..Tuple5 ladder), Phase HKT (H0, TP2),
> `src/compiler/types.{h,c}`.

---

## Overview

The `Kind` enum in `src/compiler/types.h:45-53` is a closed ladder:

```c
KIND_STAR, KIND_ARROW, KIND_ARROW2, KIND_ROW, KIND_ARROW3, KIND_ARROW4, KIND_ARROW5
```

`kind_for_arity(n)` (`types.c:2141`) silently clamps any `n > 5` to
`KIND_ARROW5`, and `kind_apply_one(k)` (`types.c:2152`) walks the same fixed
ladder back down. Together they fix the maximum number of type parameters of
any type constructor at **5**, which is why `stdlib/tuple.tur` stops at
`Tuple5` and the doc comment at `types.h:1110` calls the cap "enforced at
definition sites."

This plan replaces the enum with an integer-backed representation so the
arity cap disappears and `TupleN` for arbitrary `N` becomes a stdlib-only
change. It is deliberately conservative: it preserves the existing API
surface (`Kind`, `kind_for_arity`, `kind_apply_one`, `kind_eq`,
`kind_to_string`, `kind_parse`, `hkt_kind`) and the on-disk ABI of `Type`,
so every elaborator and emitter call site keeps working without edits.

## Goals

1. Eliminate the arity-5 ceiling on type constructors.
2. Keep all existing `Kind` consumers source-compatible -- no churn at the
   ~170 reference sites already in the tree.
3. Land `Tuple6`..`Tuple8` (or higher, configurable) in `stdlib/tuple.tur`
   as a demonstration that nothing else needs to change.

## Non-goals

- Type-level functions, kind polymorphism, or first-class kinds.
- Reorganising the `Tuple` family into an HList shape (see Appendix A).
- Changing `KIND_ROW` semantics.

## Approach

### Step 1 -- Redefine `Kind` as an integer-backed type

In `src/compiler/types.h`, replace the enum with either:

**Option A (recommended):** an opaque `typedef uint16_t Kind;` with named
constants supplied as `static const Kind KIND_STAR = 0;` etc. Encoding:

| Value range  | Meaning                              |
| ------------ | ------------------------------------ |
| `0`          | `KIND_STAR`                          |
| `1..N`       | `KIND_ARROW`..`KIND_ARROW{N}` (arity = value) |
| `0xFFFF`     | `KIND_ROW` (sentinel)                |

Arity is just the numeric value, which is what `kind_for_arity` and
`kind_apply_one` already need. `KIND_ARROW2` / `KIND_ARROW3` / ... remain
valid named constants that other code can keep using.

**Option B:** a small struct `{ uint16_t arrows; bool is_row; }`. More
explicit, but every `Kind k = KIND_STAR;` initialisation in the tree (there
are dozens at `types.h:632`, `types.h:687`, ..., as well as `t.hkt_kind = …`
assignments) has to switch to `(Kind){0}` or `KIND_STAR_V`. Rejected for
that reason -- the source-compat goal pays for itself here.

Going with Option A.

### Step 2 -- Reimplement the two real switches

Only two functions actually destructure `Kind` today
(`grep -rn "case KIND_" src/compiler/`):

- `kind_to_string` (`types.c:2113`) -- replace the switch with: handle
  `KIND_ROW`, then build `"* -> * -> ... -> *"` in a small `Buf` based on
  `arrows = (uint32_t)k`. Memoise the first ~16 strings in a static table
  so the common case stays zero-alloc and the return value can remain
  `const char *`.
- `kind_apply_one` (`types.c:2152`) -- becomes
  `return (k == KIND_STAR || k == KIND_ROW) ? k : (Kind)(k - 1);`.

`kind_for_arity` becomes `return (Kind)n;` (no cap). `kind_eq` stays
`a == b`.

`kind_parse` (`types.c:2126`) already counts `" -> *"` suffixes and then
calls `kind_for_arity`, so it picks the new behaviour up for free.

### Step 3 -- Adjust `hkt_kind` storage if needed

`Type.hkt_kind` (`types.h:374`) is currently typed `Kind`. With Option A
it stays the same width (`uint16_t`) and the field is byte-compatible with
the existing layout. Verify with `static_assert(sizeof(Kind) == 2)` and
add a comment noting the new encoding.

### Step 4 -- Stdlib: extend the Tuple family

In `stdlib/tuple.tur`, add `Tuple6`..`Tuple8` (or whatever ceiling we want
to ship initially) following the existing template: `defstruct`,
`tupleN`, and `tupleN-1st`..`tupleN-Nth` accessors. The `Eq` instance for
`Tuple2` remains the only typeclass instance for now (the
parametric-struct dispatch issue called out in the file is orthogonal).

There is no hard cap any more; the ceiling is purely "how many handwritten
accessors do we want to ship." A follow-up could codegen these from a
macro, but that's out of scope for this plan.

### Step 5 -- Tests

Add fixtures under `tests/fixtures/`:

- `tuple-arity-6` -- construct + destructure a `Tuple6` end-to-end.
- `kind-arity-roundtrip` -- a `kind_parse` / `kind_to_string` round-trip
  for `arity=8` (unit-test-style; can live as a C unit test in
  `tests/unit/` if that's where kind helpers are tested today, otherwise
  drive it from `tur eval` via a small intrinsic).
- One test exercising a typeclass declared over a 6-ary constructor to
  confirm `elab_types.c:1573` and `elab_structs.c:673` still produce the
  right `hkt_kind`.

### Step 6 -- Docs

- Update the comment block at `types.h:41-53` to describe the new
  encoding.
- Update the comment at `types.h:1107-1115` to remove the "arity-cap"
  language.
- Update the `tur/tuple` module docstring (`stdlib/tuple.tur:1-12`) to
  reflect that the ladder is open-ended.

## Risk / rollback

- **ABI risk:** `Kind` width changes from `enum` (typically `int`,
  4 bytes) to `uint16_t` (2 bytes). `Type.hkt_kind` is already declared
  as `Kind`, so the struct layout will shift unless we either widen the
  typedef to `uint32_t` or audit padding. Recommend `uint16_t` plus a
  layout assert; the existing `Type` struct already has 2-byte fields
  adjacent to `hkt_kind` so the change is likely a wash. If layout
  surprises arise, fall back to `typedef uint32_t Kind;`.
- **Exhaustiveness warnings:** the two switches we delete were the only
  thing making `-Wswitch` useful for `Kind`. Acceptable -- there were
  only ever two, and both are now branchless.
- **Rollback:** revert the diff; `stdlib/tuple.tur` keeps Tuple2..5 either
  way, so no stdlib consumer is stranded.

## Sequencing

1. Land the `Kind` refactor + helper rewrites + layout assert. Zero
   stdlib changes. CI should be green with no test changes.
2. Add `Tuple6`..`Tuple8` to `stdlib/tuple.tur` plus the fixture.
3. Update docs + module docstring.

Each step is independently revertable.

---

## Appendix A -- Why not HList-style tuples?

A natural alternative is to ditch the fixed `TupleN` family entirely in
favour of a cons-shaped construction:

```turmeric
(defstruct HNil [])
(defstruct HCons [H T] (head H) (tail T))
;; (HCons Int (HCons Str (HCons Bool HNil))) ~= a 3-tuple
```

This gives *truly* arbitrary arity with only two type constructors, and it
opens the door to generic operations over tuples (`hlist-map`, `hlist-zip`,
generic `Show`/`Eq` derivation by structural recursion on the spine).
Haskell's `HList`, Scala's `shapeless`, and Rust's `frunk` all take this
route, and it's a real expressive gain.

The reasons this plan picks the integer-kind refactor instead:

| Axis | Integer-kind (this plan) | HList tuples |
| ---- | ------------------------ | ------------ |
| Surface change | `Kind` typedef + 2 functions + stdlib additions | New struct shape, accessor desugaring, type-printing changes, every existing `Tuple2..5` call site migrated or kept as legacy alias |
| Effect on existing code | None -- `Tuple2..5` keep working byte-for-byte | Either a hard cut (breaks everything that names `Tuple2`) or a long deprecation tail |
| Accessor ergonomics | `(tuple6-3rd t)` -- familiar, O(1) | `(.head (.tail (.tail t)))` or new sugar; recursive in the type checker even if O(1) in C |
| Type-printing | Stays compact: `Tuple6[A B C D E F]` | Deeply nested `HCons[A HCons[B HCons[...]]]` unless we add custom printers |
| Generic ops over tuples | Still per-arity (matches today's reality) | Free -- structural recursion on the spine |
| Cap on arity | None | None |
| Implementation cost | ~1 day, contained to `types.{h,c}` and stdlib | Multi-week, touches elaborator, codegen, printing, docs, stdlib migration |

The crucial asymmetry is that **arbitrary arity was the only thing
blocking Tuple6+** -- the rest of Turmeric's type infrastructure already
handles N-ary parameterised structs uniformly. Lifting the kind cap is
the smallest change that unblocks the user-visible feature. HList tuples
are a different feature (typeclass-driven generics over tuple shape);
they should be evaluated on their own merits, not bundled with "we want
Tuple7."

If we later decide HList tuples are worth the cost, this plan does not
block them -- arbitrary kinds are a prerequisite for HList machinery
anyway (the spine struct has kind `* -> * -> *`, and generics over it
need to apply it at varying arities).
