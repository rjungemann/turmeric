# Widening a by-value struct/record-ADT to any mallocs a box that is never freed

**Severity: medium** on hot paths, low otherwise -- one leak per widen,
documented as accepted in the union/intersection guide. Found in the
2026-08-20 docs audit.

**RESOLVED 2026-08-29, in five passes.** Every position an `any` payload box can
occupy now has an owner, and every measured shape is leak-clean under
LeakSanitizer. The passes, each of which took the shapes the one before could
not reach:

1. **Argument position does not allocate.** When the callee provably neither
   retains the `any` nor suspends, the payload copy goes in the caller's frame.
   `tests/fixtures/any-widen-frame-box`.
2. **An owned `any` local is dropped at scope exit.** For a value RETURNED as
   `any`, where a caller-frame copy would dangle. `any-widen-local-drop`.
3. **An owned temporary is dropped after the call that consumes it.** For
   `(reads (ret-any i))`, which binds nothing. Ownership comes from
   `returns_fresh_any` (the producer's tail is a widen) or
   `returns_any_param_idx` (a pure passthrough forwards its argument's
   ownership, and does not otherwise retain it). `any-widen-temp-drop`.
4. **The drop moves off scope exit when the scope's end is unreachable** and the
   binding has a single unconditional consuming use.
   `any-widen-drop-past-early-exit`.
5. **The scope drop fires at early exits.** Passes 2 and 4 still left the
   `is?`-narrowed shape: the narrowing REBINDS the name, so after it nothing
   refers to the `any` binding and pass 4 finds no use to attach to. The box
   therefore has to come off the scope -- which meant the scope drop had to fire
   at a `return` and at a tail-call back-edge, beside the dynvar pops that
   already do exactly that. `any-widen-drop-narrowed`.

Pass 5 needed two things that are worth recording, because both were wrong in
the first draft:

- The escape walk counted `(cast a Pt)` on a bare `a` as an escape, which kept
  every narrowed binding out of the rule. A box is freed only when the tag says
  a widen heap-boxed the payload, and for such a payload the cast emits
  `*(T *)TUR_UNTAG(v)` -- a COPY that cannot alias the box; for any other
  payload the drop is a no-op, so aliasing does not matter. That admission is a
  flag on the walk, set only by the `any` rules: the closure-env and catch-box
  callers still refuse a cast, where a cast result can matter.
- `emit_tail` emits a tail-position `let` INLINE rather than through
  `emit_let_value`, so the bookkeeping in the latter never ran for exactly the
  loop shape this pass exists to fix. It is repeated there, without a trailing
  drop: `emit_tail` always ends in a `return` or a back-edge, both of which fire
  the scope list themselves.

| Shape | Leaks? |
|---|---|
| widen as an argument to a non-retaining, effect-free direct call | no -- never allocated |
| `any` bound to a non-escaping local | no |
| `any` temporary from a fresh producer or a pure passthrough | no |
| any of the above in a scope with a `return` or a tail-call back-edge | no |
| `any` local narrowed by `is?`, in any of those scopes | no |
| callee that retains, has an inline-C body, may suspend, or is called indirectly | keeps the box by design; `any-widen-retaining-callee` pins it |

Getting a condition wrong here turns a leak into a dangling pointer or a double
free, so every accepting fixture carries `requires.leak-check` and runs under
LeakSanitizer, which aborts on a double free as readily as it reports a leak.
The declining shapes carry as much coverage as the accepting ones.

## Not closed by this, and separate

An `any`-typed **global**'s initializer is never widened: `(def ^mut g : any 5)`
emits `tur_tagged_t = long int` and fails to compile, and so does a struct
payload. That is a missing `elab_coerce_to_any` at `def` position, not a leak.
It also means there is currently no way to retain an `any` past a call at all --
a global cannot hold one, and `vec-push!` takes the int64 carrier a two-word
`tur_tagged_t` does not fit -- which is why the "callee retains" guards in
passes 1 and 3 are in place but cannot be exercised by a fixture.

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
