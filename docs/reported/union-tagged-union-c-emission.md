---
title: Unions never get the documented per-member C union emission
category: Reported
description: Every (A | B) rides the generic tur_tagged_t, so a member that cannot be an int64 has to round-trip through the 8-byte value slot. Three defects came out of that, two of which were not the perf cost this was filed for -- a hard compile error and a silent wrong answer. Both fixed, plus the unowned box; the representation itself is still deferred.
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

Three defects, all in the round trip through the 8-byte value slot, all fixed.
The representation change the report asks for is still deferred, and is now much
less urgent -- the cost it was filed to remove is mostly gone.

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

**Still open:** only argument position is covered, which is pass 1 of the five
`any` got. A union bound to a local, returned, or held as a temporary still
leaks its box. Closing those needs a union-tagged drop: `__tur_any_drop`
switches on tag values interned by `emit_any_type_id` (`TUR_ANY_ID_BASE + i`,
i.e. >= 1000), while a union tag is a small member index, so calling it on a
union value is a silent no-op today rather than a wrong free. That is the safe
direction, but it means the drop side is a separate piece of work.

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
