# Nested VEC Literals Inside Macro Args Collapse To Runtime Homogeneous `Vec` Values

> **Status:** Resolved 2026-06-25 -- not reproducible in current main.
> **Severity:** Medium -- breaks the most ergonomic DSL surface shape
>   (vec literals of vec literals as a declaration list). Any DSL
>   author wanting to consume nested `[...]` shapes at expansion time
>   has to drop to flat positional args or string-keyed records.
> **Discovered:** 2026-06-25 (spun out from
>   [defgodot-script-macro-vec-quote-semantics.md](./defgodot-script-macro-vec-quote-semantics.md)
>   gap #4).
> **Resolution:** Both claimed behaviours pass in current main (likely
>   landed alongside `macro-args-elaborated-before-expansion` and
>   `list-macro-quote-vs-syntactic-symbol`):
>
>   1. `(defmacro outer [exports] nil)` paired with
>      `(outer [["speed" "float" 200.0]])` builds clean -- the
>      heterogeneous inner vec literal is *not* run through
>      `tur-vec-homog__` at the call site. The homog check fires only
>      when an inner `[...]` actually reaches value position.
>   2. `(first (first (rest exports)))` and the deeper navigation
>      patterns from the report return the expected AST elements,
>      including string and float leaves out of a mixed inner vec.
>
>   What still trips `tur-vec-homog__` is a macro that returns a
>   heterogeneous inner vec literal *as a value* (e.g.
>   `(defmacro outer [exports] exports)` over a mixed `[...]`) -- that
>   is the language's intentional homogeneity rule for `vec-of`, not
>   the bug filed here. Heterogeneous fixed-arity payloads still need
>   `tupleN`, the proposed `#row{...}` row type, or list/cons surface.
>
>   Pinned by `tests/fixtures/macro-arg-nested-vec-literal-ast-passthrough/`.
>   Archived to `docs/archive/`.

---

## Summary

A macro arg shaped like `[[a b c] [d e f]]` arrives at the macro with
the outer `[...]` reachable as AST (its elements can be navigated
with `first` / `rest`) but the **inner** `[...]` collapse into runtime
homogeneous `Vec` values. The inner vec's elements cannot be plucked
with `first` / `rest` at expansion time, and any mixed-type contents
trip the homogeneity check in `tur-vec-homog__`.

Concretely, given:

```turmeric
(defmacro outer [exports]
  ;; exports arrives as AST; (rest exports) navigates fine.
  ;; But (first exports) returns a runtime Vec, not an AST node.
  ...)

(outer [["speed" "float" 200.0]
        ["health" "int"   100]])
```

the macro can split the outer list into two items, but each item is
an opaque `Vec` value. `(first item)` does not return `"speed"`;
either it errors or it returns the wrong thing depending on which
construction path was taken.

Even worse: the *literal* itself fails to construct before reaching
the macro -- `["speed" "float" 200.0]` mixes `cstr` and `float`,
which `tur-vec-homog__` rejects with *"function 'tur-vec-homog__'
arg 2: expected tyvar, got int"* (or `got cstr`, etc.) at the
elaboration of the outer call site.

So even before AST navigation matters, the mixed-type inner vec
literal is a non-starter.

## Why this matters

The most natural DSL surface for declarations -- one row per record,
columns positional inside `[...]` -- requires either:

- Nested vec literals that survive as AST through macro arg passing.
- Heterogeneous-typed vec literals that don't run through homog
  typing in macro-arg position.

Without one of those, DSL authors fall back to alternatives that read
worse:

```turmeric
;; Cannot use:  [["speed" "float" 200.0] ["health" "int" 100]]
;; Workarounds:

;; Flat positional (loses row grouping):
:exports "speed" "float" 200.0 "health" "int" 100

;; Strings only (gives up the value's natural type):
:exports [["speed" "float" "200.0"]
          ["health" "int"   "100"]]

;; Cons-list with explicit constructors (lots of ceremony):
:exports (list (decl "speed" "float" 200.0)
               (decl "health" "int"   100))
```

The flat positional shape is what the
[turmeric-godot defgodot-script MV
shell](../upcoming/v1/godot-language-binding-plan.md) workaround
documents users toward.

## Minimal repro

```turmeric
;; FAILS at outer-call construction: tur-vec-homog__ rejects the
;; mixed cstr / float inner vec.
(defmacro outer [exports] nil)
(defn main [] : int
  (outer [["speed" "float" 200.0]])
  ;;       ^^^^^^^ ^^^^^^^ ^^^^^
  ;; error: function 'tur-vec-homog__' arg 2: expected tyvar, got cstr
  0)
```

```turmeric
;; Even with homogeneous inner contents, the inner vec is opaque to
;; macro-time navigation -- (first item) does not return "speed".
(defmacro outer [exports]
  `(println ~(first (first (rest exports)))))
(defn main [] : int
  (outer [["a" "b" "c"]
          ["d" "e" "f"]])
  ;; expected: prints "d". Actually: panics or returns wrong shape.
  0)
```

A clean fixture under `tests/fixtures/macros/` would pin both
behaviours so future regressions are visible.

## Fix direction

Two viable shapes:

1. **Vec literals stay reachable as AST through nesting.** An inner
   `[a b c]` inside an outer `[[...] [...]]` is treated as a vec-AST
   node, not lowered to a runtime `Vec` value during outer
   construction. Homogeneity typing applies only at AST realisation
   time (when the vec is actually used as a value).
2. **A heterogeneous tuple / row literal type that doesn't run
   through homog typing in macro-arg position.** `tuple3` works
   today for fixed arity; a heterogeneous-vec analog (or a
   `#row{:k v}` shape that already exists -- see
   [P0 typed-field row literals shipped](../../../.claude/projects/-Users-rjungemann-Projects-turmeric/memory/project_p0_typed_field_rows.md))
   could fill this gap.

(1) is the more general fix; (2) is the smaller addition that already
has machinery in the codebase.

## Notes

- Independent of
  [macro-args-elaborated-before-expansion.md](./macro-args-elaborated-before-expansion.md)
  and
  [list-macro-quote-vs-syntactic-symbol.md](./list-macro-quote-vs-syntactic-symbol.md).
  Even if those are fixed, this gap still blocks the nested-vec
  surface.
- Closing this gap, combined with the macro-arg AST passthrough
  (above), would let `defgodot-script` ship the plan's preferred
  surface:

  ```turmeric
  (defgodot-script Player :extends Node2D
    :exports [(speed   : float 200.0)
              (texture : Texture2D)]
    :signals [(hit (damage : int))
              (died)]
    ...)
  ```
