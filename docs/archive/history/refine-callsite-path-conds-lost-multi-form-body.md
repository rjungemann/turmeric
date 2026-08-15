---
title: Call-site path conditions are lost when the caller's body has more than one form
category: Bug report (resolved)
description: caller_body was set to the defn's LAST body form (the return subject), so a crossing in any earlier form was checked without the branch conditions that guard it. Fixed by passing the whole body.
---

# Call-site path conditions lost in a multi-form body -- RESOLVED

**Severity:** completeness, plus a wrong `--strict-refine` answer. Not a
miscompile -- a call-site crossing never elides the callee's own entry check,
so the program stays correct; what is lost is the proof, and with
`--strict-refine` a program that is in fact fully guarded is rejected.

**Status:** fixed. `rt_whole_body` (`src/compiler/elab_fns.c`) now hands
`refine_fill_call_site_env` the caller's whole body rather than its last form.
Pinned by `tests/fixtures/refine-crossing-path-conditions-multi-form`.

Found while landing
[refine-predicate-measures-plan.md](../refine-predicate-measures-plan.md);
unrelated to that plan's sort work.

## Repro

```turmeric
(defn sdiv [n : int d : #refine{ d : int | (not= d 0) }] : int
  (/ n d))

;; ONE body form: the path condition is collected, the crossing discharges.
(defn caller [x : int] : int
  (if (not= x 0) (sdiv 10 x) 0))

;; TWO body forms: identical guard, no path condition, crossing unknown.
(defn main [] : int
  (let [x 3]
    (if (not= x 0)
      (println (sdiv 10 x))
      (println 0)))
  0)
```

```
$ TUR_REFINE_STATS=1 TUR_REFINE_DUMP=1 tur check repro.tur --enable=refined
--- refinement VC (argument 2 of 'sdiv' in 'caller') ---
(assert (not (= x 0)))               <-- path condition present
(assert (not (not (= x 0))))
--- refinement VC (argument 2 of 'sdiv' in 'main') ---
(assert (not (not (= x 0))))         <-- no hypothesis at all
refine: 2 obligation(s): 1 proven, 0 refuted, 1 unknown
```

Note the `main` case also loses the `let` equation `x = 3`, which the same walk
would have supplied.

## Root cause

`src/compiler/elab_fns.c:4774` (the `defn` elaboration path):

```c
const Form *rt_subject = (call->as.list.len > body_start)
                       ? call->as.list.items[call->as.list.len - 1] : NULL;
...
refine_fill_call_site_env(e, rt_cs_start, rt_env, <name>, rt_subject);
```

`rt_subject` is the **last** form of the `defn` -- correct for its job as the
return obligation's subject, and wrong for its second job as
`RefineCallSite.caller_body`. `rt_push_cs_path_conds` walks `caller_body` down
to `call_form` (`elab_fns.c:1628`); when the call lives in any earlier body form
the walk never reaches its target, `rt_collect_path_conds` returns false, and
the function pushes nothing. In the repro `main`'s last form is the literal `0`,
so the walk searches `0` for the call.

This is why every case in `tests/fixtures/refine-crossing-path-conditions` uses
a single-form body: the fixture set never exercised the multi-form shape.

## Fix

`rt_whole_body(e, call, body_start)` builds the form the walk descends. A
single-form body is passed through **unwrapped**, so the common case allocates
nothing and produces a tree byte-identical to before; a multi-form body is
wrapped in a synthetic `(do ...)`.

`do` is deliberately not one of the three heads `rt_collect_path_conds` treats
specially (`if`, `let`, `match`), so it falls to the generic descent -- which
is exactly the semantics wanted: a later body form is *not* guarded by an
earlier one, and the walk collects nothing from siblings it passes over.

Both vetoes in `rt_push_cs_path_conds` now range over the whole body, and both
move in the safe direction:

- `rt_form_mentions_set(caller_body, 0)` was checked against the last form
  only, so an assignment in an *earlier* form could invalidate a condition
  unnoticed. It now declines every crossing in a body that assigns anywhere.
  Strictly more conservative, and it closes a small hole rather than opening
  one. (A later refinement could scope the check to the forms that actually
  precede the call.)
- `rt_form_occurrences(caller_body, call_form) != 1` now counts across the
  whole body, so a `call_form` node reachable from two body forms -- which a
  macro sharing a node can produce -- is correctly ambiguous instead of
  spuriously unique.

## Regression cover

`tests/fixtures/refine-crossing-path-conditions-multi-form`, under
`--strict-refine`. Every guarded call in it sits in a **non-last** body form,
which is the part that matters: put the `if` last instead and the case passes
with or without the fix, which is how the gap survived. Verified by rebuilding
with the fix disabled -- three `TUR-E0371`s (`guard-then-literal`,
`let-then-more`, `main`); with it, `5 obligation(s): 5 proven, 0 unknown`.

The fixture also keeps a single-form `one-form` case as a control, so a future
change that breaks the unwrapped path shows up here too.
