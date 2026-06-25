# Macro Arguments Are Elaborated Before The Macro Sees Them

> **Status:** Reported, not yet investigated.
> **Severity:** Medium-high -- the most foundational of the three
>   DSL-author gaps spun out of the original
>   `defgodot-script` investigation. Without this, no DSL surface can
>   accept *bare identifiers* in argument positions; surfaces are
>   forced to take strings, keywords, or already-bound expressions.
> **Discovered:** 2026-06-25 (spun out from
>   [defgodot-script-macro-vec-quote-semantics.md](./defgodot-script-macro-vec-quote-semantics.md)
>   gap #3).
> **Related (closed):** Gap #1 of the parent report
>   (`(quote sym)` now produces a `:Sym` literal) closed 2026-06-25.

---

## Summary

Macros do **not** receive their arguments as opaque AST tokens. The
arguments are first walked by the elaborator. Calling

```turmeric
(mk (speed : float 200.0))
```

errors with TUR-E0003 *"unbound symbol 'speed'"* -- the macro never
gets a chance to consume `speed` as a name because the elaborator
fails on the call site first.

This forces DSL surfaces to use *expressible values* (strings,
numbers, keywords) or *bound symbols* in argument positions. Surfaces
that want bare identifiers as "names" -- the natural Lisp/Racket
shape, and the shape the parent
[godot-language-binding plan](../upcoming/v1/godot-language-binding-plan.md)
specs for `defgodot-script` -- cannot be implemented as written
without the user pre-quoting at the call site.

## Why this matters

Every macro DSL that wants record-ish / declaration-ish surfaces hits
this. The pattern the parent plan describes,

```turmeric
(defgodot-script Player :extends Node2D
  :exports ((speed   : float 200.0)
            (texture : Texture2D))
  :signals ((hit (damage : int))
            (died))
  ...)
```

is essentially impossible: `speed`, `Texture2D`, `hit`, `damage`,
`died` are all bare identifiers that the elaborator immediately
resolves and rejects.

The workaround today is to take strings:

```turmeric
(defgodot-script "Player"
  :exports [["speed" "float" 200.0]
            ["texture" "Texture2D" ...]]
  ...)
```

That ships -- the [turmeric-godot defgodot-script MV
shell](../upcoming/v1/godot-binding-aot-plan.md) and its
[upgrade plan](../upcoming/v1/godot-language-binding-plan.md) both
sit on top of it -- but it's user-visible ergonomic debt.

## Minimal repro

```turmeric
(defmacro mk2 [decl]
  ;; would expand to (println "speed") if decl reached the macro intact
  `(println ~(sym->str (first decl))))

(defn main [] : int (mk2 (speed : float 200.0)))
;;                       ^^^^^^^^^^^^^^^^^^^^^
;; error: unbound symbol 'speed'
;; The elaborator walks the call args BEFORE the macro expands;
;; `speed` is rejected before mk2 ever runs.
```

A clean repro fixture under `tests/fixtures/macros/` would pin the
chosen semantics so future regressions are visible.

## Fix direction

Two viable shapes:

1. **`defmacro` takes raw AST.** Universal change: every defmacro
   receives its arguments unelaborated. Cleanest semantics; biggest
   behavioural shift for stdlib code that assumes elaborated args.
2. **Per-parameter `^syntax` marker.** Opt-in -- a defmacro signature
   like `[name ^syntax decl & body]` declares that `decl` is passed
   as AST while other params elaborate normally. Smaller blast
   radius; lets stdlib code keep current semantics and DSL authors
   opt in per parameter.

(2) is the more shippable path -- it avoids breaking the established
patterns the same way the now-closed Gap #1 broke them when applied
universally (see the parent report's empirical note).

## Notes

- Closing this gap alone fixes most DSL ergonomics. It does not
  fix [nested-vec-literals-collapse-to-runtime-vec.md](./nested-vec-literals-collapse-to-runtime-vec.md)
  -- inner `[a b c]` still loses its AST shape inside an outer
  literal even if macro args are AST -- so a complete fix for the
  defgodot-script use case wants both.
- Compare with
  [list-macro-quote-vs-syntactic-symbol.md](./list-macro-quote-vs-syntactic-symbol.md):
  that gap is about the stdlib's `(list bare-sym ...)` pattern in
  macro bodies, not about how macro *args* are received. The two are
  independent.
