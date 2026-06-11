---
title: Backquoting a list whose head is an inline-C (` ```c ... ``` `) block fails expansion
category: Reported
severity: Surface-syntax pothole (workaround is one extra wrapping form; not a real blocker for any plan)
discovered: 2026-06-11, planning ECS spice E2 (docs/upcoming/ecs-prereq-plan.md, gap D)
researched: 2026-06-11, narrowed from "macros can't emit inline-C" to "list-with-CBLOCK-head"
resolved: 2026-06-11. Fix in `src/compiler/elab_macros.c::ct_eval_quasiquote` --
  when the constructed list has `items[0]->tag == F_CBLOCK`, the list is
  auto-wrapped with `do` so the CBLOCK sits in a body position. Matches the
  Clojure model the research surfaced. The misleading "expression in call
  head has type `nil`, which is not callable" diagnostic no longer fires
  on this shape. Regression covered by
  `tests/fixtures/macro-emits-list-with-cblock-head/`.
---

# Backquoting a list whose head is an inline-C (` ```c ... ``` `) block fails expansion

> **Status: fixed 2026-06-11.** All three macro shapes -- bare CBLOCK,
> backquoted `(do CBLOCK)`, and the previously-failing bare backquoted
> `(CBLOCK)` -- now compile and produce identical function pointers. The
> auto-wrap is local to the quasi-quote walker; no elaborator changes;
> the full fixture suite gained one PASS with no regressions.

> **Triage update 2026-06-11:** Research narrowed this from "macros
> can't emit inline-C at all" (overstated) to a much smaller bug.
> Macros can emit inline-C cleanly via `` `(do ```c ... ```) `` or by
> putting the block at the macro body's top level with no backquote.
> The failure is specific to `` `(```c ... ```) `` -- a backquoted
> list whose *head* is an F_CBLOCK -- which the elaborator interprets
> as "call the C-block-value as a function," producing the misleading
> "expression in call head has type `nil`, which is not callable"
> diagnostic. The root cause is in `ct_eval_quasiquote` (or in the
> downstream call elaborator); a Clojure-style auto-quote of
> CBLOCK-headed lists, or a sharper diagnostic at the same spot, is
> the recommended fix.
>
> **For ECS defsystem specifically**, this is not a blocker. Turmeric
> already auto-converts a top-level `defn`'s name to `ptr<void>` at
> call sites that expect one (see `thread-spawn-fn` users in
> `tests/fixtures/channel-basic/input.tur:174`), so `defsystem` can
> emit `(make-system reads writes ~impl-name)` directly without any
> inline-C trampoline. See § "What the ECS plan actually needs" below.

## Summary

A defmacro body that produces an expansion of the form
`` (```c ... ```) `` -- a one-element list whose only element is an
F_CBLOCK -- fails to elaborate with "expression in call head has
type `nil`, which is not callable." Two adjacent shapes succeed:

- A bare `` ```c ... ``` `` at the macro body's top level (no
  backquote needed; the C block IS the expansion).
- `` `(do ```c ... ```) `` -- the same C block inside a `do`, where
  the C-block sits in a body position rather than the call-head
  position of a list.

The bug is therefore narrow: only the *exact* shape
`backquote-list-of-cblock-only* trips it.

## Severity

Surface-syntax pothole. Any macro author who hits it can wrap with
`(do ...)` and continue. The diagnostic is misleading (talks about
"call head" and "nil" when the user wrote a C block), which is the
real cost: the path from error message to fix is non-obvious. No
known plan is *blocked* by this; several plans pay a one-token-per-
use tax until it's fixed.

## Minimal repro -- the failing shape

```turmeric
(defn my-func [x : int] : int (+ x 100))

(defmacro fnptr-fail []
  `(```c
    return (void *)&my_hyfunc;
   ```))

(defn get-it [] : ptr<void> (fnptr-fail))

(defn main [] : int
  (println (if (= 0 (:: (get-it) :int)) 0 1))
  0)
```

Diagnostic:

```
test.tur:4:5: error: expression in call head has type `nil`, which is not callable
3 | (defmacro fnptr-fail []
4 |   `(```c
  |     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
5 |     return (void *)&my_hyfunc;
6 |    ```))
```

## Working shapes (both verified 2026-06-11)

```turmeric
;; A: bare CBLOCK as the macro body (no backquote at all).
(defmacro fnptr-A []
  ```c
  return (void *)&my_hyfunc;
  ```)

;; B: CBLOCK inside a do-form (the recommended workaround).
(defmacro fnptr-B []
  `(do
     ```c
     return (void *)&my_hyfunc;
     ```))
```

Both produce the same function pointer (verified by
`(= (:: (get-A) :int) (:: (get-B) :int))` returning `true`). Hand-
written non-macro code is also fine (the CBLOCK parser is healthy).
The failure is precisely when the macro expansion produces a list
whose head is a CBLOCK.

## Observed vs. expected

Observed: a list with an F_CBLOCK in the head position is elaborated
as a function call, with the CBLOCK as the callee. The CBLOCK
elaborates to "type `nil`," so the call-head check fires with the
misleading diagnostic.

Expected: either
(a) the elaborator recognises "list whose only element is a CBLOCK"
    as equivalent to the bare CBLOCK -- effectively a one-element
    do-block -- and elaborates the CBLOCK in expression position; or
(b) the quasiquote walker auto-quotes the list so it's treated as
    data, not a call (matches the Clojure model where atoms like
    keywords/strings/numbers self-evaluate); or
(c) the diagnostic at minimum names the actual problem ("F_CBLOCK
    is not a callable form -- did you mean to wrap with `(do ...)`?").

## Research findings 2026-06-11

A subagent surveyed F_CBLOCK handling in the compiler and another
surveyed Lisp-family precedent:

**Compiler side.** `src/compiler/elab_macros.c` already handles
F_CBLOCK correctly in all five touch points it has:

| Line | Function | Behaviour |
|------|----------|-----------|
| 107-109 | `ct_form_equal` | binary-equality compare -- correct |
| 248 | `form_contains_ct_builtins` | returns `false` -- no CT eval needed -- correct |
| 692 | `ct_eval_form` | returns the form as an inert literal -- correct |
| 863-871 | `quasiquote_expand_form` | returns the form as-is -- correct |
| 888 | `substitute_params` | returns the form as-is -- correct |

So the failure isn't at the macro layer at all. The bug fires later
when the expansion lands at the elaborator: the resulting list has
F_CBLOCK as its head, and `elab_call.c` (around line 512, per the
subagent's read) reports "expression in call head has type `nil`."

**Lisp precedent.** Three angles relevant to the fix:

1. **Clojure** (`LispReader.java:1008-1133`,
   `SyntaxQuoteReader.syntaxQuote`): the walker only descends into
   `IPersistentCollection`. Strings, keywords, numbers, characters
   "self-evaluate" -- returned identity at lines 1117-1121. Unknown
   atoms fall through to `(quote form)` at line 1123. This is the
   model Turmeric is closest to.

2. **SBCL** (`src/code/backq.lisp`): a backquoted form is wrapped
   `(quasiquote expr)`. The expander walks conses, special-cases
   the `COMMA` struct, and returns every other atom as-is. Same
   shape as Clojure -- "only conses get walked."

3. **Chicken Scheme** (`foreign-lambda`, `foreign-declare`): inline
   C is *not a new AST node* -- it's a string literal at read time.
   Survives quasiquote trivially because strings are atoms. Carp
   (Lisp-to-C) deliberately doesn't have quasiquote at all and uses
   string-based `Unsafe.emit-c` instead.

The pattern is consistent: **any AST leaf that isn't a cons gets
returned identity by the quasiquote walker.** Turmeric's
quasiquote walker already does this for CBLOCK -- the problem is
that the surrounding F_LIST wraps the CBLOCK as if it were a call.

## Recommended fix locations

Three plausible spots; pick whichever fits the codebase's
invariants:

1. **`ct_eval_quasiquote` in `src/compiler/elab_macros.c` (F_LIST
   case, around line 131-167).** When the constructed list has
   `items[0]->tag == F_CBLOCK`, auto-quote or auto-`do`-wrap before
   returning. Concretely (Explore agent's suggestion):

   ```c
   if (n_out > 0 && items[0]->tag == F_CBLOCK) {
     return ct_value_form(form_list_prepend(
       env->elab->arena, f->span, sym_do, items, n_out));
   }
   ```

   Cheap, local, no elaborator change.

2. **`elab_call.c` around the call-head type check (line ~512).**
   Detect the F_CBLOCK-in-head case and treat the list as a `do`
   of its body. Matches Clojure's "self-evaluating atoms" semantics
   structurally. Larger blast radius but more general.

3. **Diagnostic-only.** Keep the failure but rewrite the message:
   "F_CBLOCK is not a callable form -- did you intend `(do ```c ...
   ```)`?" Cheapest; doesn't unlock anything but removes the trap.

(1) is the smallest move that actually expands the macro language.
(3) is the smallest move that costs the user nothing. (2) is what
you'd write if you wanted Turmeric's macros to be Clojure-shaped.

## Proposed fix directions

Replaced by "Recommended fix locations" above. The earlier text
proposed three speculative directions; the research above pins down
the actual edits.

## Proposed fix directions

1. **Smallest: treat `F_CBLOCK` as a literal inside backquote.** Add
   the missing case to the quasi-quote walker so the C-block form
   passes through to the expansion. Validates on the minimal repro
   above.

2. **Slightly more: support `~`-unquote inside the C body's text.**
   Mirroring how `__TUR_TY_<NAME>__` markers already get substituted
   at emit time, allow the macro to embed `~name` markers in the C
   text that get replaced with the macro-eval'd symbol name. This
   lets `defsystem` emit `(void *)&~impl-name` and have it become
   `(void *)&physics_hyimpl` at expansion. Larger scope but
   substantially more useful.

3. **Compose with str->sym.** With the str->sym gap landed
   ([../archive/history/ecs-macro-symbol-synthesis-missing.md](../archive/history/ecs-macro-symbol-synthesis-missing.md)),
   macros can compute the C-mangled name as a string
   via `str-append` + identity-mangling, then splice it as a
   symbol into the unquoted position. With (1) + str->sym, (2)
   becomes optional.

(1) is the unblocker. (2) is the ergonomic finish.

## What the ECS plan actually needs

The original framing said this gap blocks `defsystem`'s parallel-on-
threadpool path. The research turned up a cleaner route:

**Turmeric already implicitly coerces a top-level defn name to a
`ptr<void>` at call sites that expect one.** Verified end-to-end:

```turmeric
(defn physics-impl [w : int] : int (+ w 1))
(defn render-impl  [w : int] : int (* w 2))

(defn take-ptr [p : ptr<void>] : bool ```c return p != NULL; ```)
(defn ptrs-distinct [a : ptr<void> b : ptr<void>] : bool
  ```c return a != b; ```)

;; All three of the following are `true`:
(take-ptr physics-impl)
(take-ptr render-impl)
(ptrs-distinct physics-impl render-impl)
```

This is exactly the pattern `thread-spawn-fn` in
`tests/fixtures/channel-basic/input.tur:174` uses. So a `defsystem`
macro can emit:

```turmeric
(defmacro defsystem [name params reads writes body]
  (let [impl (str->sym (str-append (symbol-name name) "-impl"))]
    `(do
       (defn ~impl ~params ~body)
       (def  ~name (make-system ~reads ~writes ~impl)))))
```

The `~impl` reference in `(make-system ... ~impl)` is just a symbol;
the call site's `:ptr<void>` annotation does the rest. **No inline-C
trampoline is emitted by the macro at all**, and the
parallel-on-threadpool path in E2 becomes implementable as a follow-
up that does not need this gap closed first.

## What this *does* still cost

The remaining cost is purely ergonomic and confined to a particular
macro shape:

- **Diagnostic confusion** when a user writes
  `` `(```c ... ```) `` and gets "expression in call head has type
  `nil`." The fix path is "wrap with `(do ...)`," not "give up on
  macro-emitted inline-C."
- **No `~name`-substitution-inside-C-text yet.** Macros can emit
  inline-C as a literal block but cannot splice macro-eval'd
  identifiers *into* the C text. That's a separate feature (Chicken
  / Gambit deliberately don't allow it; the C is opaque to the
  reader). If a future macro genuinely needs to bake a substituted
  identifier into inline-C, it has to use the `__TUR_TY_<NAME>__`
  per-instantiation substitution path or define a tiny helper defn
  per substitution.
- **FFI-glue macros generally** still benefit from a one-token
  cleanup once this gap closes; nothing is blocked.

## Validation plan

A fix is validated when:

- The failing minimal repro (`` `(```c ... ```) `` shape) expands,
  compiles, and runs (prints `1`).
- The two working shapes (bare CBLOCK, CBLOCK-in-`do`) still
  compile and emit byte-identical C.
- A round-trip fixture under
  `tests/fixtures/macro-emits-cblock-list/` covers all three shapes
  (bare, in-do, in-bare-list) and asserts they all return the same
  function pointer.
- The error diagnostic, if the failure case is kept as an error,
  names "F_CBLOCK in call head" rather than "type `nil`."

## Interaction with the str->sym gap

`str->sym` shipped 2026-06-11. Combined with the
"top-level defn name auto-coerces to `ptr<void>`" feature documented
above, the original motivating use case (ECS `defsystem` emitting a
function pointer per system) is unblocked *without* this gap being
fixed. This gap is now optional polish: it removes a trap diagnostic
and shrinks one form of expansion by one token. The ECS prerequisite
plan ([`../upcoming/ecs-prereq-plan.md`](../upcoming/ecs-prereq-plan.md))
should reclassify D from "blocks E2 parallel-on-threadpool" to
"ergonomic polish, low priority."
