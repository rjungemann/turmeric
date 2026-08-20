# #map{} has no typed-empty suffix although []:T and #set{}:T exist

**Severity: low**. Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `#map{}:(K V)` is accepted and lowers to
`(:: #map{} (Map K V))`.

## Repro

`[]:T` and `#set{}:T` pin empty-literal element types, but an empty `#map{}`
required ascribing the full `(Map K V)`. `#map{}:(int cstr)` was not a
suffix at all -- the `:(int cstr)` read as a separate form, so inside a `let`
it surfaced as *"let binding name must be a symbol, got non-symbol"*.

## Root cause

`maybe_container_type_suffix` in src/compiler/reader.c built a fixed
one-argument type application `(Ctor elem)`, and was wired only into the vec
and set closers. `read_map_literal`'s call site did not go through it.

## Resolution

`maybe_container_type_suffix` gained an `n_type_args` parameter:

- `n_type_args == 1` -- unchanged. `[]:int`, `#set{}:cstr`, and the
  compound-element form `[]:(Vec int)` behave exactly as before.
- `n_type_args == 2` -- the suffix is a parenthesised **pair**:
  `#map{}:(cstr int)` lowers to `(:: #map{} (Map cstr int))`.

The pair spelling is forced by arity: it has to come from somewhere, and a
bare `#map{}:cstr int` would swallow whatever token followed the literal.
There is no ambiguity with the compound-element reading -- `[]:(Vec int)` is
one element type that happens to be parenthesised -- because which reading
applies is fixed by the literal kind, not by the shape of the suffix.

A mis-shaped suffix (`#map{}:int`, `#map{}:(int)`, `#map{}:(a b c)`) is
rejected in the reader, naming the expected `#map{}:(<key> <value>)` shape.
Letting it through would surface later as a mis-arity `(Map int)` type
application, pointing at a form the user never wrote.

## Scope note -- what this does and does not buy

The suffix is sugar for the ascription and is **neither stronger nor weaker**
than it. Measured against `main`, `(map-assoc (:: (map-new) (Map cstr int))
42 1)` -- a key of the wrong type -- is accepted, and so is the equivalent
through the new suffix; the same looseness is already present in the shipping
`#set{}:T`. So this closes a *consistency and verbosity* gap in the
literal-suffix family, not a soundness one. Tightening `Map`/`Set` parameter
checking is a separate concern and was deliberately not folded in here.

## Tests

- `tests/fixtures/data-literal-typed-empty` -- extended with four map rows:
  an empty `#map{}:(cstr int)`, `.eq?` dispatch recovering
  `Eq[Map[cstr, int]]` across maps built up from the typed empty literal
  (equal and unequal), and two empty typed maps comparing equal with no value
  to infer from. The pre-existing vec/set rows are untouched and still assert
  the one-argument path, including `[]:(Vec int)`.
- `tests/fixtures/errors/map-suffix-not-a-pair`, `.../map-suffix-one-arg`,
  `.../map-suffix-three-args` -- the three mis-arity spellings.

Suites: run.sh 2663 passed / 0 failed; run-turi.sh 1834 passed / 0 failed.
No `expected.c` snapshot moved -- the change only adds syntax.

## Guides updated

- docs/guides/data-literals-guide.md -- section retitled to include
  `#map{}:(K V)`, the example block and desugaring sentence gained the map
  form, a paragraph explains why the map suffix is a pair and why that does
  not clash with `[]:(Vec int)`, and the final Rules bullet flipped from
  "not yet supported" to the arity rule and its diagnostics.
