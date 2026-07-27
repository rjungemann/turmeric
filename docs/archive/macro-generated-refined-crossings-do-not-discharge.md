# Macro-generated refined guards/crossings do not discharge (only `~@body`-spliced user forms do)

**RESOLVED (2026-07-26).** Fixed by recording each macro call's expansion
(`refine_note_macro_expansion`, called from the macro dispatch in
`elab_call.c`; table on `Elab.refine_mexp_*`) and letting the crossing path
walk traverse INTO expansions: `rt_form_occurrences` and
`rt_collect_path_conds` walk a macro call AS its recorded expansion (not its
raw arguments -- walking both would double-count a `~body`-spliced crossing),
and `rt_form_mentions_set` scans expansions so a template-hidden `set!`
declines like a written one. That fixes BOTH channels at once: the generated
crossing is reachable (channel 1), and the generated guard is collected
verbatim from the expansion so congruence matches (channel 2 -- there was no
hygiene mark; the template's `alive?` is the same interned symbol). All three
report shapes plus a nested macro-in-macro chain now prove
(`tests/fixtures/refine-macrogen-crossings`); adversarial negatives stay
rejected (no guard, wrong entity, inverted guard, no frozen region, `set!`
hidden in the expansion -- `tests/fixtures/errors/refine-macrogen-*` -- and a
same-form-spliced-twice ambiguity still declines via the `!= 1` guard). Full
fixture suite green (2372), differential fuzzer 0 soundness bugs. This
unblocks the ergonomic `for-each-alive` macro (RE1 (c)).

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

## Root cause (traced)

Two channels, both in the crossing path-condition collector (`elab_fns.c`):

1. **Macro-generated CROSSING is absent from the caller body** ("generates only
   the read" fails, with a hand-written guard). Instrumented, the generated
   crossing reports `rt_form_occurrences(caller_body, call_form) == 0`
   (`[push] call='get!' ... occ=0`) -- so `rt_push_cs_path_conds` declines before
   collecting any guard. Zero, not a span mismatch: the `call_form` (the
   post-expansion `(get! w e0)`) simply does not appear in `cs->caller_body` at
   all. This is consistent with the caller body being the SOURCE defn body (the
   macro call `(do-read w e0)` unexpanded): a `~@body`-spliced USER crossing IS
   present in the source (inside the macro call's arguments, found by the
   generic walk), but a template-GENERATED crossing exists only after expansion,
   so the source walk never reaches it. `rt_form_ident`'s span fallback cannot
   help -- there is no node to match. Fix candidates: walk the POST-expansion
   body for path conditions (at the cost of re-exposing the copied-crossing case
   `rt_form_ident` was added for), or expand macro calls encountered during the
   path walk.
2. **Macro-generated GUARD's measure not matched** ("generates only the guard"
   fails, with a hand-written read). Here the crossing IS in the source, so it is
   found; the generated guard's `alive?` likely carries a hygiene mark, so it
   does not match the refinement predicate's `alive?` by name when the encoder
   tests congruence. (Not yet instrumented; the channel-1 evidence is direct.)

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
