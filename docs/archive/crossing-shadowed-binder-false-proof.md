# A call-site crossing under a shadowing binder proved a false goal

**Severity:** medium -- a wrong answer from the solver layer, not a miscompile.
The callee's own entry check is never elided by a crossing proof, so the
runtime stayed protected and the program still panicked. What was lost is the
diagnostic's honesty: `--strict-refine` accepted a program that panics.

**Status:** FIXED. Crossings under a shadowing `let` binding or pattern binder
are now abandoned outright.

## Repro

```turmeric
(defn sdiv [n : int d : #refine{ d : int | (not= d 0) }] : int (/ n d))

(defn f [x : #refine{ x : int | (> x 0) }] : int
  (let [x (- x x)]
    (sdiv 10 x)))

(defn main [] : int (println (f 3)) 0)
```

Before: `refine: 2 obligation(s): 2 proven, 0 refuted, 0 unknown`, and
`--strict-refine` compiled clean. The inner `x` is zero, so `(not= d 0)` does
not hold; the program panics at the callee's entry check.

## Root cause

The encoder has ONE FLAT NAMESPACE. The crossing's argument form is `x`, which
encodes to the same variable as the parameter `x`, so it silently inherits the
parameter's hypothesis `x > 0` -- a fact about a completely different value.

This predates path conditions. The crossing environment has always carried the
parameter refinements, and the argument form has always been encoded in the
flat namespace; nothing about the branch conditions was involved. It was found
while extending crossing path conditions to `let` and `match`, because that
work added a shadow probe as a NEGATIVE test and the negative came back
"proved."

## Why dropping the equation was not enough

The first attempt declined only the `x = v` hypothesis and kept the rest of the
path. That still proved the goal: the collision is in the NAME, not in the
fact. The outer `x > 0` was already in the environment and applied to the inner
binding regardless of what the path contributed.

The return-obligation splitter (`rt_prove_paths`) handles the same collision by
alpha-renaming, which it can because it also rewrites the body it is about to
prove. A crossing has no body to rewrite -- only a call form to leave alone --
so abandoning the crossing is the honest answer. It costs a diagnostic in a
rare shape and removes a wrong one.

## Fix

`rt_collect_path_conds` reports a shadowing binder through a `*shadowed`
out-param, and `refine_resolve_call_sites` skips the crossing entirely when it
is set. Covers both `let` bindings and `match` pattern binders.

`tests/fixtures/refine-crossing-shadowed-binder` pins that the callee's entry
check survives and fires. It does NOT distinguish the false proof from the
skip -- both abort, because the check was never the thing at risk -- and the
fixture says so.
