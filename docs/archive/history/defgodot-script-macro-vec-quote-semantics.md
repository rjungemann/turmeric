# `defgodot-script` Macro Fights List Quote-vs-Value Semantics on VEC Items

> **Status:** Closed / archived 2026-06-25. Original symptom (Gap #1)
>   fixed in `tur`; the remaining language gaps surfaced by the
>   investigation now live in three focused follow-up reports linked
>   below.
> **Severity (historical):** Medium.
> **Discovered:** 2026-06-25.

---

## What was investigated

A `defgodot-script` macro for the turmeric-godot binding tried to
accept the plan's preferred surface

```turmeric
(defgodot-script Player :extends Node2D
  :exports ((speed : float 200.0))
  :signals ((hit (damage : int)))
  ...)
```

and could not, despite multiple attempts to thread the data through
`quote` / `defmacro` / `(list ...)`. The investigation isolated **four
independent Turmeric language-level gaps** that contributed to the
failure.

## Resolution

**Gap #1 closed in `tur` on 2026-06-25.** `(quote sym)` now produces a
first-class `:Sym` literal instead of recursing into elaboration of
the wrapped symbol. The change mirrors the `F_KEYWORD` lowering in
two places:

- `src/compiler/elab_toplevel.c::F_QUOTE`
- `src/compiler/elab_call.c::e->sym_quote`

The quoted symbol is captured by interned-pointer identity, not by
binding lookup, so `'foo` is a value of type `:Sym` regardless of
whether `foo` is in scope. DSL helper defns can now build AST nodes
that contain symbol values.

`bash tests/run.sh` on clean main was `1732 passed, 103 failed`;
with the fix applied it is `1732-1733 passed, 102-103 failed`
(the one-fixture difference is the pre-existing flaky `hamt-delete`
fixture, not a regression).

**Gaps #2, #3, #4 split into focused reports:**

- [list-macro-quote-vs-syntactic-symbol.md](../reported/list-macro-quote-vs-syntactic-symbol.md)
  -- the asymmetry between `(list bare-sym ...)` (preserves syntactic
  symbol) and `(list 'sym ...)` (now a `:Sym` value, not a callable
  AST head).
- [macro-args-elaborated-before-expansion.md](../reported/macro-args-elaborated-before-expansion.md)
  -- macros don't receive their args as opaque AST; the elaborator
  resolves bare identifiers at the call site first.
- [nested-vec-literals-collapse-to-runtime-vec.md](../reported/nested-vec-literals-collapse-to-runtime-vec.md)
  -- inner `[...]` inside an outer `[[...] [...]]` collapses into a
  runtime homogeneous `Vec` value at outer-call construction, both
  losing AST navigability and tripping `tur-vec-homog__` on mixed-type
  contents.

Each is independent; closing any one is useful, closing all three
would unlock the parent plan's preferred surface.

## Why this stays archived rather than as an open report

The proximate symptom ("the defgodot-script macro doesn't work") was
that the macro author had a single failing attempt and could not tell
which substrate gap to file against. Now that the gaps are
characterised individually, the symptom is no longer the right unit
of tracking. The three follow-ups are.

## See also

- [godot-language-binding-plan.md](../upcoming/v1/godot-language-binding-plan.md)
  -- the parent plan's outstanding-work entry pointing at the
  follow-ups.
- The turmeric-godot binding ships a minimum-viable `defgodot-script`
  shell macro that wraps the body in `(do ...)` -- the richer
  vec-literal surface waits on the follow-up reports landing.
