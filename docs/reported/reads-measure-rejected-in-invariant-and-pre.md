# A `#reads` measure is accepted only in a parameter refinement, rejected in the other three contract positions

**Severity: medium (blocks the headline use case for `loop-invariants`, and is
inconsistent with the C2 mechanism built to solve exactly this).** The purity
gate `TUR-E0375` is applied with no awareness of `#reads` in three of its four
positions -- `:invariant`, `:pre` and a return refinement -- while the
parameter-refinement path accommodates it. So the one mechanism the language
has for "an inline-C measure over borrowed state" reaches a parameter
refinement and nothing else.
Consequence: **no stdlib container's length or element can appear in a loop
invariant**, because every stdlib accessor is inline C. Filed 2026-10-02, found
looking for an illustrating use case for `loop-invariants` and hitting the
rejection on the first and most obvious one.

## The differential

One `#reads` measure, four positions. Only the parameter refinement accepts it:

```turmeric
(defn vlen [^borrow v : (Vec int)] #reads v : int
  (vec-len v))
```

| position | predicate | result |
| --- | --- | --- |
| parameter refinement | `i : #refine{ x : int \| (and (>= x 0) (< x (vlen v))) }` | **accepted** |
| `:invariant` | `(while (< i (vlen v)) :invariant (and (>= i 0) (<= i (vlen v))) ...)` | **`TUR-E0375`** |
| `:pre` | `:pre (<= n (vlen v))` | **`TUR-E0375`** |
| return refinement | `: #refine{ r : int \| (<= r (vlen v)) }` | **`TUR-E0375`** |

```
error [TUR-E0375]: contract predicate has side effects; predicates must be pure
note: evaluating this predicate changes program state, so whether the check is
      compiled in becomes observable
```

The control establishes that `#reads` is what makes row 1 pass: swap `(vlen v)`
for a raw `(vec-len v)` in the same parameter refinement and it is `TUR-E0375`
too.

## Why this matters more than one rejected spelling

`vec-len` is declared `#fx{}` -- pure on the effect row -- and its body is
inline C, which the refinement purity walk treats as proven-impure
(`elab_fns.c:855`: inline-C bodies are "themselves IMPURE by `EX_INLINE_C`").
Every stdlib container accessor is inline C. So the canonical loop-invariant
example cannot be written:

```turmeric
(while (< i (vec-len v)) :invariant (and (>= i 0) (<= i (vec-len v))) ...)
```

That is the bounded-index walk both
[loop-invariants-plan](../upcoming/loop-invariants-plan.md) and
[ecs-refinement-typed-apis-plan](../upcoming/v1/ecs-refinement-typed-apis-plan.md)
cite as the motivating use case. The workaround is to pass the bound as a plain
`int` parameter and never mention the container in the invariant, which works
(measured -- see the RE2 probe update of 2026-10-02) but means the feature
cannot talk about the data structure it is iterating.

## Root cause

`rt_diag_impure_pred` (`src/compiler/elab_fns.c:357`) is the sole `TUR-E0375`
emitter. It has no `#reads` awareness. It has four call sites, and only one of
them is guarded:

| call site | position | guarded? |
| --- | --- | --- |
| `elab_fns.c:404`, inside `rt_inject_param_checks` | parameter refinement | **yes** -- `if (rt_pred_reads_measure(e, ct_preds[ci])) continue;` at `elab_fns.c:387` returns before reaching it |
| `elab_fns.c:538`, inside `elab_loop_invariant_pred` | `:invariant` | no |
| `elab_fns.c:10674` | `:pre` | no |
| `elab_fns.c:478`, inside `rt_wrap_return_check` | return refinement / `:post` | no |

`elab_loop_invariant_pred` also hard-bails on the next line
(`if (rt_expr_definitely_impure(pred_e)) return NULL;`), so the invariant is
dropped entirely rather than kept as a runtime-only check.

The guard at `:387` was written for a narrower purpose than the one it ends up
serving: its comment explains it suppresses the runtime *entry check* for a
`#reads` parameter refinement, because the crossing proof is the enforcement
point. Skipping the injection happens to skip the `TUR-E0375` emit two lines
later. So the accommodation in row 1 is partly incidental, which is probably
why it was never extended to the other positions.

## Is `TUR-E0375` right to fire here?

CT1's rationale (the comment at `elab_fns.c:340`) is about **observability of
evaluation**: a predicate with side effects makes behaviour depend on whether
its own checks were compiled in, since `--no-contracts`, a release build, and
static discharge all change how often it runs. `(>= (tick) 0)` is the example.

A `#reads` measure does not have that property. Reading borrowed state is
observationally neutral no matter how many times it runs -- and inside a
`frozen` region, which is where the congruence grant applies, nothing can
change it. So for CT1's own stated concern, a read-only measure is as good as
pure, and the rejection is over-broad rather than protective.

Two caveats worth stating, because they decide where the fix goes:

1. **`#reads` is a trusted annotation, not a verified one.** An inline-C body
   declaring `#reads v` could write. That is the known trust boundary --
   `TUR-W0383` reports the cases where the compiler can see the violation, and
   [trusted-refinement-claims-plan](../archive/trusted-refinement-claims-plan.md)
   owns the general question. Honouring `#reads` at the CT1 gate extends that
   existing trust to one more position; it does not create a new kind of trust.
2. **Accepting it is useful even outside a `frozen` region.** Outside one, a
   `#reads` measure gets a fresh symbol per occurrence, so the invariant proves
   nothing -- but it can still be *runtime-checked*, which is strictly better
   than being rejected. Today you get neither.

## Fix directions

**Preferred: teach the gate, not each call site.** Add the `#reads` test inside
`rt_diag_impure_pred` so all four positions agree, rather than copying
`rt_pred_reads_measure` to three more places. CT1's concern is writes; the
predicate for "may this be evaluated repeatedly without being observable"
is what the gate actually wants, and `#reads`-ness answers it. This also
removes the incidental quality of the row-1 accommodation -- the guard at
`:387` can then go back to meaning only what its comment says.

Then, separately, in `elab_loop_invariant_pred`: drop the
`rt_expr_definitely_impure` hard-bail for a `#reads` predicate so the
invariant survives as a runtime-checked contract even where it cannot be
proved.

**Narrower, if the gate change is contentious:** guard only
`elab_loop_invariant_pred` and the `:pre` site. Smaller blast radius, leaves
the inconsistency in place at `elab_fns.c:478`, and leaves the next contract
position to rediscover this.

**Orthogonal and worth doing regardless:** the `#fx{}`-versus-purity-walk
divergence on `vec-len` is confusing on its own terms. A reader sees a `#fx{}`
row and reasonably concludes the function is pure. Either the purity walk
should have a small allowlist of stdlib accessors it accepts as pure
primitives (the same way it knows `+`), or the stdlib accessors should stop
being inline C. The second is the real fix and is the same rewrite that
`trusted-refinement-claims-plan`'s R4 is blocked on -- making the measure
layer hold its state in Turmeric-visible structs rather than a malloc'd block
behind inline C -- so the two should land together rather than separately, as
that plan's trigger note already argues for the ECS case.

## Acceptance

A fixture in each of the three rejecting positions (`:invariant`, `:pre`,
return refinement) with a `#reads` measure over a `^borrow` container:
accepted, runtime-checked outside `frozen`, and proved (check elided) inside
it. Plus the negative control that a raw inline-C accessor with no `#reads` is
still `TUR-E0375` in all four -- the gate must narrow, not disappear.

## Relationship to graduating `loop-invariants`

This is the gap I would close before graduating that row. The two declines in
[loop-invariant-declines-more-than-soundness-requires](loop-invariant-declines-more-than-soundness-requires.md)
make the feature prove less than it could; this one stops it from being
written at all for the use case it was built for.
