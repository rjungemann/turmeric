# Widening a by-value struct/record-ADT to any mallocs a box that is never freed

**Severity: medium** on hot paths, low otherwise -- one leak per widen,
documented as accepted in the union/intersection guide. Found in the
2026-08-20 docs audit.

**Narrowed 2026-08-29 in three passes.** All three places an `any` payload box
can be owned now own it. What is left is two shapes that need real
inter-procedural / all-exits ownership, described precisely below rather than
as "widening leaks".

**Pass 1 -- argument position does not allocate.** When the callee provably
neither retains the `any` nor suspends, the payload copy goes in the caller's
frame, so there is nothing to own. The reported repro -- a loop passing a
by-value struct to an `[x : any]` parameter -- is leak-clean, pinned by
`tests/fixtures/any-widen-frame-box`.

**Pass 2 -- an owned `any` local is dropped at scope exit.** Pass 1 cannot help
where the `any` outlives the expression that built it (a value RETURNED as
`any`): a caller-frame copy would dangle. Such a value lands in a local, and a
local is where ownership can be settled -- if the name does not escape, the body
is its last use. Pinned by `tests/fixtures/any-widen-local-drop`.

**Pass 3 -- an owned `any` temporary is dropped after the call that consumes
it.** The case with no ownership written down anywhere: `(reads (ret-any i))`
binds nothing. Two facts settle it, and both are needed. Callee-side,
`returns_fresh_any` says the producer's body tail is a widen, so every call
mints a box that aliases nothing -- "the argument is a call returning `any`" is
NOT enough, because a function handing back an `any` it was *given* returns an
alias, and dropping that frees a box its other holder still uses. Caller-side,
the consuming parameter is non-retaining and effect-free, so the box is dead
the moment that call returns. Pinned by `tests/fixtures/any-widen-temp-drop`,
which also carries the alias shape that must NOT be dropped.

The drop hangs off the panic-signal hoist, which is the only place a call is
guaranteed to have been materialized into a statement (`__ps_N = f(...)`
followed by its check). That is the statement boundary an earlier pass of this
report recorded as missing; it exists, just not where the call is assembled.

| Shape | Leaks? | Why |
|---|---|---|
| widen as an argument to a non-retaining, effect-free direct call | no | pass 1 -- never allocated |
| `any` bound to a non-escaping local | no | pass 2 -- dropped at scope exit |
| `any` temporary from a `returns_fresh_any` producer, consumed by a non-retaining effect-free call | no | pass 3 -- dropped after the call |
| `any` local in a scope with an early exit (a tail-recursive loop body, a `return`) | **yes** | the scope-exit collection declines when an early exit could skip the free. Conservative and a leak, never a UAF -- the same limitation the closure-env and catch-box frees have, since all three are trailing frees rather than drops on every exit |
| `any` returned by a callee that hands back one it was GIVEN -- `(reads (alias tmp))` | **yes** | `returns_fresh_any` is a callee-side fact and cannot see that THIS caller's argument was itself a temporary. Distinguishing them is ownership flow through the callee |
| callee that retains, has an inline-C body, may suspend, or is called indirectly | n/a | those keep the heap box by design; `tests/fixtures/any-widen-retaining-callee` pins them, including the effectful case, which became testable when [perform-in-fn-with-any-param-has-no-cps-lowering](../archive/perform-in-fn-with-any-param-has-no-cps-lowering.md) landed |

Getting any of these conditions wrong turns a leak into a dangling pointer, so
the shapes that must decline carry as much coverage as the ones that must
accept -- `any-widen-retaining-callee` and the `alias` case in
`any-widen-temp-drop`.

## What is actually left

Two shapes, both needing more than a local rule:

1. **A local in a scope with an early exit.** The free is emitted after the
   body; a `return` or a TCO'd tail call jumps past it. Fixing it means
   dropping on every exit edge, which is a drop-obligation pass -- and it would
   fix the closure-env and catch-box frees in the same stroke, since they share
   the limitation.
2. **An alias returned by a callee.** `(reads (alias tmp))` leaks because
   nothing can tell, at the call site, whether the `any` a callee returned was
   freshly minted or handed through. That is ownership flow across a call
   boundary -- an effect/ownership annotation on the result, or an
   interprocedural pass.

Neither is a special case waiting to be written; both are the same missing
piece, which is why this stays open rather than being archived.

## Repro

A loop passing a by-value struct to a `[x : any]` parameter grows RSS
linearly.

## Root cause

src/compiler/emit_expr.c:3834-3836 --
`({ T *__tur_box = malloc(...); *__tur_box = ...; TUR_TAG(...); })` with no
ownership fold or drop glue for the box.

## Fix direction

Give the `any` box drop glue (or arena/ownership-fold treatment mirroring
`type_is_boxed_container_elem` elements), or intern constant widenings at file
scope like the fat-shim boxes.

### What the 2026-08-29 narrowing did instead

Neither of those: for the argument case there is a cheaper answer than owning
the box, which is not allocating one. `elab_coerce_to_any` marks the widen
`frame_box` when the callee's `any` parameter is inferred non-retaining and its
declared effect row is empty; the emitter then spells the payload as a
block-scope local and tags its address, instead of the `malloc` + `TUR_TAG`
statement-expression.

Non-retention reuses `nonretain_ptr_param_mask` rather than adding a parallel
mask -- it already answers exactly this question ("does this body keep a
pointer this parameter carries?") for the closure-env and catch-box frees, and
two mechanisms deciding one thing is how these seams go wrong. Two walks needed
teaching for it to fire at all:

- `expr_subtree_has_inline_c` reported "may hide inline-C" for `EX_ANY_TYPE_OF`
  / `EX_ANY_IS` / `EX_ANY_CAST` / `EX_GET_FIELD` via its conservative `default`,
  which switched the whole inference off for any body that so much as read an
  `any` or a struct field. That also silently disabled the catch-box
  confinement, which has always shared this mask.
- `box_uses_confined` had no cases for the three `any` readers, so a bare read
  fell through to the strict escape walk and counted as retention. Each reader
  yields something that cannot alias the payload box (a tag, a bool, or -- for
  the boxed by-value payload this rule exists for -- a deref copy).

Passes 2 and 3 then took the two positions the frame box cannot reach -- a
value returned as `any` (owned by the local it lands in) and one never bound at
all (owned by the expression that consumes it) -- both through
`__tur_any_drop`, whose per-id boxed flag is interned by `emit_any_type_id`
from the same predicate the widen uses, so "the drop frees it" and "the widen
allocated it" cannot drift.

What the original directions were reaching for is still the answer to the two
rows left in the table above: a genuine drop-obligation pass. The difference is
that it is now needed only for an early-exit edge and for ownership flow across
a call, not for the ordinary cases.

## Guides to update when fixed

- docs/guides/union-intersection-types-guide.md (the "Note" under
  Boxing/cast/type-of)
