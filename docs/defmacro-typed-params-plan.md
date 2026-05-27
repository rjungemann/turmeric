# Defmacro Type-Annotated Params Plan

## Scope and non-scope

In scope:
- A decision on what `(defmacro name [param :Type] body)` should
  do. Today it is a hard parse error
  (`src/compiler/elab_macros.c:1011`), and the diagnostic
  ("defmacro: parameter must be a symbol, got keyword") doesn't tell
  the user how to fix it.
- Whichever direction we pick, fix `stdlib/future.tur:132` --
  currently the only site in the whole codebase that hits this
  (`(defmacro promise-pair [cell :ptr<void>] ...)`) -- so the file
  loads cleanly again.

Out of scope:
- General macro hygiene improvements (gensyms, capture rules).
  Orthogonal.
- A typed-macro system in the Scala-3 / Racket sense, where the
  macro's expansion is checked against a contract. Way bigger than
  the papercut this plan is responding to.
- Reader-level keyword handling. The lexer is fine; this is purely
  about how `elab_defmacro` interprets the parameter list.

## Current state

`elab_defmacro` (`src/compiler/elab_macros.c:1003`) walks the param
list looking for either:

- a bare `F_SYM` -- the parameter name, or
- the `&` symbol marker, after which exactly one more `F_SYM` is the
  rest parameter.

Anything else is rejected at parse time:

```
defmacro: parameter must be a symbol, got keyword
```

This contrasts with `defn`, which accepts (and requires, for
type-checked params) `[name :Type]` pairs. Users coming from `defn`
muscle memory can reach for the same syntax in a `defmacro` body and
get a terse error.

Survey: across `stdlib/` and `tests/` there is exactly **one** site
using type-annotated macro params -- `stdlib/future.tur:132`:

```turmeric
(defmacro promise-pair [cell :ptr<void>]
  (list (promise-of-cell cell) (future-of-cell cell)))
```

This is almost certainly a copy-paste from a nearby `defn`. The
annotation is meaningless at the macro layer (the parameter binds a
`Form`, not a `:ptr<void>` runtime value) and was never enforced; the
file simply fails to load today.

No spice, no test, no example depends on this file loading -- it's
dead library code -- but the error message is bad and the inconsistency
between `defn` and `defmacro` is real.

## Design options

### Option A -- fix the one site; improve the diagnostic

Keep the strict parser. Update the error to point users at the right
fix:

```
defmacro: parameter '%s' has a type annotation, but macro parameters
bind Forms (not runtime values) and cannot be typed.  Use
'(defmacro name [%s] body)' (remove the annotation), or '(defn name
[%s :T] body)' if you meant a runtime function.
```

Then remove the annotation from `stdlib/future.tur:132`.

Pros:
- Tiny diff. No grammar change. Honest about the semantics
  (macro params bind syntax, not values).
- Forces the writer to think about whether they wanted a macro at
  all -- the annotation is a signal they may have wanted `defn`.

Cons:
- The friction returns the next time someone copy-pastes from `defn`.
  (Survey says: zero times in the existing codebase, so this risk is
  low.)

### Option B -- silently accept and discard the annotation

Treat `[name :Type]` in a `defmacro` param list as equivalent to
`[name]`. The keyword is parsed and dropped.

Pros:
- One-line change in `elab_defmacro`. No new error path.
- Aligns the surface of `defmacro` and `defn` so muscle memory works.

Cons:
- The annotation becomes a syntactic lie -- it appears to say
  something, but the compiler ignores it. A reader sees
  `(defmacro foo [x :int] ...)` and reasonably expects `x` to be an
  `int`-valued runtime parameter, which it isn't.
- Encourages exactly the confusion (`defmacro` vs `defn`) that the
  current strict parser surfaces.

### Option C -- give annotations a meaning (Form-tag hint)

Allow `[name :Form]`, `[name :Symbol]`, `[name :Int]`, etc., where the
keyword names a Form tag (`F_SYM`, `F_NUM`, `F_LIST`, ...) the
expansion site must satisfy. The check fires at macro-call time with
a real error if the argument's tag doesn't match.

Pros:
- The annotation becomes meaningful. Authors of complex macros get
  better error messages at call sites.
- Brings macros closer to the rest of the language without faking
  runtime types.

Cons:
- Substantial scope: need a keyword-to-Form-tag table, a new check
  pass at macro expansion, a way to express "any Form", and error
  messages that don't make readers more confused. (`:Form` vs the
  unrelated runtime `Form` struct, etc.)
- Cost is wildly out of proportion to the one site that triggers
  the current papercut. Worth a plan of its own if real demand
  shows up.

## Recommendation

**Option A.**

The papercut is a one-line stdlib fix and a slightly nicer error
message. The codebase has exactly one offending site and zero tests
that exercise the case. Options B and C both pay a real complexity
cost (a silent annotation lie, or a whole new Form-tag-check
machinery) for a problem that nobody outside `stdlib/future.tur` has
hit.

If, after Option A lands, the new error fires repeatedly in real
user code (spices, examples, anywhere with `defmacro`), reopen this
plan with a tally of where the friction lives and reconsider
Option B.

## Implementation steps (Option A)

1. **Improve the diagnostic in `elab_defmacro`** at
   `src/compiler/elab_macros.c:1014`. When the offending form is an
   `F_KEYWORD`, mention the `defn`-vs-`defmacro` distinction and
   show the corrected param list verbatim. For other tags
   (`F_NUM`, `F_STR`, etc.) keep the generic message.
2. **Fix `stdlib/future.tur:132`**: change
   `(defmacro promise-pair [cell :ptr<void>] ...)`
   to
   `(defmacro promise-pair [cell] ...)`.
   Update the surrounding docstring if it claims a type contract that
   never existed.
3. **Add a negative fixture**
   `tests/fixtures/errors/defmacro-typed-param/` that asserts the
   improved diagnostic substring.
4. **Sanity-load** `stdlib/future.tur` once after the fix
   (`./build/tur check stdlib/future.tur`) and confirm it parses --
   the file has been broken-on-load since before this plan started,
   and Option A removes that block. Any further `tur check` errors
   are pre-existing and out of scope for this plan.

## Risks and open questions

- **future.tur has more rot.** The file is dead-shipped and may have
  other syntactic stale spots that no test catches. The plan's
  success criterion is just that step 4 stops failing on
  `promise-pair`. Anything else found there belongs in a separate
  cleanup.
- **Error-string churn.** Test fixtures match diagnostic substrings.
  Step 1 should keep enough of the existing wording
  ("defmacro: parameter") that any future fixture asserting on this
  message keeps working; the negative-fixture in step 3 should pin
  the new wording.
- **Repeated copy-paste.** If users routinely copy `[x :T]` from
  `defn` to `defmacro`, Option A alone is friction. The recommendation
  is to ship Option A and watch; revisit with data if it bites.

## Success criteria

- `(defmacro foo [x :int] ...)` emits an error that names the
  problem and points the reader at the fix (`defn` vs no
  annotation).
- `./build/tur check stdlib/future.tur` no longer fails on
  `promise-pair`.
- `tests/fixtures/errors/defmacro-typed-param/` passes.
- Every other `defmacro` in `stdlib/` continues to compile
  unchanged.
