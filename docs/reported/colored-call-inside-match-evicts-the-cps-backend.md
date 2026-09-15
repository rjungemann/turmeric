# Two CPS-backend shapes a colored function cannot take: `match`, and a by-value ADT `handle` result

**Severity: medium.** Both are `tur: this effect operation has no lowering
here` at compile time -- a loud compiler limitation, not a wrong answer -- on
programs the interpreter runs correctly. Neither has a diagnostic that names
the actual shape; both report the `perform` in the *callee*, several functions
away from the form that evicted.

**Status: open.** Found 2026-09-15 while fixing
[effect-row-lost-through-a-constructor-argument](../archive/effect-row-lost-through-a-constructor-argument.md).
They are **not** that report's defect and were not introduced by its fix -- the
`let` control spelling hits both identically, which is the measurement that
separates them. What the fix changed is that they now surface as this
diagnostic rather than as a silent `tur: unhandled effect` abort at run time,
because the coloring walks no longer drop the call edge that reaches them.

## Shape 1 -- `match` anywhere in a colored function

```turmeric
(defeffect Ask [] : int)
(defdata Wrap (MkWrap int))
(defn g [] : int (perform (Ask)))
(defn via-match [] : int (match (MkWrap 1) (MkWrap n) (+ n (g))))
(defn main [] : int (println (handle (via-match) (Ask [] k) (resume k 41))) 0)
```

```
$ TUR_TRACE_EVICT=1 tur check p.tur
[EVICT] BODY-UNSUPPORTED  eff=0 via-match unsupported form: EX_MATCH
```

There is no constructor argument in the repro at all -- the scrutinee is a
literal -- so this is about `match` and the colored call, nothing else. The
CPS translation (`src/passes/cps_ir.c`) has no `EX_MATCH` arm; the node is in
`cps_form_name`'s list of kinds that reach the default arm, fail
`safe_to_delegate` (a colored call is in the subtree), and evict.

Note this is *not* the same as the archived `cps-backend-effect-under-match`,
which was about the control-op SEED walk (`cps_directly_uses_control`) missing
its `EX_MATCH` arm. That one is fixed and the seed walk has the arm. This is
the translation, one pass later.

## Shape 2 -- a `handle` whose result is a by-value defdata ADT

```turmeric
(defeffect Ask [] : int)
(defdata Wrap (MkWrap int))
(defn g [] : int (perform (Ask)))
(defn wrap-val [w : Wrap] : int (match w (MkWrap n) n))
(defn mk-wrap [] : Wrap (let [n (g)] (MkWrap n)))     ;; the LET control spelling
(defn main [] : int
  (println (wrap-val (handle (mk-wrap) (Ask [] k) (resume k 41))))
  0)
```

```
[EVICT] BODY-STRUCT-CORE  eff=1 main
```

A **defstruct record** in the same position is fine, which is the useful
control:

```turmeric
(defstruct Box [v : int])
(defn mk-box [] : Box (Box (g)))
(println (.v (handle (mk-box) (Ask [] k) (resume k 41))))   ;; prints 41
```

So the discriminator is defdata-ADT vs defstruct-record, not "by-value
aggregate" as such -- which is worth knowing before reading `BODY-STRUCT-CORE`
as a general aggregate limit. A single-variant record ADT and a multi-variant
one take different paths here and only the record one survives.

## Why they were not fixed alongside the constructor-argument report

That fix was two things: a shared child enumeration for the two coloring walks
in `src/passes/cps.c`, and extending the existing elaboration hoist
(`elab_hoist_control_operands`) to the constructor call and the field read.
Neither reaches these: the coloring is now *correct* for both shapes -- they
are colored, which is what surfaces the diagnostic -- and the hoist has nothing
to hoist, since the control spelling that a hoist would produce is exactly what
Shape 2's repro already writes by hand.

Both want work inside `src/passes/cps_ir.c`: an `EX_MATCH` arm that ANF-binds
the scrutinee and translates each arm under the shared continuation, and
whatever `BODY-STRUCT-CORE` is gating for a multi-variant ADT result. Neither
is a follow-on to a coloring fix.

## Workarounds

- Shape 1: move the `match` into a pure helper the colored function calls, and
  keep the colored call out of the helper.
- Shape 2: return a defstruct record rather than a defdata ADT across the
  `handle`, or reduce the ADT to a scalar inside the handled region.
