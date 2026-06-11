---
title: Backquote `~(dot-sym X)` inside a do/let body silently drops sibling forms
category: Reported
severity: Macro-author footgun (silent miscompile / dropped statements)
discovered: 2026-06-11, during ECS spice E1' execution (docs/upcoming/ecs-spice-plan.md)
---

# Backquote `~(dot-sym X)` inside a do/let body silently drops sibling forms

## Summary

A defmacro body that uses `\`(do ... (println (~(dot-sym A) ~w)) ...)`
silently discards every sibling form before the `(println (~(dot-sym
A) ~w))` expression at expansion time. The do-block runs only its tail.
The macro still elaborates and compiles -- no diagnostic surfaces --
the program just behaves as if the earlier forms were never written.

The same shape with a hard-coded `\`.Pos w` instead of the computed
`~(dot-sym A) ~w` works correctly: every sibling executes. So the
trigger is the *unquoted dot-sym call*, not the surrounding do/let.

Workaround: build the form with explicit `(list ...)` instead of
backquote. The ECS spice's query macros took this route.

## Severity

Silent miscompile in macros. The user gets no error -- their loop body
just appears not to execute. This is the exact "works by luck because
the surrounding code happened to be the tail expression" failure mode
the bug-reporting rule was written to surface.

## Minimal repro

```turmeric
(defstruct W [Pos : int])

;; Variant A: hard-coded `.Pos` -- works, prints 100, 7, 300.
(defmacro pp-good [w]
  `(do
     (println 100)
     (println (.Pos ~w))
     (println 300)))

;; Variant B: `~(dot-sym A)` -- compiles silently, prints only 300.
(defmacro pp-bad [w A]
  `(do
     (println 100)
     (println (~(dot-sym A) ~w))
     (println 300)))

(defn main [] : int
  (let [w (make-struct W 7)]
    (pp-good w)         ;; -> 100, 7, 300
    (pp-bad  w Pos)     ;; -> 300        (!)
    0))
```

Observed output for `pp-bad`: `300`. Expected: `100`, `7`, `300`.

Even an unused outer let binds the same way:

```turmeric
(defmacro pp-unused-dot [w A]
  (let [d      (dot-sym A)
        access (list d w)]   ;; <- computed but never spliced
    `(do
       (println 100)
       (println 200)
       (println 300))))      ;; backquote body references *nothing* of d/access

(pp-unused-dot w Pos)         ;; prints only `300`
```

So the issue is broader than just "unquote inside the do body": *any*
outer let whose RHS calls `dot-sym` (or `list` over a dot-sym result)
appears to corrupt the surrounding backquote when it's used as a macro
expansion.

## Severity check

This is not "the call site is malformed and dropped"; the call site
runs successfully, just with the wrong body. The dropped expressions
include side-effecting `println` calls, which are exactly the case
where silent dropping is dangerous.

## Workaround verified in the ECS spice

`ecs/query.tur` builds for-each1/2/3 with `(list let (vec ...) (list
while ...))` style construction throughout, never using backquote. This
emits the expected sequence and the integration tests pass:

- `tests/integrate2.tur` -- for-each2 over Pos+Vel, sum = 125750.
- `tests/filter-with-without.tur` -- for-each1 + world-tagged? /
  world-untagged? inside the body, sum = 1368.

Both produced the correct results once the macros were rewritten away
from backquote. The before/after diff isolates the bug to the backquote
+ dot-sym interaction.

## Observed vs. expected

Observed: backquote evaluation interacts with surrounding CT-eval
results in a way that drops or reorders forms when `dot-sym` (and
possibly other CT builtins -- `symbol-name`, `str-append`, `list`)
appears in the same macro body. The dropped forms are silently
discarded; no diagnostic is emitted.

Expected: backquote should be a hygienic, side-effect-free
form-constructor. Forms in the template should appear in the
expansion in the order written, regardless of what CT computations
happen alongside.

## Root-cause pointer

Unknown. Best guess: the CT evaluator's quasiquote walker shares
state (an arena cursor, a CT-value slot, a span position) with the
top-level CT-eval pass that handles `dot-sym`, and a call to dot-sym
between two list-construction steps either (a) advances the cursor
past the earlier siblings or (b) overwrites a slot the backquote was
about to read. Reproducing in a debugger would clarify.

Files to inspect first:
- `src/compiler/elab_macros.c` -- `ct_eval_builtin`,
  `form_contains_ct_builtins`, and whatever drives quasiquote expansion.
- The "compile-time let requires an even number of binding forms" path
  that fired for closely-related macro shapes (a let body with both a
  computed binding via `~(dot-sym ...)` and a `^mut` annotation).

## Proposed fix directions

1. **Quasiquote isolation.** Rebuild form-construction inside backquote
   so it allocates a fresh CT context per template, independent of any
   surrounding `(let [...] ...)` CT bindings. The simplest version
   audits where the quasiquote walker reads/writes state and copies
   what it needs up-front.

2. **Diagnostic floor.** Until the root cause is found, emit a warning
   (or hard error) when a backquote template contains an unquote whose
   value originates from a CT builtin call AND the backquote sits
   inside an outer macro `let` that also calls a CT builtin. That
   pattern is the trigger; flagging it would have caught this at
   expansion time instead of silently dropping forms.

3. **Document and route around.** Add a "use `(list ...)` to construct
   forms when you need `dot-sym`" recipe to the macro-writing guide
   with a link back to this report. (The ECS spice has already had to
   take this route.)

## Validation plan

A fix is validated when:

- The `pp-bad` minimal repro prints `100\n7\n300` (currently `300`).
- A version of `ecs/query.tur` rewritten back to the natural
  backquote shape (`\`(let [__s1 (~(dot-sym A1) ~world) ...] ...)`)
  produces the same emitted C as the `(list ...)`-style today.
- The "compile-time let requires an even number of binding forms"
  error that fired on the original for-each2 attempt no longer fires
  with the same input.

Until then, ECS query macros stay on the `(list ...)` construction
path; the rationale is documented in `ecs/query.tur`'s module
docstring so the next maintainer doesn't try to "clean it up" with
backquote.
