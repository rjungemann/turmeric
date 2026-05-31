# Map / Vec Data Literals -- Plan (DL0--DL5)

> **Status:** Not started.
>
> **Flag:** `-Xdata-literals` (opt-in; no effect on programs that don't use the
> syntax). All phases gated behind this flag so the feature can land
> incrementally.
>
> **Last updated:** 2026-05-30

---

## Motivation

Constructing maps and vecs in Turmeric today is verbose:

```turmeric
(hamt-of "name" name "age" age "active" 1)
(vec-of x y z)
```

For literal-shaped data with mixed literal/computed values, that boilerplate
adds up fast -- fixtures, default configs, test payloads, builder calls all
read worse than they need to. Adjacent languages (Clojure, EDN, JS, Python,
even Ruby) all have literal syntax for their core collections; Turmeric does
not.

A previous plan ([json-reader-macro-plan.md](json-reader-macro-plan.md))
targeted the narrower "paste a JSON blob" use case. But JSON-only fixes
nothing for the more common case: a map/vec with literal *shape* but
some computed *values*. The deeper gap is the lack of evaluating
collection literals.

This plan adds reader syntax that constructs HAMT maps and vecs whose slot
values are normal Turmeric expressions, evaluated at runtime in the
surrounding scope.

```turmeric
;; Literal shape, computed values -- no quasiquote needed.
(def payload #map{:name name :age (+ age 1) :active 1})
(def points  [(make-point 0 0) (make-point 1 1) origin])
```

If this lands well, the `#json(...)` reader macro becomes a thin
specialization (literal-only slots) of the same machinery, or can be dropped
entirely in favor of writing the map literal directly.

---

## Design

### The two surface forms

| Syntax | Produces |
|---|---|
| `[e1 e2 e3 ...]` *(expression position)* | Vec: `(vec-of e1 e2 e3 ...)` |
| `#map{k1 v1 k2 v2 ...}` | HAMT map: `(hamt-of k1 v1 k2 v2 ...)` |

The vec form reuses the existing `[...]` reader -- `F_VEC` -- and adds
context-sensitive elaboration (see below). The map form is a brand-new
`#`-dispatch entry that avoids every existing collision.

### Why `[...]` for vecs (Clojure-style overloading)

Turmeric's reader already parses `[...]` uniformly as `F_VEC` regardless
of context (`reader.c:1982`). Parameter lists in `defn`, binding vectors
in `let`/`loop`, and any other `[...]` produce the same `F_VEC` form
tag. The surrounding special form decides how to interpret it.

This is exactly how Clojure resolves the apparent "param list vs vector
literal" collision: there *is* no collision because `[x y]` is always
the same vector data structure. `defn` consumes its second argument as
a binding spec; `let` consumes its first argument as a binding spec;
everywhere else, `[x y]` is just a vector.

The change here is purely **elaboration**: when an `F_VEC` appears in
expression position (i.e. it wasn't grabbed by a binding-form macro
first), lower it to `(vec-of ...)`. No reader change required.

### Why `#map{...}` for maps

The `{...}` slot in expression position is already claimed by
**curly-infix** arithmetic (`{a + b}` -> `(+ a b)`), and `#{...}`
already produces `F_MAP` for effect rows. Both are load-bearing and
should not be touched.

`#map{...}` is a fresh dispatch with no current meaning -- the reader
sees `#`, reads the tag `map`, then `{`, and reads a delimited
sequence. The named-tag spelling pairs symmetrically with `#set{...}`
(see Future work) so both collection literals follow the same
`#<tag>{...}` shape. A reader sees `#map{` and `#set{` and immediately
knows which collection they are getting -- there is no shorthand-vs-named
asymmetry to remember.

Alternatives considered and rejected:

- **`##{...}`** -- type-agnostic and one char shorter, but pairs
  awkwardly with `#set{...}` (the only other reserved collection
  literal). Symmetry between the two outweighs the saved character.
- **`#m{...}` / `#hamt{...}` / `#dict{...}`** -- single-letter `#m`
  is cryptic; the longer forms commit to an implementation detail
  (HAMT) or a non-Turmeric vocabulary word (dict). `#map` matches
  the conceptual type name and is the same length as `#set`.
- **Bare `{...}` with parity disambiguation** (even count -> map,
  odd count -> infix) -- clean in the common case, but the 2-element
  edge case (`{- x}` unary infix vs `{:k v}` map) requires a
  first-form check that adds parsing subtlety for marginal payoff.
- **Reuse `#{...}`** -- already `F_MAP` (effect rows). Sharing the
  surface form would require context-sensitive elaboration over a
  type-system-load-bearing construct. Too risky.

### What the reader produces

| Source | Elaborates to |
|---|---|
| `[1 2 3]` *(expr position)* | `(vec-of 1 2 3)` |
| `[]` *(expr position)* | `(vec-of)` |
| `#map{:a 1 :b x}` | `(hamt-of :a 1 :b x)` |
| `#map{}` | `(hamt-of)` |
| `[#map{:k v} #map{:k w}]` | `(vec-of (hamt-of :k v) (hamt-of :k w))` |

Slots are arbitrary expressions -- variable references, calls, nested
literals, etc. The normal typechecker handles them; the literal is
just sugar over `hamt-of` / `vec-of`.

### "Expression position" for `[...]`

A `[...]` `F_VEC` form is consumed as a **binding spec** (not
elaborated to `vec-of`) when it appears as:

- The parameter list of `defn`, `defmacro`, `fn`, `defmethod`, etc.
- The binding vector of `let`, `loop`, `letfn`, `for`, `doseq`, `when-let`, etc.
- A destructuring pattern inside one of the above.
- The argument list slot of any future macro that opts in via a known
  marker (the elaborator maintains a small allow-list).

In every other position -- function arguments, top-level values,
right-hand sides of `def`, etc. -- `F_VEC` elaborates to `(vec-of ...)`.

The allow-list lives in the elaborator and is the single source of
truth for "what is a binding form." Adding a new binding-form macro
means appending one entry; the rest of the language remains uniform.

### Key forms in `#map{...}`

Keys must be one of:

- A keyword: `:name`, `:age` -- the typical case.
- A string literal: `"name"` -- when interop with `cstr`-keyed maps is needed.
- An int literal: `42` -- for int-keyed maps.

Other forms (variables, calls) as keys are **rejected** in DL0 with a
clear error. Computed keys are rare in practice and allowing them would
make the literal harder to read at a glance ("is `foo` here a key or
a value?"). If a real use case emerges, add a separate form for it
(see Future work) rather than overloading the basic literal.

### Vec element types

`[...]` produces a `vec-of` call with no type annotation; the element
type is inferred from the first element (same as the existing `vec-of`
macro). Mixed-type vecs are rejected by the typechecker, not the reader.

For an explicit element type, write `(vec-of :int 1 2 3)` (current
behavior) or wrap the literal: `(the (Vec :int) [1 2 3])` once that
form exists.

### Reader changes

Two pieces of work:

1. **`#map{...}` dispatch** in `src/compiler/reader.c`: after the
   initial `#`, read the tag identifier (`map`), then require `{`,
   then read a delimited sequence under a new `F_MAP_LITERAL` tag.
   ~15 lines. Structuring this as a tag-dispatch table (rather than
   a hard-coded `map` branch) leaves room for `#set{...}` to slot
   in later without touching this code.
2. **No vec reader change** -- existing `F_VEC` is reused. All work
   for `[...]` happens in the elaborator.

A single new `Form` tag (`F_MAP_LITERAL`) keeps the data literal
distinguishable from `F_MAP` during elaboration.

---

## Phases

### DL0 -- Reader dispatch for `#map{...}`

Add `#map{...}` to the `#`-dispatch table in `src/compiler/reader.c`.
Introduce `F_MAP_LITERAL` in `src/compiler/forms.h`.

- Parse errors (odd map slot count, missing `}`) become `TUR-E0280` /
  `TUR-E0281` reader errors with line/col pointing into the literal.
- Key-form validation: keyword / string / int only; anything else is
  `TUR-E0282`.

### DL1 -- Elaboration for both forms

In the elaborator:

- Lower `F_MAP_LITERAL` to a `hamt-of` call.
- Lower `F_VEC` to a `vec-of` call **when in expression position**
  (i.e. not consumed by a binding-form macro). Maintain the
  binding-form allow-list described above.

Audit existing macros that take an `F_VEC` argument to confirm none
break. Likely-affected: `defn`, `defmacro`, `defmethod`, `fn`, `let`,
`loop`, `letfn`, `for`, `doseq`, `when-let`, `if-let`, `def-struct`
field lists, `defstruct` field lists, attribute argument lists,
pattern-match clause lists. Each of these grabs the `F_VEC` slot
explicitly today; the elaborator change just needs to skip
expression-position lowering for those slots.

Check whether `hamt-of` and `vec-of` already accept the shapes we need.
If not, extend them (see JR1 in the JSON reader plan -- the same
prerequisite applies).

### DL2 -- Sweet-exp interaction

Verify the literals work inside `.tursweet` files alongside indentation,
neoteric, and `$` rest-of-line. The reader changes happen below the
sweet-exp layer, so this should work transparently -- but add a fixture
to lock in the behavior:

```turmeric
#lang sweet-exp

defn build-payload [name :cstr age :int] :ptr<void>
  #map{:name name :age age :active 1}

defn three-points [] :ptr<void>
  [make-point(0 0) make-point(1 1) origin]
```

### DL3 -- Fixtures

Add test fixtures under `tests/fixtures/`:

- `data-literal-map-basic` -- `#map{:a 1 :b 2}` round-trips
- `data-literal-vec-basic` -- `[1 2 3]` in expression position round-trips
- `data-literal-vec-in-defn` -- confirms `[x y]` in `defn`/`let` still
  works as a binding spec (regression guard)
- `data-literal-computed-values` -- `#map{:k (+ 1 2)}` evaluates slot
- `data-literal-nested` -- `[#map{:k 1} #map{:k 2}]`
- `data-literal-empty` -- `#map{}` and `[]` (expression position)
- `data-literal-bad-key` -- `#map{x 1}` produces `TUR-E0282`
- `data-literal-odd-slots` -- `#map{:a 1 :b}` produces `TUR-E0280`
- `data-literal-sweet-exp` -- works inside `.tursweet`

### DL4 -- Docstrings, guide, and JSON-reader relationship

- `;;;` docstrings on `hamt-of` and `vec-of` if not already present;
  reference the literal syntax from each.
- `docs/guides/data-literals-guide.md` covering `#map{...}`, `[...]` in
  expression position, key-form rules, type inference notes, and the
  interplay with `#json(...)` (if that plan also lands -- otherwise
  note that data literals supersede it for non-JSON-sourced cases).
- Update [json-reader-macro-plan.md](json-reader-macro-plan.md): either
  mark it as superseded for the mixed-value case, or scope it down to
  "JSON-source-paste only."
- Update `CLAUDE.md` Sweet-Expression Style section to mention the new
  literal forms (since `.tursweet` is the most common place they will
  appear).

### DL5 -- `just docs` + snapshot audit

Regenerate codegen snapshots affected by elaboration changes, run
`bash tests/run.sh`, confirm zero `FAIL` lines.

The biggest regression risk is DL1's binding-form allow-list -- a missed
entry means a binding spec gets wrongly lowered to `(vec-of ...)`,
breaking the consuming macro. The fixture suite plus full snapshot run
should surface any miss.

---

## Error codes

| Code | Condition |
|---|---|
| `TUR-E0280` | Odd number of slot forms in `#map{...}` (unmatched key) |
| `TUR-E0281` | Unexpected EOF inside `#map{...}` |
| `TUR-E0282` | Invalid key form in `#map{...}` (must be keyword, string, or int literal) |

---

## Non-goals for DL0--DL5

- **Set literals** -- reserved as `#set{...}` (see Future work). Out of
  scope for DL0--DL5 because `Set` is not yet a stdlib collection type.
- **Computed keys** in `#map{...}` -- restricted to literal keys; revisit
  if a real use case emerges.
- **Quasiquote / unquote-splicing** -- a separate, larger feature. Data
  literals cover the common "literal shape, computed values" case
  without needing it.
- **Type annotation on literals** -- e.g. `##<MySchema>{...}`. The JSON
  reader plan reserves analogous syntax (JR2); the same reservation
  can apply here, but is not in this plan.
- **Pattern matching against literals** -- `(match x [#map{:k v} ...])`
  is appealing but lives with the pattern-matching subsystem, not the
  reader.

---

## Future work

### Quasiquote for runtime data

The escape hatch for everything data literals cannot express --
templating ASTs, building maps whose *keys* are computed, generating
code-like structures. The standard Lisp approach is backtick + comma +
comma-at:

```turmeric
`(:event :user-created :payload #map{:id ~user-id :name ~user-name})
```

This is a larger language change (reader rules for unquote, splicing
semantics, interaction with the macro system) and worth a dedicated
plan once a concrete forcing use case appears. Data literals alone
cover the majority of construction ergonomics without requiring it.

### Set literals (`#set{...}`)

The conventional Lisp/Clojure spelling for sets is `#{...}`, but that
slot is already taken by **effect rows** in Turmeric (`#{Unsafe}`,
`#{IO File}`, etc.). The effect-row syntax was deliberately chosen
to *look* like a set, since an effect row is conceptually a set of
effects -- so the original meaning still earns its spelling.

Rather than migrate effect rows off `#{...}` (a wide fixture-surface
refactor for marginal payoff), reserve **`#set{...}`** as the
spelling for set literals when a `Set` collection type lands in
stdlib. Same dispatch pattern as `#map{...}`; lowers to `(set-of ...)`.

The two collection literals share the `#<tag>{...}` shape on purpose:
`#map{...}` and `#set{...}` look like siblings, and a future
`#<tag>{...}` (e.g. `#bag{...}`, `#ordmap{...}`) drops into the same
slot without further bikeshedding.

### Reader-macro plugin API

If the project grows several literal forms (`#map{`, `#set{`, `#json(`,
`#sql`, `#html`...), it may be worth exposing a registration API
rather than continuing to hand-code branches into `reader.c`. Not
urgent -- the dispatch table is small and a handful more entries is
fine.
