# `(list bare-sym ...)` Inside `defmacro` Relies On Syntactic-Symbol Passthrough That `'sym` Does Not Provide

> **Status:** Reported, not yet investigated.
> **Severity:** Low-medium -- subtle but real asymmetry in how
>   `(list ...)` treats bare symbol names vs. quote-form symbol values
>   inside macro bodies. Affects macro authors trying to build AST
>   that contains symbols which are NOT already bound in scope.
> **Discovered:** 2026-06-25 (spun out from
>   [defgodot-script-macro-vec-quote-semantics.md](./defgodot-script-macro-vec-quote-semantics.md)
>   gap #2).
> **Related (closed):** Gap #1 of the parent report
>   (`(quote sym)` now produces a `:Sym` literal) closed 2026-06-25 --
>   that change makes this asymmetry more visible.

---

## Summary

Inside a `defmacro` body, two superficially-equivalent ways to embed
a symbol in the expansion AST behave differently:

```turmeric
;; WORKS -- list's macro semantics preserve bare `if` as a syntactic
;; symbol AST node in the expansion. `if` is a bound special form, so
;; the elaborator accepts the head position.
(defmacro mk-a [] (list if true 1 2))

;; WORKS for the same reason -- backquote captures the bare symbol.
(defmacro mk-b [] `(println "hi"))

;; FAILS post-Gap-#1 -- `'foo` now produces a :Sym value, and Sym is
;; not callable in head position. Before Gap #1 was fixed, this used
;; to "work" only when `foo` was bound in scope.
(defmacro mk-c [] (list 'foo 1 2))
```

The stdlib `cond` macro and its kin lean on the bare-symbol path:

```turmeric
;; stdlib/macros.tur
(defmacro cond [& clauses]
  ...
  (list if (first clauses) (second clauses)
        (list cond ~@(rest (rest clauses)))))
```

Here `if` and `cond` are bare symbols. The `(list ...)` macro -- a
defmacro itself -- expands into a tcons builder whose elements stay
as syntactic-symbol AST nodes. The elaborator then sees an `(if ...)`
or `(cond ...)` form and dispatches normally.

## The asymmetry

Before Gap #1 closed: `(list 'foo ...)` was elaborated by stripping
the quote and resolving `foo` against scope. If `foo` was bound, the
binding came back; if not, TUR-E0003.

After Gap #1 closed: `(list 'foo ...)` produces a list containing a
`:Sym` runtime value. The elaborator doesn't try to resolve `foo`
anymore -- but the resulting AST has a `:Sym` value in head position,
which the elaborator emits as *"expression in call head has type
`Sym`, which is not callable."*

Neither behaviour gives macro authors a way to build an AST node
containing a symbol *that isn't currently in scope*. The bare-symbol
form only works for already-bound names; `'sym` produces a value
that isn't a callable AST head.

## Why this matters

Any macro that constructs an expansion with a freshly-named function
or method call hits this. Example: a `define-record` macro that
wants to emit `(defn ~accessor-name [r] ...)` for each field -- the
accessor name doesn't exist yet at expansion time, so neither bare
`~accessor-name` (unbound) nor `'~accessor-name` (Sym value, not
callable) does the right thing.

The closest workaround is to use bare `gensym` symbols (which the
elaborator binds during expansion) or to lean on backquote, which
captures bare symbols in callable-AST form. Neither generalises to
arbitrary author-chosen identifiers.

## Fix direction

The clean fix is to give `(list ...)` (and other AST-building forms
like backquote) a way to construct AST nodes from `:Sym` values. The
shape:

```turmeric
;; A new constructor: take a Sym value, return an AST head
;; suitable for use in list / backquote.
(ast-call-head 'foo)    ;; -> AST node that, when in head position,
                        ;;    dispatches like the bare symbol `foo`
```

Or symmetrically, a coercion in the other direction: when a `:Sym`
value lands in a callable-head AST position, look it up in the
enclosing scope at expansion time.

Either approach restores the round-trip: macro authors can carry
symbols around as values (`'sym`, post-Gap-#1) AND splice them into
head positions of generated calls (the new path).

## Minimal repro

```turmeric
;; This used to "work" only when foo was bound; now uniformly fails.
(defmacro mk [] (list 'foo 1 2))
(defn foo [a : int b : int] : int (+ a b))
(defn main [] : int (mk))
;; error: expression in call head has type `Sym`, which is not callable
```

A fixture under `tests/fixtures/macros/` would pin the chosen
semantics.

## Notes

- Independent of
  [macro-args-elaborated-before-expansion.md](./macro-args-elaborated-before-expansion.md):
  that gap is about macro *args*; this one is about macro *bodies*.
- Not blocking v1 -- stdlib's existing macros (`cond`, the various
  `do-m`/`when-let`/`for` shapes) all use bare-bound-symbol patterns
  that still work fine. The gap matters when a DSL author wants to
  emit calls to fresh names the user supplies.
