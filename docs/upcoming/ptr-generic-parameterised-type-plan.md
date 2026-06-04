---
title: Parameterised `:ptr<T>` Generic Pointer Type Plan
category: Planning
description: Today `:ptr<T>` is hard-coded as a single keyword string `:ptr<void>`. There is no general parameterised `:ptr<T>` -- `:ptr<float>`, `:ptr<int>`, `:ptr<MyStruct>` all fail to parse as keywords and degrade to either "unsupported return type keyword" errors or untyped int handles. This plan makes `:ptr<T>` a first-class parameterised type. Until it lands, every "typed heap cell" in stdlib and fixtures is forced into a `:ptr<void>` + inline-C cast workaround.
---

# Parameterised `:ptr<T>` Generic Pointer Type -- Plan

> **Status:** Implemented (P1-P4 core)
> **Last Updated:** 2026-06-03
> **Type:** compiler -- type system / parser
> **Surfaced by:**
> - [language-readiness-for-typed-signal-plan.md](language-readiness-for-typed-signal-plan.md) G5 spike
>   (`tests/fixtures/typed-state-cell/`)

---

## Implementation notes (landed)

`:ptr<T>` is now a first-class typed raw pointer. The surface syntax carries
**no inner colon** -- write `:ptr<float>`, `:ptr<MyStruct>`, `:ptr<ptr<float>>`
(not `:ptr<:float>`). Such names lex as a single keyword token already, so no
lexer change was needed.

Design decision (P2): rather than introduce a new `TypeKind` (which would force
a `case` into every `-Wswitch`/`-Werror` switch over `TypeKind`), the typed
pointer is **folded onto the existing `TY_PTR_VOID`** with an optional pointee:

- `Type.as.ptr.inner` (a `struct Type *`) holds the pointee `T`; `NULL` means
  the legacy untyped `ptr<void>`. See `type_ptr()` in `types.h`.
- Equality (`type_eq`): two typed pointers are equal iff their pointees are
  equal; `ptr<void>` (NULL inner) stays interoperable with any pointer for
  back-compat (the runtime threads raw handles through `ptr<void>` sinks).
- Codegen (`type_c_name`): `ptr<T>` lowers to `T *` -- including from inline-C
  bodies, so `(defn f [] :ptr<float> ```c ... ```)` declares `double *f(...)`
  with no `(intptr_t)` round-trip. Compound C names are interned
  (`intern_type_name`) so the string stays live and LeakSanitizer-clean.

Resolution entry point: `ptr_type_from_keyword_name()` in `elab_types.c`,
called from `type_expr_from_form` (both `F_SYM` spaced and `F_KEYWORD` compact
forms) and from the inline keyword dispatch in `defn`/`fn` params and returns.
The inner type is resolved recursively, so primitives, structs, ADTs, type
variables (`ptr<A>` inside a `[A]` param list), and nested pointers all work.

Acceptance: `tests/fixtures/typed-state-cell/` compiles and runs with
`:ptr<float>` and no casts; `ptr<int>`/`ptr<bool>`/`ptr<Struct>`/`ptr<A>` and
nested `ptr<ptr<float>>` (-> `double **`) all verified; full `tests/run.sh`
green (the one unrelated `float-fat-closure` codegen-snapshot failure predates
this work).

---

## The missing functionality

`:ptr<void>` is treated as a single 9-byte keyword string throughout the
elaborator -- see the literal `memcmp(kw->name, "ptr<void>", 9)` checks
scattered across `src/compiler/elab_typeclasses.c:121,619,675,766,1255,1877,1929`
and `src/compiler/elab_call.c:1686`. There is no code path that
recognises a generic `:ptr<T>` for any other `T`.

As a result:

```turmeric
(defn alloc-state [] : ptr<float>          ;; (1) error
  ```c return calloc(1, sizeof(double)); ```)

(defn read-state [s : ptr<float>] : float    ;; (2) silently degrades
  ```c return *s; ```)
```

Diagnostics observed in (1):

```
unsupported return type keyword 'ptr<float>': it is not a built-in type
and is not bound by any parameter; declare it (e.g. `[ptr<float>]` type
params) or annotate a parameter with it to use it as a type variable
```

And in (2), the parameter binding is accepted as if `ptr<float>` were a
free type variable, then immediately rejected at callsite type-checking
with `TUR-E0001: function 'read-state' arg 1: expected <struct>, got int`.
Both behaviours stem from the same root cause: the keyword `:ptr<float>`
is not lexed/parsed as the application `Ptr` to type argument `float`,
but as a single opaque identifier the elaborator has never heard of.

The supported workaround is to type the cell as `:ptr<void>` and cast
inside the inline-C body of every accessor:

```turmeric
(defn read-state [p : ptr<void>] : float
  ```c double *s = (double *)p; return *s; ```)
```

This pushes type information out of the signature and into the C body,
which defeats the typing point for any non-trivial heap-cell ABI (DSP
state, ring buffers, mutable accumulators, etc.).

## Why this matters

Every typed-heap-cell idiom in the language currently routes through
`:ptr<void>` + cast or an opaque `:int` handle:

- `stdlib/future.tur` -- promises and futures are all `:ptr<void>`.
- `stdlib/mutmap.tur` -- mutable map storage is `:ptr<void>`.
- DSP filter state, ADSR envelopes, ring buffers, sample accumulators
  (the entire surface the typed signal library wants).
- Any future "owned heap T" abstraction (`Box<T>`, arena slabs, etc.)
  if/when it lands.

The G5 spike of
[language-readiness-for-typed-signal-plan.md](language-readiness-for-typed-signal-plan.md)
verified that `:ptr<:float>` is the *desired* shape for filter state but
that the language cannot express it today, so the spike fixture (and any
follow-on signal-library code) carries the cast-in-the-body workaround.

## Goals

1. `:ptr<T>` parses as a parameterised type for any well-formed `T`:
   primitives (`:ptr<float>`, `:ptr<int>`, `:ptr<bool>`), structs
   (`:ptr<Pair>`, `:ptr<Cons>`), ADTs, type variables (`:ptr<A>` inside
   a polymorphic `[A]` parameter list), and nested forms
   (`:ptr<:ptr<float>>`).
2. Inline-C bodies see the parameter as the correct C pointer type --
   `double *p` for `:ptr<float>`, `int64_t *p` for `:ptr<int>`,
   `Tuple2 *p` for `:ptr<Tuple2>` -- with no `(intptr_t)` round-trip in
   user code.
3. The existing `:ptr<void>` spelling continues to work unchanged
   (back-compat).
4. The same machinery lets callers write `(:: x :ptr<float>)`
   annotations and let-bindings without surprises.

## Non-goals

- Owned/borrowed/reference-counted pointer modifiers (`Box<T>`,
  `Rc<T>`). This plan is plain raw `T *`.
- Sized types or layout-aware variants (`Vec<T>` already exists; this is
  about single-cell typed pointers).
- Per-element lifetime tracking, deallocation hooks, escape analysis.
  Free is still the user's responsibility, identical to `:ptr<void>`.

## Phasing

### P0 -- audit existing `ptr<void>` hardcoding

Catalogue every literal `"ptr<void>"` string compare and decide for each
site whether the new code path subsumes it or whether `void` stays a
hard-coded special-case (likely the latter -- `void` is its own type,
not a `T`). Producing this list also makes the test plan obvious: every
existing `:ptr<void>` use must continue to compile and run.

Affected files (initial scan):

- `src/compiler/elab_typeclasses.c:121,619,675,766,1255,1877,1929`
- `src/compiler/elab_call.c:1686`

### P1 -- parser/keyword recognition

Today `:ptr<T>` is lexed as a single keyword token whose bytes are
`p t r < ... >`. Choose one of:

a. **Lex-time decomposition** -- when a keyword token starts with
   `ptr<`, peel off the inner payload and re-tokenise as the application
   `Ptr` to one type argument. Requires the inner payload to be a single
   type expression (no commas) and to balance angle brackets for nesting.

b. **Reserve `Ptr` as a type constructor** and require the surface form
   `(Ptr T)` instead of `:ptr<T>`. Simpler to implement, but breaks the
   existing `:ptr<void>` spelling.

(a) is preferred for back-compat. The parser already balances `<...>`
for things like `Vec<T>` in `defstruct`, so the machinery is mostly in
place.

### P2 -- type representation

Decide where `Ptr T` lives in the type lattice:

- A new `TY_PTR` constructor parameterised by an inner type, with
  `TY_PTR_VOID` either folded into it (`TY_PTR(VOID)`) or kept as a
  legacy alias.
- Equality: two `:ptr<T1>`, `:ptr<T2>` are equal iff `T1 == T2`. `:ptr<void>`
  remains its own thing (raw pointer, callsite-erased), distinct from
  `:ptr<int>`, `:ptr<float>`, etc.
- Subtyping: none. A `:ptr<float>` is *not* assignable to `:ptr<void>`
  implicitly (would re-introduce the cast-in-body footgun).

### P3 -- elaborator + codegen

- Every site that currently checks `kw->len == 9 && memcmp("ptr<void>")`
  needs to extend to "is this any `TY_PTR`?" with the void case kept as
  a sub-branch where it actually matters (raw-pointer rules).
- Codegen lowers `:ptr<T>` parameters to the corresponding C pointer
  type in function signatures, so inline-C bodies see `double *p` for
  `:ptr<float>`, etc., without needing `(intptr_t)` casts.
- Return-type emission similarly threads through; `calloc(1, sizeof T)`
  results in a body whose declared C return is `T *`.

### P4 -- amend existing workarounds

This is the deworkaround pass. The plan explicitly tracks rewriting the
`:ptr<void>` + cast idiom in places where the original intent was a
typed pointer. Concretely:

- `tests/fixtures/typed-state-cell/input.tur` -- restore the originally
  intended `:ptr<float>` spelling, drop the `double *s = (double *)p;`
  cast from every accessor body. The spike's verdict in
  [language-readiness-for-typed-signal-plan.md](language-readiness-for-typed-signal-plan.md)
  upgrades from amber to green.
- Any tur-signal-rebuild filter-state code that lands in the interim --
  rewrite from `:ptr<void>` to `:ptr<float>` (or `:ptr<FilterState>` for
  multi-field state once that shape is also typed).
- Audit `stdlib/future.tur`, `stdlib/mutmap.tur`, and any other
  `:ptr<void>`-typed handle whose value is *not* genuinely opaque to
  the consumer. Per-call: decide whether the void spelling is correct
  (raw / heterogenous storage, e.g. a hash map's value cells) or a
  workaround (homogeneous typed cell). Convert the workaround cases;
  leave the genuinely-opaque ones.
- Any `docs/reported/*` issues opened in the interim that were caused
  by the cast-in-body idiom get closed/resolved.

The deworkaround pass must not happen until P3 lands; this section
exists so the workarounds aren't forgotten when the underlying fix
ships.

### P5 -- documentation

- Update CLAUDE.md / docs/guides to mention `:ptr<T>` as the preferred
  spelling for typed heap cells, with `:ptr<void>` reserved for genuinely
  raw/erased pointers.
- Add a stdlib pattern note for "typed heap cell" showing the new
  spelling.

## Acceptance

1. The G5 spike fixture (`tests/fixtures/typed-state-cell/`) compiles
   and runs with `:ptr<float>` as its declared type and no
   `(intptr_t)` / `(double *)` cast anywhere in user code.
2. `:ptr<void>` continues to work for every existing call site
   (full `bash tests/run.sh` green).
3. `:ptr<T>` for `T` in {`int`, `float`, `bool`, a `defstruct`-defined
   struct, a `defopaque` newtype, and a type variable inside a
   polymorphic param list} all elaborate, emit the expected C pointer
   type, and round-trip values correctly.
4. Nested `:ptr<:ptr<float>>` is accepted and lowered to `double **`.

## Cross-references

- Surfaced by:
  [language-readiness-for-typed-signal-plan.md](language-readiness-for-typed-signal-plan.md)
  G5 (and indirectly G8, which needs typed filter state).
- Feeds: [tur-signal-rebuild-plan.md](tur-signal-rebuild-plan.md) --
  any typed-state DSP primitives want this.
- Related: [stdlib-inline-c-deworkaround-plan.md](stdlib-inline-c-deworkaround-plan.md),
  [stdlib-opaque-handle-types-plan.md](stdlib-opaque-handle-types-plan.md) --
  both target the same "typed-thing hidden behind an int handle" smell
  from different angles. Coordinate P4's audit list with both.
