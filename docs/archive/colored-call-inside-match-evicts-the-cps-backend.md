# Two CPS-backend shapes a colored function cannot take: `match`, and a by-value ADT `handle` result

**RESOLVED 2026-09-16** -- both shapes lower now; see Resolution at the end.

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

## Resolution (2026-09-16)

Both shapes were gates in front of machinery that already existed, not
missing lowerings.

**Shape 1 -- `match` on a by-value ADT with a colored arm.** The CPS match
emitter (`emit_match`, `emit_cps_ir.c`) has had a by-value scrutinee path
since SR1: it binds a local copy and reads through its address, exactly as
the direct emitter's switch does. What kept `(match (MkWrap 1) (MkWrap n) (+
n (g)))` out was the translation gate `match_dk_ok` (`cps_ir.c`), written for
boxed tagged sums (`n_ctors >= 2`, so a `tag` word exists). It now admits a
by-value record or flat sum (`adt_is_byvalue_product`, not `:heap`), and the
emitter treats a single-constructor record -- which has no `tag` member --
as one unconditional arm. All-nullary enums stay out on both paths (the
value is the bare tag), and whether the by-value atom may cross a DK slot is
still `slot_box_ty`'s question, asked on the scrutinee atom as before.

**Shape 2 -- a by-value aggregate `handle` result handed to a call.** The
core check refused a by-value ADT atom as a cps->direct call argument on the
premise that the callee's direct C signature takes it through the int64
carrier. That is true of a generic base (`(Option A)`), and false of a
non-generic callee, whose signature IS the aggregate (`wrap_val(tur_adt_Wrap
w)`); the CPS typed-argument renderer already passes such an atom raw. The
gate (`call_args_ok`) now admits the atom when the callee's declared
parameter spells the same aggregate C type and is not pass-by-pointer.

The report's discriminator was slightly off: "a defstruct record is fine"
was about the CONSUMER -- `(.v ...)` is a delegated field read -- and a
defstruct record handed to a CALL evicted identically (`box-val (handle
...)`). Both are pinned.

Pinned by `cps-match-byvalue-scrutinee-colored-arm` (single-ctor record,
by-value sum with colored calls in every arm, trailing catch-all) and
`cps-handle-byvalue-adt-result-into-call` (defdata record, by-value sum,
defstruct record, plus the field-read control), both agreeing with
`--interpret`.

Found on the way and left alone: a `perform` whose continuation ends in a
cps->cps tail call is refused by `perform_body_ok` (straight-line
continuations only), and a colored generic taking a tyvar-elemented
parameter sig-rejects by the mono-template invariants. Neither is this
report's shape.
