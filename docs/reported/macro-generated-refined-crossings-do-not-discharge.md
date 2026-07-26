# Macro-generated refined guards/crossings do not discharge (only `~@body`-spliced user forms do)

**Severity:** medium (blocks any macro that GENERATES a refined guarded read --
an ergonomic `for-each-alive`, accessor-generating macros, etc.; the hand-written
form and `~@body`-splicing macros are fine). Not a soundness hole -- the failure
is a spurious `TUR-W0372` (unproven), never a wrong proof.

## Summary

A guarded refined read discharges when written by hand, but NOT when a macro
*generates* the guard and/or the crossing (as opposed to splicing the user's
forms through `~@body`). All three of these are `TUR-W0372` while the identical
hand-written code is `1 proven`:

```turmeric
;; hand-written -> 1 proven
(let [__f (& w)] (if (alive? w e0) (get! w e0) -1))

;; macro generates BOTH the guard and the read -> W0372
(defmacro guarded-read [w e] `(if (alive? ~w ~e) (get! ~w ~e) -1))
(let [__f (& w)] (guarded-read w e0))

;; macro generates ONLY the read -> W0372
(defmacro do-read [w e] `(get! ~w ~e))
(let [__f (& w)] (if (alive? w e0) (do-read w e0) -1))

;; macro generates ONLY the guard -> W0372
(defmacro do-guard [w e body] `(if (alive? ~w ~e) ~body -1))
(let [__f (& w)] (do-guard w e0 (get! w e0)))
```

(`alive?` is a `#reads w` measure; `get!` takes
`e : #refine{ x | (alive? w x) }`; `w` is frozen by the `(& w)` borrow.)

## Boundary: `~@body`-spliced user crossings DO discharge

The `ecs/freeze` `frozen` macro splices the user's body via `~@body`, so the
crossing is the USER's form and retains its source span --
`tests/fixtures/refine-stateful-frozen-macro` proves (that was fixed by
`rt_form_ident`, a span-based crossing match, see
`docs/archive/frozen-macro-breaks-refinement-guard-discharge.md`). The cases
above are different: the crossing/guard are RECONSTRUCTED by the quasiquote
template (`(get! ~w ~e)`, `(alive? ~w ~e)`), not spliced from the user.

## Root cause (direction)

Two channels, both in the crossing path-condition collector (`elab_fns.c`):

1. **Macro-generated CROSSING not matched** ("generates only the read" fails,
   with a hand-written guard). `rt_push_cs_path_conds` finds the crossing in the
   caller body via `rt_form_occurrences` using `rt_form_ident` (pointer OR
   source `Span` + head + arity). A `~@body`-spliced user crossing keeps its
   source span; a template-GENERATED crossing gets a synthesized/macro-def span
   that differs between the registered `cs->call_form` and the captured
   `cs->caller_body` copy, so occurrences is 0 and the guard is dropped.
2. **Macro-generated GUARD's measure not matched** ("generates only the guard"
   fails, with a hand-written read). The generated guard's `alive?` may carry a
   hygiene mark, so it does not match the refinement predicate's `alive?` by
   name when the encoder tests congruence.

## Fix directions

- Make crossing identity survive template generation: match a generated crossing
  to its caller-body copy by structural equality (head + arg spelling) when the
  span differs, keeping the `!= 1` ambiguity guard.
- Ensure a macro-template free identifier used as a measure (`alive?`) resolves
  to the same binding/name the refinement predicate uses, so congruence matches.

## Impact / workaround

Blocks an ergonomic `for-each-alive` (or any accessor-generating) macro from
producing code whose refined reads discharge. Until fixed: hand-write the guarded
read, or write the macro to SPLICE the user's guard+read via `~@body` rather than
generate them. See `docs/upcoming/v1/ecs-refinement-typed-apis-plan.md` RE1 (c):
the recursive refined loop is proven by hand (named-let/letrec), so the ergonomic
macro is the only piece this blocks.
