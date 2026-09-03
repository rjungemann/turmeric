# An untyped closure param fed a niche Vec slot word reads it back as a carrier box

**Severity: medium** -- a silent wrong read on the DEFAULT path (since the
Option niche graduated, 2026-09-03), for one shape the checker admits.  The
residue the [container-element-form plan](../upcoming/container-element-form-plan.md)
CE3 recorded; it became a default-path fact with the graduation.

## Repro

```turmeric
(load "stdlib/string.tur")
(defn main [] : int
  (let [v (:: (vec-new) (Vec (Option String)))
        w (:: (vec-new) (Vec (Option String)))]
    (vec-push! v (some (string/from-cstr "aa")))
    (vec-push! w (some (string/from-cstr "aa")))
    ;; UNTYPED params, ascribed back to the element inside the body
    (println (if (vec-eq? v w (fn [a b] : bool
                                (eq? (:: a (Option String)) (:: b (Option String)))))
               "eq" "ne"))
    0))
```

`vec-eq?` (stdlib/vec.tur, inline-C) hands the comparator `a->data[i]` --
the raw slot word, which for a niche element is the String pointer itself
(CE2).  Inside the closure `a` is an erased `int64_t`, and
`(:: a (Option String))` is the carrier->niche direction of the bridge
(`emit_carrier_bridge`'s niche row, emit_core.c), which UNBOXES:
`(a ? tur_opt_value_checked(a) : 0)`.  That reads the String's first words as
a tag and a payload.  Whether it aborts (the tag check) or hands the payload
`eq?` a garbage word depends on the payload type's layout; on `String` the
probe printed "eq" for the wrong reason.

## Root cause

The bridge decides word-vs-box by the VALUE, never by the type alone (the
plan's Risks section): a hoisted `vec-get` temp is marked in the slot-word
table, and the synthesized `(eq? v w)` comparator's params carry the
`__cmp_slot_` prefix for the same reason.  A user closure's param has no
mark -- the closure is elaborated on its own, before anything knows which
helper will call it -- and an unmarked int64 word ascribed to a niche Option
is, by the carrier contract, a box.  TUR-E0714 covers the STORE side only
(a niche element stored through a fully erased receiver); nothing on the
read side can see an erased closure param.

## The fix on the user's side (works today)

Write the element type on the params:

```turmeric
(vec-eq? v w (fn [a : (Option String) b : (Option String)] : bool (eq? a b)))
```

The closure's C params are then the by-value element and the words are
bridged at the closure boundary through the marked path.  Pinned by
`tests/fixtures/option-niche-vec-closure-cmp` (shape 1), alongside the
let-bound slot word and `(eq? v w)`.

## Fix directions (compiler side)

1. **Mark by helper contract.**  Let a parameter declare that it is fed
   slot words -- e.g. an annotation on `vec-eq?`'s `cmp-fn` -- and have the
   arg loop, when it sees a closure literal in that position, mark the
   closure's params before the closure body is emitted.  This is the
   synthesized comparator's mechanism generalised from a name prefix to a
   declared contract, and it keeps the value-keyed discipline.
2. **Refuse the ambiguity.**  An `(:: p T)` where `p` is an erased closure
   param and `T` is a niche Option cannot know its convention; a diagnostic
   in the spirit of TUR-E0714 ("ascribe the parameter") would make the
   residue loud instead of silent.  Cheaper, and it turns the one silent
   shape into a one-line fix at the site.
