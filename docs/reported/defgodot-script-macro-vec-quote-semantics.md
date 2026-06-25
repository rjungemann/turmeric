# `defgodot-script` Macro Fights List Quote-vs-Value Semantics on VEC Items

> **Status:** Investigated 2026-06-25; root cause characterised below.
>   The defgodot-script ergonomic surface is workaroundable today by
>   using string decl names (`["speed" "float" 200.0]` rather than the
>   plan's `(speed : float 200.0)`); the underlying language gap is
>   real and worth its own fix pass.
> **Severity:** Medium -- macro authors building DSLs that want to
>   accept bare *symbol names* (as opposed to strings or expressions)
>   in their surface cannot make those names round-trip through the
>   expander. Workaround: take strings.
> **Discovered:** 2026-06-25

---

## Summary

When a macro body emits a list form that contains VEC items, the
quoting model used by the macro expander vs. the value-construction
model used by ordinary list literals do not line up. The macro author
ends up unable to decide a single posture -- quote the whole template
and lose access to value-built VECs spliced from the macro
environment, or build the list with value semantics and lose the
ergonomic literal form for the surrounding shape.

Concretely the reporting agent hit this trying to write something
along the lines of:

```turmeric
(defmacro defgodot-script [name & body]
  ;; wants to emit a form that includes VEC items inside, where some
  ;; VECs are literal and some come from the macro's lexical env
  ...)
```

and observed that whichever way the template was written, one of the
two VEC sources came out wrong: literal-shaped VECs were fine but
spliced VECs were quoted (or vice versa).

## Why this matters

`defgodot-script` is the kind of DSL we want spices (turmeric-godot,
turmeric-raylib, ...) to ship. If macro authors have to actively avoid
VEC items in the DSL surface to dodge a quoting-model gap, the macro
system is leaking implementation detail into every spice that wants a
declarative entry point. This is also a recurring shape -- any DSL
that wants record-ish / vector-ish literals in its surface will hit
the same wall.

## Root cause

The investigation isolated three concrete pieces of behaviour:

### 1. `(quote sym)` does not suppress elaboration

Inside a `defn` body, `(list 'do "hi")` errors with TUR-E0003
"unbound symbol 'do'". The elaborator walks inside `(quote ...)` and
resolves the wrapped symbol. The same expression inside a
`defmacro` body type-checks at definition time but errors the moment
the macro is *expanded* at a real call site. Special forms (`if`,
`do`) and stdlib names (`println`) all hit this.

This is the proximate cause of "fights quote-vs-value semantics" --
`'sym` produces a `:Sym` runtime value, but the AST position
expects a syntactic-symbol node, and the elaborator does not bridge
the two.

### 2. Bare-symbol references DO work in `(list ...)` inside defmacro

`(defmacro mk [] `(println "hi"))` works (backquote with bare symbol).
`(defmacro mk [] (list if true 1 2))` works (bare `if` because `if`
is a bound special-form keyword). The stdlib `cond` macro relies on
this pattern.

The mechanism: at expansion time, the `list` macro preserves the
syntactic form of bare symbols (they stay symbols in the resulting
AST). `'sym` does *not* go through the same path -- it produces a
value the elaborator later tries to dereference.

### 3. Macro args are elaborated, not preserved as raw AST

Critically: macros do NOT receive their arguments as opaque AST
tokens. The arguments are first walked by the elaborator. Calling
`(mk (speed : float 200.0))` errors with "unbound symbol 'speed'" --
the macro never gets a chance to consume `speed` as a name because
the elaborator already failed on the call site.

This forces DSL surfaces to use *expressible values* (strings,
numbers, keywords) or *bound symbols* in argument positions.
Surfaces that want bare identifiers as "names" (like the plan's
`(speed : float 200.0)`) cannot be implemented as written without
the user pre-quoting at the call site.

## Minimal repro

```turmeric
;; FAILS -- 'do is elaborated (TUR-E0003 unbound 'do' at expansion)
(defmacro mk [] (list 'do "hi"))
(defn main [] : int (mk))

;; WORKS -- bare bound symbol preserved by list's macro semantics
(defmacro ok [] `(println "hi"))
(defn main [] : int (ok))

;; FAILS at the CALL site -- macro args are elaborated before expansion
(defmacro mk2 [decl] `(println ~(sym->str (first decl))))
(defn main [] : int (mk2 (speed : float 200.0)))
;; ^ "unbound symbol 'speed'" -- the elaborator never lets the macro see it
```

Worth landing as a fixture under `tests/fixtures/macros/` to pin the
chosen semantics.

## Workaround for defgodot-script today

Use string decl names so nothing is unbound at the call site:

```turmeric
(defgodot-script "Player"
  :exports [["speed" "float" 200.0]
            ["health" "int"   100]]
  :signals [["hit"] ["died"]]
  body...)
```

A recursive variadic macro built entirely with backquote +
bare-bound-symbol references (no `'sym`, no `(quote sym)`) can then
emit the inner `(godot-export ...)` calls. This is the path Phase #3
of the godot-language-binding-plan's outstanding work will take when
it lands.

## Fix direction (language-level)

If we want the plan's original `(speed : float 200.0)` surface to
work, one or more of these has to change:

1. **`(quote ...)` should suppress elaboration of its argument.** A
   :Sym value in head-of-list position should round-trip back to a
   syntactic symbol the elaborator can dispatch on, or the elaborator
   should simply not look inside `quote`. This unblocks helper
   defns that build AST.
2. **Macro arguments should be passed as unelaborated AST.** Today
   they go through the type-checker first. A `defmacro [^syntax decl]`
   marker (or a global "macros take AST" rule) would let DSL surfaces
   accept bare identifier names.
3. **Or: explicitly document and bless the workaround** -- macro DSLs
   take strings/keywords/expressions, never bare identifiers -- and
   land a `tests/fixtures/macros/quote-vs-value/` fixture pinning
   the semantics so this stops surprising people.

Doing (1) alone fixes the helper-defn path. Doing (2) alone fixes the
surface ergonomics. Doing both fixes everything.

## Notes

- Not blocking v1 -- the string-decl workaround ships fine and the
  Phase #3 macro can be written around it.
- The investigation also surfaced that `tur/sym-dynamic`'s `str->sym`
  is NOT a substitute for the missing surface: it returns a `:Sym`
  value (interned runtime symbol), not the syntactic-symbol AST node
  the macro expander wants in head position. They are different
  representations.
