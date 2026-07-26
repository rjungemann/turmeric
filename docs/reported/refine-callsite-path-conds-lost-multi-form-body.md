---
title: Call-site path conditions are lost when the caller's body has more than one form
category: Bug report
description: caller_body is set to the defn's LAST body form (the return subject), so a crossing in any earlier form is checked without the branch conditions that guard it.
---

# Call-site path conditions lost in a multi-form body

**Severity:** completeness, plus a wrong `--strict-refine` answer. Not a
miscompile -- a call-site crossing never elides the callee's own entry check,
so the program stays correct; what is lost is the proof, and with
`--strict-refine` a program that is in fact fully guarded is rejected.

Found while landing
[refine-predicate-measures-plan.md](../upcoming/v1/refine-predicate-measures-plan.md);
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

## Fix directions

Give the call site the whole body rather than the return subject. The `defn`
form's body forms are `call->as.list.items[body_start ..]`; either synthesise a
`(do ...)` wrapper for `caller_body`, or teach `rt_push_cs_path_conds` to accept
a form *slice* and walk each in order.

Two things to keep while doing it:

- `rt_form_occurrences(caller_body, call_form) != 1` declines ambiguous paths.
  Widening `caller_body` widens what that counts over, which is the correct
  direction (a node shared by two body forms genuinely has no single path) but
  must be re-checked, not assumed.
- `rt_form_mentions_set(caller_body, 0)` declines when the body assigns
  anywhere. Widening makes this fire on more callers, costing precision in
  exchange for keeping the existing soundness argument intact. That trade is
  correct as-is; a later refinement could scope the `set!` check to the forms
  that actually precede the call.

Regression cover to add alongside: the `main` case above, as a
`--strict-refine` happy fixture (it must reach `0 unknown`).
