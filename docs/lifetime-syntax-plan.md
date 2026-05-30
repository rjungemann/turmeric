# Lifetime-Annotation Syntax Plan (`'a` on types)

> **Status:** Not started (planning). Companion to
> [typing-gap-plan.md](typing-gap-plan.md) Phase TY4, which closed the real
> borrow-escape soundness gap (TUR-E0105) and made the lifetime *machinery*
> (`lifetime_elision_apply`, `lifetime_outlives`, `lifetime_has_cycle`,
> `lifetime_check_program`) live, correct, and cycle-safe. This plan covers the
> one piece TY4 deliberately left out: **surface `'a` lifetime-annotation
> syntax on types.**

## Why this exists

TY4 wired the lifetime machinery into `PASS_BORROW_CHECK` (it runs on every
compile), but it currently has nothing to act on:

> surface `'a` lifetime-annotation syntax on types is not built -- without it
> elision produces no constraints on ordinary programs, so inter-procedural
> *lifetime* checking has nothing to act on yet.

Concretely: `Type.lifetimes[]` is never populated from source. So
`lifetime_elision_apply` only ever sees borrow types with `n_lifetimes == 0`,
applies Rule 1 (give each borrow parameter its *own fresh* lifetime), and the
resulting constraint set is always trivially satisfiable -- `lifetime_has_cycle`
can never fire on a real program, and no two borrows are ever required to relate.
The escape check (TUR-E0105) is scope-depth based and does **not** depend on this
syntax; it is fully enforced today. What is missing is the ability for a
programmer to *name* lifetimes and tie an output borrow to a specific input
borrow across a call boundary -- i.e. genuine inter-procedural lifetime checking.

This plan adds the surface→`Type` plumbing that turns the dormant machinery into
an enforced discipline.

## Current state (verified integration points)

| Concern | Location | State |
|---|---|---|
| Apostrophe lexing | `src/compiler/reader.c:160` (`is_sym_start`), `:179` (`is_sym_cont`) | `'` (ASCII 39) is a **valid symbol char**, so `'a` already lexes as a single symbol token named `'a` (via `read_symbol_or_minus`). **No new token / lexer change needed.** Note `reader.c:509` (`read_quote`) handles the *quote macro* `'x` -> `(quote x)` only at the start of an expression, not mid-symbol. |
| Lifetime fields on `Type` | `src/compiler/types.h:365-378` | `LifetimeId lifetimes[MAX_TYPE_LIFETIMES]; uint8_t n_lifetimes;` already exist (`MAX_TYPE_LIFETIMES == 4`). Helpers `type_has_lifetime` / `type_first_lifetime` at lines 547-554. There is **no** `has_explicit_lifetime` flag -- `n_lifetimes > 0` is the "has a lifetime" predicate. (Separately, `MAX_LIFETIMES` in lifetimes.h is 8, the per-function cap.) |
| Borrow type constructors | `src/compiler/types.c` / `types.h:879-917` | `type_ref_immut` / `type_ref_mut` build `&T` / `&mut T` with `n_lifetimes == 0`. Lifetime-carrying variants **already exist**: `type_ref_immut_lifetime` / `type_ref_mut_lifetime` (types.h:900/910) set `lifetimes[0]` + `n_lifetimes = 1` -- LS1 calls these. |
| Param/return type parsing | `src/compiler/elab_types.c`, `src/compiler/elab_fns.c` (`elab_defn`) | Where `&T`, `&mut T`, `:ref` types are built. A trailing/leading `'a` is not consumed. **Borrow *return* types currently hard-error** ("unsupported return type keyword 'ref<int>'") -- a prerequisite gap. |
| Per-function lifetime context | `src/compiler/expr.h` `struct FnDef` -- `LifetimeContext lifetime_ctx`, `Type *param_types` | `lifetime_ctx` holds IDs + constraints but has **no name→ID map**. |
| Machinery | `src/passes/lifetime_elision.c`, `src/passes/lifetimes.c`, `src/passes/borrow_check.c` (`lifetime_check_program`) | Complete and wired; runs from `src/main.c` in the `PASS_BORROW_CHECK` case. |
| Diagnostics | `src/compiler/diag.{h,c}` | `TUR-E0105` (borrow escapes) and `TUR-E0106` (cyclic lifetime) already registered. |
| Tests | `tests/fixtures/borrow-*`, `tests/fixtures/errors/*` (`expected.diag`), `tests/lifetime_unit.c` (ctest `lifetime_unit`) | Escape fixtures + unit tests for elision/cycle exist. |

## Design decisions to settle first

1. **Surface placement of `'a`.** Two options, pick one for consistency:
   - Rust-style prefix on the target: `&'a int`, `&mut 'a int`.
   - Keyword-style for the `:ref` family: `(x :ref 'a)` / a `^'a` attribute.
   The `&`/`&mut` reader forms (`elab_borrow_immut` / `elab_borrow_mut`) are
   *expression* forms, not type forms; lifetime syntax lives in **type
   annotation position** (param `: &'a int`, return `: &'a int`), so this is an
   `elab_types.c` concern, not a borrow-expression concern.
2. **Lifetime quantifier declaration.** Does a function implicitly quantify over
   every `'a` it mentions (Rust 2018 elision style), or must it declare them
   (e.g. a `[ 'a 'b ]` lifetime-param list alongside the existing `[f]`
   kind/type-param list in `elab_defn`)? Recommend **implicit quantification**
   to start -- every distinct `'a` in a signature is a fresh function lifetime
   parameter -- matching how type variables already work.
3. **Storage cap.** `Type.lifetimes[MAX_TYPE_LIFETIMES]` is currently sized 4
   (types.h); `MAX_LIFETIMES` (lifetimes.h, the per-function cap) is 8. `&'a &'b T`
   needs only 2, so 4 is likely plenty -- decide whether to keep 4 or unify on 8.

> These three are genuine product/语言-surface choices. Resolve them (an
> `AskUserQuestion` at kickoff) before writing code -- they change the parser
> shape.

## Phases

- **LS0 -- Name→ID resolution in `LifetimeContext`.** Add a small lifetime-name
  table (parallel `const char *names[]` / `LifetimeId ids[]`, or reuse the
  symbol-interning table) so a `'a` string resolves to a stable `LifetimeId`
  within one function, and a second `'a` in the same signature resolves to the
  *same* ID. Add `lifetime_context_intern(ctx, name) -> LifetimeId`.
  *Done when:* unit test in `tests/lifetime_unit.c` shows repeated `'a` interns
  to one ID and `'b` to a different one.

- **LS1 -- Parse `'a` in type-annotation position.** In `elab_types.c`, when a
  borrow type (`&T`, `&mut T`, `:ref`) is followed by (or prefixed with, per the
  LS-design decision) a symbol whose name begins with `'`, intern it via the
  enclosing function's `LifetimeContext` and store the `LifetimeId` in
  `Type.lifetimes[0]` and setting `n_lifetimes = 1` -- preferably by calling the
  existing `type_ref_immut_lifetime` / `type_ref_mut_lifetime` constructors.
  Thread the active `LifetimeContext` into the
  type-annotation parser (currently it has no access to it).
  *Done when:* `(defn f [x : &'a int] : int ...)` elaborates with
  `param_types[0].lifetimes[0]` populated; a fixture asserts via `emit-c` /
  debug dump.

- **LS2 -- Allow borrow *return* types.** Remove the "unsupported return type
  keyword" hard-error for `&T` / `&mut T` / `:ref` in `elab_defn`'s return-type
  block so a function can declare `: &'a int`. This is a prerequisite for any
  inter-procedural lifetime relationship to be expressible.
  *Done when:* a function with a borrow return type elaborates instead of
  erroring, and the returned borrow flows through codegen (or is explicitly
  rejected with a clear message if codegen support is deferred).

- **LS3 -- Feed explicit lifetimes into elision + solving.** With
  `param_types` / return `Type` now carrying explicit lifetimes,
  `lifetime_elision_apply` (already rewritten in TY4) will skip Rule 1 for
  borrows that already carry a lifetime (`n_lifetimes > 0`), and the constraints
  it derives will
  reflect the programmer's intent. Verify `lifetime_check_program` rejects a
  signature whose explicit lifetimes form a cycle (TUR-E0106) and accepts a
  well-formed one. Add the outlives constraints implied by an output lifetime
  equal to an input lifetime.
  *Done when:* a hand-written cyclic signature triggers TUR-E0106 end-to-end
  (not just in the unit test).

- **LS4 -- Inter-procedural borrow checking at call sites.** Extend
  `borrow_check.c` so that when a function with explicit-lifetime borrow params
  is *called*, the lifetimes of the actual arguments are unified/constrained and
  a returned borrow's lifetime is tied to the correct argument. This is the
  payoff: a borrow returned from a call that outlives its source argument is
  rejected. (Scope-check carefully against the existing scope-depth escape check
  so the two cooperate rather than double-report.)
  *Done when:* an inter-procedural escape (return a borrow tied to `'a`, then
  let it outlive the `'a` argument) is rejected, and the valid case compiles.

- **LS5 -- Tests + docs.**
  - Happy fixtures: explicit `'a` that checks (`tests/fixtures/lifetime-*`).
  - Error fixtures (`tests/fixtures/errors/*` with `expected.diag`):
    `TUR-E0106` cyclic signature; an inter-procedural borrow-outlives violation.
  - Extend `tests/lifetime_unit.c` for LS0 interning and LS3 explicit-lifetime
    elision behavior.
  - Update `docs/guides/substructural-types-guide.md` "Borrows and Lifetimes"
    section: replace the "explicit `'a` syntax is not yet wired" note with the
    real syntax and examples.
  - Update `docs/typing-gap-plan.md` TY4 "honest scope note" to point here and
    mark the surface-syntax follow-up done.
  *Done when:* `bash tests/run.sh` reports zero `FAIL`, `ctest` green, zero
  `expected.c` drift.

## Non-goals

- Lifetime annotations on `defstruct` fields (struct-held borrows). The `Type`
  fields support it, but the checking story is larger; defer to a follow-up.
- Higher-ranked lifetimes (`for<'a>`), variance, and subtyping beyond the simple
  outlives relation the current solver models.
- Changing the scope-depth escape check (TUR-E0105) -- it stays as the
  always-on, syntax-independent backstop.

## Risks

- **Disambiguation.** `'a` is already a legal symbol everywhere (reader.c:160/179).
  Restricting lifetime interpretation to *type-annotation position* avoids
  breaking any existing program that happens to use a `'`-prefixed identifier as
  a value symbol. LS1 must be careful to only treat `'a` specially inside the
  type parser.
- **Double-reporting.** LS4 must coordinate with the scope-depth escape check so
  a single violation does not emit both TUR-E0105 and a new lifetime error.
- **Codegen for borrow returns (LS2).** If returning borrows isn't fully
  supported in codegen, LS2 may need to gate borrow-return *type-checking* ahead
  of *codegen*, or explicitly defer codegen with a clear diagnostic.

Since: planned 2026-05-30
