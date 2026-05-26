# Runtime List Quasiquote Plan

## Scope and non-scope

In scope:
- A quasiquote-style construct that builds a **runtime `Cons` list**
  with interpolated values, e.g. `` `(1 ~x 3) `` evaluating to
  `(cons 1 (cons x (cons 3 (nil-value))))`.
- Unquote-splicing for prepending another list: `` `(1 ~@xs 9) ``.
- An honest analysis of whether to overload existing backtick syntax
  or introduce a separate sigil.

Out of scope:
- General quasiquote-for-arbitrary-data (maps, tuples, vectors). Lists
  only.
- Replacing the existing compile-time quasiquote, which is essential
  for macro authoring and stays exactly as it is.
- Heterogeneous element types (see tuple plan; `Cons` is monomorphic).

## Current state

`src/compiler/reader.c` already reads backtick / tilde / tilde-at as
reader macros:

| Sigil | Reader form | Today's meaning |
|-------|-------------|-----------------|
| `` `x `` | `F_QUASIQUOTE` | compile-time template wrapper (`reader.c:567`) |
| `~x` | `F_UNQUOTE` | interpolate compile-time value (`reader.c:526`) |
| `~@x` | `F_UNQUOTE_SPLICING` | splice compile-time list of forms |

These are processed by `elab_macros.c:118` (`ct_eval_quasiquote`) to
build `Form` trees for macro expansion. They are **never** lowered to
runtime cons-construction. There is no existing path from a quasiquote
in expression position to a `(Cons A)` value at runtime.

Adjacent in the codebase:
- The `list-macro-plan.md` work, if landed, gives `(list a b c)` as a
  runtime constructor. This is a prerequisite -- this plan compiles
  down to it.
- `tuple-type-plan.md` covers the heterogeneous case. Quasiquote-list
  is monomorphic by construction (everything must unify to one `A`).

## Use cases

1. Building lists with one or two interpolated values:
   `` `(:tag ~name) `` instead of `(list :tag name)` -- and honestly,
   the second is shorter. The quasiquote shines only when there are
   many literal positions mixed with a few computed ones.
2. Splicing an existing tail onto a new head:
   `` `(:start ~@middle :end) ``. This is the case where quasiquote
   meaningfully beats `(cons :start (concat middle (list :end)))`.
3. Migration from other Lisps where users have quasiquote in muscle
   memory.

Brutal honesty: case 1 is barely worth a feature. Case 2 is real but
covered by a `list*`/`concat` combo. Case 3 is taste. **This feature
is the weakest of the three plans -- read the recommendation before
committing.**

## Design options

### Option A -- overload existing `` ` ``: context-sensitive lowering

When the elaborator sees `F_QUASIQUOTE` in **expression position
inside a runtime body** (not a macro template), lower it to a
`(list ...)` call with unquoted slots passed through and
unquote-splicing slots routed through `concat`.

- Pros: no new reader sigil; matches Clojure/Scheme intuition.
- Cons: context-sensitive semantics for a reader form. The
  elaborator must distinguish "I'm inside a `defmacro` body" from
  "I'm inside a `defn` body," and the user must hold the same
  distinction in their head. The same character sequence means two
  different things depending on enclosing form. This is the kind of
  footgun the project has avoided so far.
- Worse: a macro that happens to construct a list and return it for
  later runtime use now has ambiguous quasiquote inside it.

### Option B -- new sigil for runtime quasiquote (e.g. `` #`(...) ``)

Add a distinct reader macro that means "runtime list quasiquote." `#`
is already the dispatch prefix; `` #` `` is currently unused per
`src/compiler/reader.c:1909-2001`.

- Pros: zero ambiguity with macro-author quasiquote. The intent is
  visible at the call site. Tooling and humans both win.
- Cons: another sigil to learn. Two backtick-shaped things in the
  language.

### Option C -- a runtime macro, no new syntax (`list-qq`)

Provide `(list-qq a b ~c ~@xs d)` as a macro that lowers to the same
construction. Reuse the existing `F_UNQUOTE` and `F_UNQUOTE_SPLICING`
reader forms when they appear inside `list-qq`'s args; outside of it,
unquote-outside-quasiquote is already an error so nothing changes.

- Pros: no reader change at all; one new macro in `stdlib/list.tur`.
- Cons: `(list-qq ...)` is ugly enough that nobody will use it; if
  it's that long, just write `(list a b c)` and `concat`.

### Option D -- don't ship this

Use `(list a b c)`, `(list* a b c rest)`, and `(concat xs ys)`. These
cover every case quasiquote-list would cover, with no new syntax and
no ambiguity. The only thing missing is the lexical sugar, and
list-of-mostly-literals is not the common shape in real Turmeric code
based on a spot-check of stdlib.

### Recommendation

**Option D for now. Option B if and when real usage demands it.**

Reasoning:
- The compile-time quasiquote machinery (`elab_macros.c`) is doing a
  fundamentally different job (Form-tree construction for macro
  expansion). Overloading the same syntax (Option A) couples two
  unrelated subsystems and pays the readability cost forever.
- Option C is a worse `(list ...)`.
- Option B is the right answer **if** there's pull -- but the pull
  hasn't materialised yet. Land `list` + `concat` first, see whether
  users reach for "I wish I could splice mid-literal" often enough
  that a sigil pays for itself.
- Option D keeps the language smaller. The cost of waiting is one
  extra `concat` call at use sites that already exist in tiny numbers.

Reject Option A unconditionally. The context-sensitive reading of `` ` ``
is not worth it.

## Surface design (Option B, if pursued)

```turmeric
#`(1 2 3)              ; => (cons 1 (cons 2 (cons 3 (nil-value))))
#`(1 ~x 3)             ; => (cons 1 (cons x (cons 3 (nil-value))))
#`(:hd ~@middle :tl)   ; => (cons :hd (concat middle (cons :tl (nil-value))))
```

Element typing: same constraint as `list` -- all elements (including
the unspliced contents of any spliced list) must unify to one `A`.

Empty form: `` #`() `` => `(nil-value)`.

Unquote outside `` #` ``: already a reader error today; behaviour
unchanged.

Unquote inside a `` #` `` that is itself inside a `defmacro` template:
this is the edge case. Recommendation: the inner `` #` `` is parsed
fresh, so its `~` binds to it, not to the outer macro template. Same
rule as nested quasiquote in Scheme. Document and write a fixture.

## Implementation steps (Option B)

Only relevant if/when the call to land this happens. Sequenced after
`list-macro-plan.md` is done.

1. **Add `concat`** to `stdlib/list.tur` if not already present. Hard
   prerequisite for splice lowering.
2. **Reader change.** Add `` #` `` dispatch in `src/compiler/reader.c`
   that emits a new form tag, e.g. `F_RT_QUASIQUOTE`, distinct from
   `F_QUASIQUOTE`. Inside it, `~` and `~@` keep their existing reader
   forms -- the difference is purely the outer tag.
3. **Elaborator lowering.** New pass (or new arm in an existing pass)
   that walks `F_RT_QUASIQUOTE` and rewrites it to:
   - Literal slot: `x` as-is.
   - `F_UNQUOTE x`: `x` as-is (evaluates at runtime).
   - `F_UNQUOTE_SPLICING xs`: chunked into a `concat` call.
   Final shape is a tree of `cons` and `concat` calls.
4. **Type checking.** Falls out naturally if lowering produces ordinary
   `cons`/`concat` calls -- the existing typer handles the unify
   constraint. No new type rules.
5. **Fixtures.** `tests/fixtures/typed/list-quasiquote-basic/`,
   `.../-splice/`, `.../-type-mismatch/`, `.../-nested-in-macro/`.
6. **Docstring + docs.** Add a short section to whatever guide covers
   list construction. Cross-reference `list`/`list*`/`concat`.

## Risks and open questions

- **Sigil bikeshed.** `` #` `` is the obvious one; alternatives like
  `` @` `` or `` `[ `` exist. Decide before reader work begins, not
  during.
- **Splice typing.** When `~@xs` splices, `xs` must already be a
  `(Cons A)` where `A` unifies with the other elements. The error
  message needs to be intelligible -- "spliced list element type
  doesn't match surrounding list" rather than a raw unification
  failure inside generated cons calls.
- **Nested quasiquote (macro template containing `` #` ``).** Per the
  surface design above, the inner one is independent of the outer.
  Verify the macro expander treats `F_RT_QUASIQUOTE` as a plain form
  to be substituted into, not as something to be evaluated at macro
  expansion time.
- **Performance.** `concat` over splice positions is O(n) in the
  spliced list length. For tight loops, hand-written cons is faster.
  Document; don't micro-optimise.
- **Pull-through from Lisp habits.** Users coming from Scheme/Clojure
  will reach for backtick by reflex and get the compile-time
  quasiquote (which probably errors in expression position). Make
  sure the diagnostic says "for runtime list construction, use `list`
  or `` #` ``." A good error here is half the user education.

## Success criteria (if Option B ships)

- `` #`(1 ~x 3) `` evaluates correctly at runtime and produces the
  expected `(Cons int)`.
- Splice form `` #`(:a ~@xs :b) `` works for any list whose element
  type unifies with `:a`/`:b`.
- Macros containing `` #` `` in their *expansion body* (not template)
  still work; nested macro-template + runtime-quasiquote test passes.
- Diagnostic on compile-time `` ` `` in runtime expression position
  points users at `` #` `` or `list`.

## When to do this

**Not now.** Land `list-macro-plan.md` and `concat` first. Wait until
real code shows at least three independent splice-mid-literal use
sites that would meaningfully read better with `` #` `` than with
`(cons ... (concat xs ...))`. If that bar is not met within a quarter
of `list` shipping, close this plan as won't-do -- the
compile-time quasiquote already exists for macro authors, and that's
the audience that actually benefits from quasiquote syntax.
