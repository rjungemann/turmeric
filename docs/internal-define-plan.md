# Internal `define` Plan

A Racket-style `(define <name> <init>)` that introduces a binding in the
**current lexical body** instead of nesting a new scope. The form is
recognised as a body item by `defn`, `fn`, `let`, and `do`, and is rewritten
into the equivalent nested-`let` shape before normal elaboration.

## Scope and non-scope

In scope:
- A new special form `define` recognised only in *body position* of the
  forms below. Outside body position it is an error.
- Body-position recognition in: `defn`, `fn`, `let`, `do`.
- Splice-style semantics: `(define x v)` makes `x` visible in every form
  *after* it within the same body (let\*-like). Forward references and
  mutual recursion among internal `define`s are **out of scope** for v1.
- Sweet-exp surface: `define name value` at the start of a body line works
  via the existing reader; no separate reader change required.
- A diagnostic when `define` appears anywhere that is not a body position
  (top of file, `if` branches, vector positions, etc.).

Out of scope:
- A letrec-style group that allows mutual recursion among adjacent
  `define`s. (Possible v2 -- see "Future work".)
- Internal `defn` / `defmacro` / `defstruct`. Those are top-level forms.
  Internal *function* binding is already covered by `(define f (fn ...))`.
- Replacing `def`. `def` remains the top-level constant introducer with
  its current "global scope only" semantics; `define` is the body-form
  sibling.
- Pattern-destructuring in `define` (`(define [a b] v)`). `let` already
  has vector destructuring; add it to `define` only if demand appears.
- Sequence rewriting inside *macro* output. Macros that expand into
  `(do ...)` get the behaviour for free because `do` is one of the body
  sites. Macros that fabricate a `defn`/`fn`/`let` also get it for free.
  No new macro hygiene work.

## Current state

`def` (`src/compiler/elab_fns.c:2638`) is **already restricted to global
scope** -- it explicitly errors with "def is only valid at the top level"
when `e->scope != &e->global`. So the top-level/global story is settled;
the open question this plan answers is only the *nested* one.

Body sequences are processed in four places today:

| Form  | File:line                          | Body handling |
|-------|------------------------------------|---------------|
| `defn` | `src/compiler/elab_fns.c:1393`    | Walks `items[body_start..]`, elaborates each, wraps in `EX_DO`. |
| `fn`   | `src/compiler/elab_fns.c:2155`    | Same pattern. |
| `let`  | `src/compiler/elab_forms.c:9`     | After bindings vec, walks body items; result is the last. |
| `do`   | `src/compiler/elab_forms.c:832`   | Walks all items, result is the last. |

Each of these sites currently elaborates body forms by calling
`elab_form` on each item in left-to-right order with no pre-pass. That is
exactly the seam where we need to splice `define`.

Compile-time evaluation (`ct_eval_form`, `src/compiler/elab_macros.c:402`)
has its own parallel handlers for `do`/`let`/`fn` because macros run in
a Form-level interpreter. **Those need the same treatment** or `define`
will quietly break inside macro bodies. This is the single biggest
implementation risk and is why this plan exists rather than a 30-minute
patch.

No existing form interns the symbol `"define"`. Adding it is additive.

## Semantics: let\* vs letrec

The proposal is **let\*** semantics for v1:

```
(do
  (define x 1)
  (define y (+ x 1))   ;; sees x
  y)
;; rewrites to:
(do
  (let [x 1]
    (let [y (+ x 1)]
      y)))
```

`(define f (fn [n] (f (- n 1))))` -- a self-recursive internal function --
**does not** work in v1 because `f` isn't in scope inside its own init.
This is a real wart for newcomers from Racket/Scheme.

Two reasons to accept it for v1 anyway:
1. It matches Turmeric's existing `let` semantics. `let` is sequential
   (let\*-style), not letrec. Making `define` letrec while `let` is let\*
   would split the rule users have to learn.
2. Mutual / self recursion at file scope already works through `defn`'s
   pass-1 forward declarations. The pressure for internal letrec is real
   in Racket because Racket has no separate pass; here, the workaround
   (lift the recursive helper to top-level `defn`) is one keystroke.

The letrec extension is sketched in "Future work" but explicitly **not
delivered** by this plan.

## Design questions answered

**Q: Can `define` appear anywhere in the body, or only at the top?**
Anywhere. Top-only is simpler to implement but surprising; Racket allows
internal `define` interleaved with expressions (it just lifts them all).
Allowing it anywhere in the body matches the let\* rewrite trivially --
each `define` becomes a `let` wrapping all subsequent forms.

**Q: What about `(define x)` with no init?**
Error. Turmeric has no notion of an uninitialised binding (`let` requires
an init too). Diagnostic: `define requires an initial value`.

**Q: Does it support `^mut` and other annotations?**
Yes, mirroring `let`: `(define ^mut counter 0)` desugars to
`(let [^mut counter 0] ...rest...)`. The full annotation set accepted by
`let` (`^mut`, `^persistent`, `^linear`, `^unique`, `^affine`, `^relevant`)
is the same set accepted before the name in `define`. The desugar is
literally "splat the annotation into the `let` binding slot."

**Q: Type annotations?**
`let` accepts `[name :type init]`. Mirror it: `(define name :type init)`.

**Q: Interaction with `:pre`/`:post`/contracts in `defn`?**
Body splicing happens *after* the contract-extraction loop in
`elab_defn` (it already advances `body_start` past `:pre`/`:post`). The
splice operates on the same `body_start..len-1` window. No interaction.

**Q: What about `tur eval` / REPL single expressions?**
The REPL evaluates each top-level form in turn through the same
elaboration pipeline. A bare `(define x 1)` typed at the REPL is **not**
in a body sequence and would error. Either:
- (recommended) the REPL wraps its top-level history in an implicit `do`
  -- one small change in `src/turi/repl.c` -- so `define` works.
- or we punt and tell users to use `def` at the REPL. Tolerable but
  ugly given that "REPL feels Schemey" is half the point of the feature.

This plan recommends the implicit-`do` change in the REPL and treats it
as part of the deliverable.

## Implementation strategy

### 1. New symbol

Add `sym_define` to `Elab` (`elab_internal.h:174` block,
`elab_core.c:980` init block). Convention: `e->sym_define = intern_cstr(st, "define");`.

### 2. The splice helper

One file-static helper, drafted in `elab_forms.c` (or a new
`elab_define.c` if we want to keep it isolated):

```c
/* Pre-pass over a body window: convert leading + interleaved
 * (define name [:type] init) items into nested-let wrapping.
 * Returns a single Form that the caller can elaborate as one expression
 * (typically by re-entering elab_form on it).
 *
 * If no defines are present, returns a synthesized (do ...) over the
 * window unchanged, so the caller has one code path.
 */
static Form *splice_internal_defines(Elab *e, Form **items, uint32_t n, Span span);
```

Algorithm (single pass, recursive descent over the body window):

1. Scan left-to-right.
2. On `(define name init)` (after annotation parsing):
   - Build a `let` binding vector `[<annots> name <:type-if-any> init]`.
   - Recursively splice the **remaining** items as the let body.
   - Wrap in `(let [...] <rest>)`.
3. On a non-`define` item: emit it into the current `do` accumulator.
4. If the accumulator has one item and no following defines, return it
   directly; otherwise wrap as `(do ...)`.

The helper builds new `Form` trees via the existing arena (`e->arena`)
and the constructors in `forms.c` (`form_sym`, `form_list`,
`form_vec`). No new IR. The output is reusable: `elab_form` is called
on it as if the user had written the desugared shape.

### 3. Wire it into the four body sites

For each site, replace the existing "loop over body items calling
`elab_form`" block with:

```c
Form *spliced = splice_internal_defines(e, &call->as.list.items[body_start],
                                        n_body, call->span);
Expr *body = elab_form(e, spliced);
```

Sites and the exact replacement points:

| Site | File | Existing line(s) to replace |
|------|------|------------------------------|
| `defn` body | `elab_fns.c:1399-1437` | The `if (n_body == 1) ... else ...` block that hand-rolls `EX_DO`. |
| `fn` body  | `elab_fns.c:~2155-2180` | Same pattern (verify exact lines during impl). |
| `let` body | `elab_forms.c` (locate in `elab_let`) | After bindings vec is parsed, before the body-elab loop. |
| `do` body  | `elab_forms.c:832-864` | The `for i in 0..n` body-elab loop. |

`do` is a recursive case: `splice_internal_defines` itself produces
`(do ...)` forms, so we must avoid infinite recursion. Solve by giving
the helper an `already_spliced` flag, or have `elab_do` call a
"flat-elab" path on its items after the helper rewrites them.

Cleanest factoring: the helper returns a `Form*` that is *guaranteed*
not to be a `(do ...)` containing further `define` forms. `elab_do`
then trusts its items are pre-spliced and elaborates each directly.

### 4. Compile-time mirror

`ct_eval_form` in `elab_macros.c` handles `do`, `let`, `fn` for the
Form-level interpreter used by `defmacro`. The same splice must run
there, or `define` will silently misbehave inside macro bodies. The
implementation is parallel: a `ct_splice_internal_defines` helper that
produces the same Form tree, invoked from the three handlers
(`sym_do` at line 402, `sym_let` at 410, `sym_fn` at 446).

### 5. Diagnostics

Bad usages and their messages:

- `(define x)` with no init: `define requires (define name [:type] init)`.
- `define` in a non-body position (e.g. `(if (define x 1) ...)`): the
  elaborator hits a bare `(define ...)` call through normal `elab_call`
  and produces `define is only valid as a body form (in defn, fn, let, or do)`.
  Implementation: register `sym_define` in `elab_call` to point at a
  stub `elab_define_error` that always emits this.
- `(define name)` at top level: same diagnostic. We could be friendlier
  and suggest `def`, but the same error text is fine.

### 6. REPL change

In `src/turi/repl.c`, wrap the accumulated top-level history (or just
each turn?) in an implicit `do` so `define` at the prompt resolves.
This is a one-call-site change and is independently useful.

## Testing

Fixture-driven, matching the rest of the codebase. New directories
under `tests/fixtures/`:

- `define-basic/` -- `(do (define x 1) (define y 2) (+ x y))` returns 3.
- `define-in-defn/` -- internal `define` inside a `defn` body.
- `define-in-fn/` -- inside a lambda.
- `define-in-let/` -- mixed with explicit `let` bindings.
- `define-annot/` -- `^mut` and `:type` annotations round-trip.
- `define-sees-prior/` -- let\* semantics: `(define y (+ x 1))` sees `x`.
- `define-no-self-rec/` -- self-reference inside init is unbound (error
  fixture; documents the v1 limitation).
- `define-bad-position/` -- `(if (define x 1) ...)` produces the
  diagnostic.
- `define-in-macro-body/` -- a `defmacro` whose expansion is a
  body-positioned `define` works because the *expanded* code is what
  hits the splice, not the macro's own body. (Confirms we don't need
  to do anything special for macro hygiene.)
- `define-in-ct-do/` -- compile-time `(do (define x 1) x)` inside a
  macro body works (exercises the `ct_eval_form` mirror).

Each fixture has the standard `input.tur` / `expected.out` /
`expected.c` triple. **Regenerate `expected.c` for every fixture
that exercises a defn/fn/let/do body shape**, per the CLAUDE.md
strict rule -- the change is broad enough to touch incidental
codegen even where no `define` is used, if the helper's "no defines
present" path produces a subtly different Form shape than the old
hand-rolled `EX_DO`. **Verify this path is a true no-op** before
declaring the fixture refresh complete.

## Risks

1. **Codegen drift in the no-define path.** If the splice helper's
   "passthrough" doesn't produce byte-identical `EX_DO` to today's
   handlers, every fixture refreshes. Mitigation: keep the four sites'
   existing `EX_DO` construction in place and *only* take the helper
   path when `define` is actually present in the body window. One
   linear pre-scan, then branch. This makes the no-define path provably
   identical.

2. **Compile-time path drift.** The `ct_eval_form` mirror is easy to
   forget. Add a fixture that *requires* it to work, so the test
   suite catches the omission.

3. **Annotation surface creep.** If `let`'s annotation set grows
   later, `define`'s parser must follow. Mitigation: share the
   annotation parser. Pull the `^mut`/`^persistent`/... loop in
   `elab_let` (`elab_forms.c:43-77`) into a reusable helper and call
   it from both `let` and the `define` splice.

4. **User confusion: `define` vs `def`.** They differ only by scope
   rules. Document both on the same page; add a one-line note in each
   form's error message pointing at the other.

5. **Future letrec extension.** If v2 adds letrec semantics, the
   rewrite changes from sequential `let` to a `letrec`-equivalent.
   That's a behaviour change for code written against v1. Mitigation:
   document v1 as let\* now, and if letrec lands, gate it behind an
   opt-in (`#lang racket-define` or similar) rather than silently
   upgrading.

## Future work

- **Letrec semantics.** Group adjacent `define`s into one `letrec`-like
  binding so mutual recursion works without lifting helpers. Requires
  detecting "adjacent define run" and a real letrec lowering (Turmeric
  doesn't have one today -- `let` is sequential). Non-trivial.
- **Internal `define-fn` / `define-macro`.** Convenience shorthands.
  Pure desugar: `(define-fn f [x] body)` -> `(define f (fn [x] body))`.
- **Pattern destructuring.** `(define [a b] pair)`. Match `let`'s
  vector-destructuring path.

## Effort estimate

Small-to-medium. The splice helper is ~80 lines, including
annotation/type parsing. Four wiring points, three compile-time mirror
points, REPL `do` wrap, and ~10 fixtures. Realistic budget: a focused
day for the implementation, half a day for the fixture sweep and
codegen-no-op verification, half a day for review polish. The risk
section is the part that can blow up.

## Recommendation

Worth doing **if** the team values Scheme/Racket familiarity at the
REPL and inside `defn` bodies. It is a small ergonomics win that
mostly saves indentation; users who prefer the current style lose
nothing because nothing changes for code that doesn't use `define`.

The honest counterargument: `let` already does this with two more
characters and explicit scope, which some readers will find clearer.
Don't expect a productivity bump -- expect a "feels right" bump for
users coming from Lisps.
