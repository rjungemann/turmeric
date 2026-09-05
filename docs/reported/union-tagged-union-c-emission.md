---
title: Unions never get the documented per-member C union emission
category: Reported
description: Every (A | B) rides the generic tur_tagged_t, so a member that cannot be an int64 has to round-trip through the 8-byte value slot. Four defects came out of that, none of them the perf cost this was filed for -- two hard compile errors, a silent wrong answer, and an unowned box. All fixed except the box's owner; the representation itself is still deferred.
---

# Unions never get the documented per-member C union emission

**Severity: low as filed, medium as found.** The original entry called this a
cost, not a defect -- "works correctly, but every `(A | B)` rides the generic
`tur_tagged_t` `{int64_t tag; int64_t val}`, so wide by-value members must
heap-box into the 64-bit `val` slot (an allocation + indirection the docs said
would not happen)". Two of those three clauses were wrong. The docs had already
been corrected (the union guide documents `tur_tagged_t` in the body and lists
member-union emission under [Deferred](../guides/union-intersection-types-guide.md#deferred)),
and it did **not** work correctly: a union carrying a member that cannot ride the
value slot was broken in both directions, and the allocation had no owner.

Found 2026-09-04 while assessing whether the deferred representation was worth
building.

## What was actually there

FOUR defects, all in the round trip through the 8-byte value slot. Three are
fixed; what remains open is the box's OWNER (1b below), and the representation
change the report asks for is still deferred and now much less urgent -- the
cost it was filed to remove is mostly gone.

The fourth was found on 2026-09-05 and was hiding inside this entry's own
"still open" note: the widen happened at ARGUMENT position and nowhere else, so
a member bound to a local or returned did not leak, it did not COMPILE.

### 1. The by-value member widen mallocs a box nothing frees -- **FIXED**

`any` had exactly this defect and it was closed in five passes
(`docs/archive/any-struct-box-leak-per-widen.md`). Every one of those rules was
keyed on `TY_ANY`, and `TY_UNION` was never added -- so the union path kept
leaking one box per widen after the `any` path stopped. Not because a union box
is harder to own: because the inference never ran for it. Two places:

- `elab_fns.c` -- `_is_ptr_scalar`, which decides whether a parameter joins the
  `nonretain_ptr_param_mask` inference, listed `TY_CSTR`/`TY_PTR_VOID`/`TY_ANY`.
  A `TY_UNION` parameter is the same `tur_tagged_t` carrying the same kind of
  payload pointer, so it answers the same question.
- `elab_call.c` -- the frame-box rule sat in the `expected_arg_kind == TY_ANY`
  branch. The union-inject branch beside it never consulted the mask.

The same program, differing only in the parameter's declared type, under
LeakSanitizer at 1000 iterations:

| parameter | result |
|---|---|
| `x : any` | clean -- payload in the caller's frame, no allocation |
| `x : (Wide \| int)` | `SUMMARY: 32000 byte(s) leaked in 1000 allocation(s)` |

Both are now clean, and the retaining/effectful/indirect callees still decline
and keep the malloc, as they must. `tests/fixtures/union-widen-frame-box`
carries `requires.leak-check`.

One deliberate difference from the `any` site: the union branch also refuses a
closure callee. The mask is indexed by the callee's own parameter vector, and
several branches at this call site shift by one for a closure's env parameter;
rather than decide which convention the mask follows, the union rule declines
outright. A wrong bit here would hand a retaining callee a pointer into a dying
frame -- a dangling pointer, which is worse than the leak it replaces.

**Still open, and re-scoped 2026-09-05.** The clause above said "a union bound
to a local, returned, or held as a temporary still leaks its box". Two of those
three did not leak -- **they did not build**, and that was a separate and larger
defect hiding inside this one.

### 1a. Only ARGUMENT position ever widened -- **FIXED 2026-09-05**

A member value flowing into a union slot has to be tagged: the union's C
representation is `tur_tagged_t`, not the member's own layout. `EX_UNION_INJECT`
was inserted at call arguments (IT4) and nowhere else, so:

```
(let [u (:: (Wide 1 2 3 4) (Wide | int))] ...)   error: invalid initializer
(defn mk [] : (Wide | int) (:: (Wide 1 2 3 4) (Wide | int)))
(defn mk [] : (Wide | int) (Wide 1 2 3 4))       error: incompatible types when
                                                 returning type 'tur_adt_Wide'
                                                 but 'tur_tagged_t' was expected
```

`elab_coerce_to_union` (elab_call.c) is the union twin of `elab_coerce_to_any`,
reached from the ascription path (elab_types.c) and from return position
(elab_fns.c, which needed a `return_union_type` capture alongside the existing
session/app/exists/fn/borrow ones -- `return_kind` alone is a bare `TY_UNION`
with no member list to match against). It deliberately never sets `frame_box`:
that optimisation rests on a CALLEE not retaining the payload for the duration
of one call, and a value bound to a local or returned outlives the expression
that made it. A leak is the safe direction there; a frame box would dangle.

An ANNOTATED let (`(let [u : (Wide | int) (Wide ...)] ...)`) already worked and
is unchanged -- it inlines the binding into the argument position and frame-boxes
it, so it never allocated. `tests/fixtures/union-widen-local-and-return` carries
all four shapes and the pre-fix behaviour of each.

### 1b. The heap box still has no owner -- **STILL OPEN**

Measured: a `(:: member union)` bound to a local in a 1000-iteration loop leaks
**32,000 bytes in 1,000 blocks**. The identical program through `any` is clean.

The reason is the same one this whole entry keeps having: `let_binding_any_freeable`
(emit_expr.c) -- the rule that decides a scope owns a widened box and may drop it
at scope exit -- opens with `if (emit_resolve_type(ctx, b->type).kind != TY_ANY)
return false;`. That is the fourth rule keyed on `TY_ANY` with no `TY_UNION`
beside it.

Adding the key is not enough, and this is the part worth knowing before starting:
the drop CHANNEL emits `__tur_any_drop(name)`, which switches on ids interned by
`emit_any_type_id` (`TUR_ANY_ID_BASE + i`, i.e. >= 1000). A union tag is a small
member index, so that call is a silent no-op on a union value -- safe, and
useless.

The shape of the fix, from having looked at it:

- At a let whose init is an `EX_UNION_INJECT`, the tag is known **statically**,
  so no runtime switch is needed at all -- the drop is an unconditional
  `free((void *)(intptr_t)TUR_UNTAG(u))`, emitted only when the injected member
  is one the widen actually boxed (`emit_type_is_byvalue_adt` and not
  `frame_box`, the same test the inject itself makes).
- What that costs is a PARALLEL drop channel: `any_pending` / `any_scope_drops`
  carry names only, and both drain by emitting `__tur_any_drop`. A per-entry
  "free directly" flag has to reach all four drain sites, or the early-exit
  paths (a `return`, a tail-call back-edge) miss it -- and the natural repro is
  tail-recursive, so a fall-through-only fix would not even cover it.
- A union local whose init is a CALL has no statically-known tag and should
  decline, keeping the status-quo leak.

### 2. A `match` arm binding a by-value member is a hard C compile error -- **FIXED**

```
error: conversion to non-scalar type requested
 7015 |   tur_adt_Wide w_1442 = (tur_adt_Wide)(intptr_t)TUR_UNTAG(__t180);
```

The inject site heap-boxes an aggregate member, so the slot holds a pointer. The
match binder (`emit_expr.c`) cast the slot to the arm type unconditionally, with
no counterpart to either of the inject's two special cases. `EX_ANY_CAST`, the
reading half on the `any` side, has had the deref all along -- the union match
binder is a third reader that never got it.

Independent of how the value arrived (a call argument, a `let`, a pass-through),
so this is not fallout from the frame-box work above; it reproduces on the
malloc path identically.

Nothing in the tree caught it because no fixture ever matched on a by-value
union member. `union-types-basic` says so in its own header: *"The body ignores
the parameter to avoid union-match codegen (IT4)."* That was written to dodge an
unimplemented path, and the dodge outlived the implementation.

### 3. A float member is silently truncated -- **FIXED**

```turmeric
(defn pick [x : (float | int)] : float
  (match x (f : float) f (i : int) 0.5))
(println (pick 7.1))     ;; printed 7
```

A wrong value with no diagnostic. The inject's float rule -- store the IEEE-754
bit pattern rather than an integer conversion -- was keyed on the **tag**:
`if (tag == (int64_t)TY_FLOAT)`. That is the right question only for an `any`
box, whose tag is `any_box_tag_for_type` (the payload's own `TypeKind`, modulo a
struct-lowering rename that cannot yield `TY_FLOAT`). A union tag is a member
index, so the test asked whether the float member happened to sit at position 12.
It never does, so the value took the integer branch: `TUR_TAG(0, (int64_t)(intptr_t)(7.1))`.

Both ends now key on the payload type, which is exactly equivalent for `any` and
correct for a union. The match binder gained the reverse reinterpret, which also
fixes an `any` match on a float -- it had the same mismatch with its own inject.

Note the CLAUDE.md float rule earned its keep here: `7.0` round-trips through the
truncating path unharmed and reports success.

`tests/fixtures/union-byvalue-member-payload` pins both 2 and 3.

## Validation

`bash tests/run.sh` 2783 passed / 0 failed with **zero snapshot drift** -- no
`expected.c` moved, which is itself the finding: nothing in 2781 fixtures
exercised either payload path. `run-leak-check.sh` 79/0, and the three
representation seams (sr2 55/0, option-niche 10/0, sr4 24/0) unchanged.

## What is still deferred (the original ask)

Per-member C union emission: a per-union monomorph typedef with a real member
union when every member has concrete codegen layout, keeping `tur_tagged_t` as
the fallback, unboxing/boxing at the same `EX_ANY_CAST` / inject seams.
`src/compiler/types.c` (`case TY_UNION: return "tur_tagged_t";`) is where the
representation is chosen.

The case for it is weaker than when this was filed. The allocation it was meant
to remove is gone for the dominant shape (argument position), and the two
correctness defects are fixed against the current representation, so they are no
longer arguments for replacing it. What remains is the indirection, the tag word
per value, and the still-unowned box in the non-argument positions listed under
1 -- and that last one is cheaper to close with a union-tagged drop than with a
new representation. It also reaches further than it looks: `tur_tagged_t` is
passed and returned by value through the CPS IR (`emit_cps_ir.c`) and is part of
the inline-C surface.

## Guides to update when the representation lands

- `docs/guides/union-intersection-types-guide.md` -- the representation
  paragraphs, Known Limitations ("Tagged Union Overhead"), and the Deferred
  table row. All three are accurate as of this report.
