---
title: U5 c-dsl / glsl -- Fix-encoded IR uplift
category: Planning -- Spice uplift (U5 carve-out)
description: Re-express the c-dsl and glsl IRs as `Fix F` so pretty-print and
  codegen passes become `cata`s instead of hand-rolled recursion with per-node
  accessor scaffolding. Carved out of
  `spices-type-features-uplift-plan.md` (Phase U5; archived 2026-06-22 at
  `docs/archive/spices-type-features-uplift-plan.md`) because c-dsl and glsl share
  enough structure that they want a coordinated design pass -- and they have
  more friction (P4 `match-fix` sugar; algebra-shape questions on glsl
  statements) than the regex/scscm/template siblings.
status: OPEN -- proposed.
---

# U5 c-dsl / glsl -- Fix-encoded IR uplift

## Goal

Replace the hand-rolled recursive IR walks in `c-dsl` and `glsl` with a
`Fix F` encoding, so the pretty-printer and codegen become `cata` over a
shared functor. The other U5 targets (regex DONE, scscm, template) stay in
the parent plan -- they don't share this design space.

## Why carve this out

The parent uplift plan (Phase U5) has five targets that all happen to
"hand-roll a recursive IR." But the targets are not uniform:

- **regex (DONE)** -- shipped; one constructor family, one
  `re-cata` traversal.
- **scscm, template** -- straightforward AST walks; the parent plan's
  one-paragraph description is enough scaffolding.
- **c-dsl, glsl** -- *not* straightforward. The IR has multiple syntactic
  categories (expressions, statements, declarations, types), the
  pretty-printer threads precedence/associativity context, codegen has
  layout/ABI obligations, and the algebra dispatch on a single sum is
  exactly the case P4 (`match-fix` sugar) is meant to make ergonomic.
  Both spices want the same functor shape (typed expression / statement /
  decl trees over a C-family grammar) so designing them once and reusing
  the design is cheaper than designing them twice.

Splitting them out keeps the parent plan honest -- it stops listing
"convert c-dsl to Fix" as a single bullet when in practice it's a
multi-week structural change -- and gives the c-dsl/glsl work room for the
detail it actually needs.

## Non-goals

- No new pretty-printer features. Output must round-trip byte-identical
  against the existing fixture corpus until the rewrite is complete; new
  formatting is a separate task.
- No codegen semantics changes. ABI, layout, name mangling, inline-C
  passthrough stay bit-identical.
- Not a stdlib change. `Fix`/`cata`/`ana` live in `stdlib/fix.tur`
  already; this plan consumes them.
- No `glsl` <-> `c-dsl` IR unification. They share *shape* (Fix over a
  C-family grammar) but the functors stay distinct -- glsl has swizzles,
  layout qualifiers, sampler types; c-dsl has pointers, unions,
  preprocessor escape hatches. A premature merge would force every node
  to carry the union of both vocabularies.

## Prerequisites

| Prereq | Status | Notes |
|---|---|---|
| `stdlib/fix.tur` (`roll`/`unroll`/`cata`/`ana`) | DONE | Consumed as-is. |
| P4 `match-fix` sugar | OPEN (parent plan) | Not a hard blocker -- a verbose `cata` body is workable, just noisy. Land before the c-dsl conversion if it ships first; otherwise convert with verbose `(match (unroll layer) ...)` and tighten when P4 lands. |
| P5 negative-fixture support in `tests/run.sh` | OPEN (parent plan) | Not required -- U5's deliverable is round-trip parse->print, not "should fail to compile." |
| c-dsl variadic builders (parent plan U6) | DONE 2026-06-22 | Builder surface is stable, so the underlying IR can be reshaped without churning the public builder API. |

## Targets

### Target 1 -- `c-dsl`

Today: hand-rolled IR with per-node accessor functions; `c_dsl__pp.c`
walks it with explicit recursion; `c_dsl__codegen.c` does the same.

Proposed shape:

```turmeric
;; Functor parameterized over the recursive position `a`.
(defdata CExprF [a]
  (CLit     [lit  : CLit])
  (CVar     [name : cstr])
  (CCall    [head : a   args : (List a)])
  (CBinop   [op : COp  lhs : a  rhs : a])
  (CIndex   [arr : a  idx : a])
  ;; ... etc
  )

(defdata CStmtF [a]
  (CExprStmt   [e : a])      ;; `a` here is a CExpr position
  (CReturn     [e : (Option a)])
  (CIf         [c : a  t : (List a)  e : (List a)])
  ;; ...
  )

(deftype CExpr (Fix CExprF))
(deftype CStmt (Fix CStmtF))
```

Open design questions to settle in the first c-dsl iteration:

- **One functor or two?** Statements embed expressions but not the other
  way around. Two functors with a `CExpr`-shaped slot inside `CStmtF`
  reads cleanly; one mega-functor over a tagged union of expr/stmt is
  noisier but lets a single `cata` cover the whole tree. Lean toward
  two; revisit if the pretty-printer ends up duplicating boilerplate
  across the two algebras.
- **Where does precedence context live?** The pretty-printer threads a
  precedence-level argument. Options: (a) a `cata` whose carrier is
  `int -> Doc`, (b) a paramorphism that sees the unrolled child layer,
  (c) annotate each `CBinop` node with its operator's precedence at
  construction. Prefer (a) -- it keeps the algebra closure-shaped and
  reuses `cata` -- unless the closure size blows out the typeclass
  result-carrier path that landed for `derive-json`.
- **Inline-C escape hatch.** c-dsl already has a `CRaw` / `CInlineC`
  node that carries a string. Keep it -- making it the explicit escape
  valve is preferable to forcing every printf-style construct through
  the typed grammar.

Deliverables (in order):

1. The two functors and `cata`-based pretty-printer; the existing
   fixture corpus round-trips byte-identical.
2. Codegen ported to a `cata` whose carrier is the C-emission state
   monad (or its plain-tuple equivalent if the monad surface is too
   heavy for this pass).
3. The builder API (`c-defn`, `c-defstruct`, etc., shipped under U6)
   still constructs the same surface cstrs; internally they now build
   `Fix CExprF` / `Fix CStmtF` and call into `pp-cata`. The cstr
   output of every builder is unchanged.
4. A "would generalize" memo at the end -- what the glsl pass should
   copy, what was specific to c-dsl.

### Target 2 -- `glsl`

Same shape as c-dsl, but the functor has glsl-specific vocabulary:
swizzles, sampler types, layout qualifiers, vector/matrix builtins,
`in`/`out`/`uniform` storage qualifiers. The statement category is
slimmer (no goto, no switch fallthrough nuance).

Ordering: ship c-dsl first, then port glsl using the memo from c-dsl's
last deliverable. The functor design will be the main carryover; the
algebra shapes are mostly mechanical once c-dsl's pretty-printer
precedence handling is settled.

Glsl-specific design questions:

- **Swizzle nodes.** `v.xyz`, `v.rgba`, `v.stpq` -- one node carrying
  the swizzle string, or four constructors per set? One node; the
  set is a property of the source vector's element semantics, not the
  AST.
- **Sampler types and layout qualifiers.** These belong in the *type*
  grammar, not the expression grammar. A separate `GlslTypeF` functor
  paralleling `CExprF`/`CStmtF` keeps the divisions clean.

## Sequencing

```
c-dsl IR redesign (functor + cata pp)
  -> c-dsl codegen ported to cata
    -> c-dsl builder surface re-points internals (no public-API change)
      -> c-dsl "would generalize" memo
        -> glsl IR redesign (reuse memo)
          -> glsl pp + codegen ported to cata
```

Each arrow is its own PR within the same spice. c-dsl's three PRs land
strictly serially (later PRs depend on earlier signatures). Glsl can
start once c-dsl's memo lands -- it doesn't have to wait for c-dsl's
builder PR.

## Validation

Per spice:

1. The spice's own `tests/` directory stays green at every PR.
2. Pretty-printer round-trip on the full fixture corpus is
   byte-identical to pre-rewrite output (no whitespace drift, no
   parenthesization changes). Bit-identical is the gate.
3. Codegen output (the emitted C / GLSL source) is byte-identical
   against the existing snapshot corpus.

If P4 `match-fix` lands during this work, refactor the algebra bodies
to use it in a follow-up PR per spice -- not interleaved with the
structural change.

## Risks

- **Algebra carrier blow-up under the typeclass result path.** If the
  pretty-printer carrier ends up shaped like `int -> Doc` (a closure
  returning a closure), the same parameterized-result codegen path that
  bit `derive-json` may bite again. Mitigation: lower the carrier to a
  plain `(Pair int Doc)`-style tuple if the closure form regresses.
- **Hidden fixture-format drift.** Round-trip byte-identity is strict;
  even an extra trailing newline counts. Expect one or two PR rounds
  spent on whitespace-only differences. Don't relax the bit-identity
  gate -- relax the printer instead.
- **Glsl is "almost c-dsl" but isn't.** Resist the urge to share the
  functor mid-way through glsl's port; it will look attractive after
  c-dsl ships and will cost more than it saves.

## Out-of-scope follow-ups

- Annotating the IR with source locations for better diagnostics --
  a `Cofree`/`Ann` wrapper over the same functor. Useful but
  orthogonal.
- A typed expression layer (`Expr ty`) on top of the syntactic IR --
  belongs in a separate "c-dsl type system" plan if it ever lands.
- Sharing a `Doc` pretty-printer combinator library between c-dsl and
  glsl. Worth doing *after* both spices are on `cata` and the carrier
  shape has settled.
