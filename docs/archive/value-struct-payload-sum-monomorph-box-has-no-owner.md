# A stdlib `Result`/`Option` monomorph over a value-struct payload heap-boxes it, and nothing frees the box

**Resolved 2026-09-03 -- 9 of 9, see the fourth round below; the glue route
is assessed there and declined.**

**Severity: medium.** On the DEFAULT path, post-SR2a, with no experiment
involved -- so it affects ordinary `(Result MyStruct MyErr)` code rather than
an erased corner. Filed 2026-08-30, found sweeping the leaks RM1's measurement
turned up. **Retitled the same day** (was `wide-payload-...`): the first draft
blamed the B4 ">8 bytes" rule, and that was wrong -- see "The rule, precisely".

## Summary

SR2a's narrowing is recorded in
[carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
as:

> A CONCRETE `(Option T)` / `(Result T E)` monomorph now flows by value, so its
> constructor is a struct literal and there is no box to own.

**That holds for scalar and pointer payloads, not for value-struct payloads.**
When `T` is a non-parametric by-value product (a `defstruct`, or a
single-variant `defdata`), the monomorph's arm stores it as a POINTER and the
specialized constructor mallocs a fresh copy of its argument -- and nothing
releases it.

```c
/* (Result Rational ArithError): both payloads are value-structs */
typedef struct tur_adt_Result__Rational__ArithError {
    int tag;
    union {
        struct { tur_adt_Rational  * _0; } Ok;
        struct { tur_adt_ArithError * _0; } Err;
    } as;
} tur_adt_Result__Rational__ArithError;

static tur_adt_Result__Rational__ArithError
ok__spec__tur_adt_Result__Rational__ArithError_tur_adt_Rational(tur_adt_Rational x) {
    tur_adt_Rational *__t459 = (tur_adt_Rational *)malloc(sizeof(tur_adt_Rational));
    *__t459 = (x);                                   /* <- leaked, one per (ok r) */
    return ctor_Ok__Rational__ArithError(__t459);
}
```

The Result value itself is by value, exactly as SR2a says. The payload under
it is not.

## The rule, precisely

Width is irrelevant. Probed with an 8-byte `(defstruct S8 [a : int])` and a
16-byte `S16`: **both** arms are `tur_adt_S* * _0` and **both** spec ctors
malloc. The rule is `adt_field_is_ros_pointer_box` (types.c):

- the OWNER is `Result` or `Option` **by name** -- a user-defined
  `(defdata MyEither [A B] ...)` over the same payload does NOT take it; and
- the resolved payload is a non-`:heap`, non-parametric, by-value product.

It is deliberate, and `adt_field_c_type`'s comment gives the two reasons: the
monomorph then references `T` only by pointer, so a guarded forward typedef
suffices (the struct-with-Option-field typedef-ordering blocker), and the
8-byte slot matches the carrier-box layout the preamble helpers
(`tur_box_ok`/`tur_box_some`) produce. The malloc that fills the slot is the
`box_adt` promotion in emit_expr.c's ctor-argument path, keyed on the same
three conditions. So "do not box at all" (fix direction 2 below) would be
undoing a layout contract, not just removing an allocation.

"ros" is not width and not read-only-string: it is this rule's own name for
"stdlib Result/Option value-struct payload, stored boxed".

## Repro

`tests/fixtures/rational-overflow`, built against an ASan libturi:

```
Direct leak of 96 byte(s) in 6 object(s) allocated from:
    #1 ok__spec__tur_adt_Result__Rational__ArithError_tur_adt_Rational
Direct leak of 16 byte(s) in 4 object(s) allocated from:
    #1 err__spec__tur_adt_Result__Rational__ArithError_tur_adt_ArithError
```

Any `(ok <struct wider than 8 bytes>)` reproduces it.

## Population, measured

A static sweep of 2182 emitted fixtures finds **11** that emit a specialized
sum constructor whose body mallocs its payload. Running those under
LeakSanitizer, **9 leak with a `*__spec__*` constructor in the trace**, 465
bytes total:

| fixture | bytes |
|---|---:|
| `rational-overflow` | 112 |
| `constrained-instance-element-dispatch` | 109 |
| `byvalue-option-over-parametric-monomorph` | 72 |
| `conv-defstruct-result-struct-field-typedef-order` | 64 |
| `polymorphic-ok-err-value-struct-payload` | 32 |
| `result-over-struct-with-option-field-typedef-order` | 24 |
| `rational-basics` | 20 |
| `instance-method-return-carrier-bridge` | 16 |
| `nested-construct-byvalue-decode` | 16 |

(`rational-arith` emits the shape but runs clean; one more leaks with a
different trace.) Small per fixture, and per CONSTRUCTION -- a loop building
`(Result Rational E)` grows without bound.

## Why this is not the erased-path residue

Distinct from [RM1](../upcoming/reclamation-plan.md)'s subject in every way
that matters for a fix:

- It is the **specialized** path, not the erased base -- the monomorph is
  by-value and the constructor is a `*__spec__*` clone.
- The box is the **payload**, not the sum carrier, so making None null or
  freeing a carrier box does not touch it.
- Ownership is **unambiguous**: the constructor mallocs a fresh copy of its
  argument, so the monomorph that holds the pointer owns it outright. There is
  no `alt-or`-style pass-through hazard here, because the box did not exist
  before the call.

That last point is what makes this more tractable than RM1's residue rather
than less: the freshness question RM1 needed a whole analysis for is settled
by construction.

## Fix directions

1. **Drop glue on the monomorph.** The payload box is owned by exactly one
   monomorph value, so releasing it belongs with that value's lifetime --
   `needs_drop_glue` already exists for `TY_RC`/`TY_REF`/`TY_WEAK`/boxed-fn
   fields and for a nested owning aggregate (`elab_structs.c`), and a
   wide-payload arm is the same kind of owning field.

   **Correction (2026-08-30): `elab_effects.c:30`'s `n_ctors == 1` is NOT the
   blocker**, as an earlier draft of this report claimed. That predicate is
   `owning_byvalue_agg`, which governs effect/multishot admissibility. The
   drop-glue EMITTER already handles multi-variant defs -- `adt_glue_is_tagged`
   (`def->n_ctors > 1`) emits a `switch (s->tag)` with per-variant field loops.

   The real gap is one level earlier, in what MARKS a def as owning
   (`elab_structs.c`): a field is flagged `drop_inner_def` only when the INNER
   def itself `needs_drop_glue`. That tracks boxes whose *contents* need
   releasing; it has no rule for a box that is merely an owned allocation, which
   is what a wide payload is (`Rational` is two int64s and needs no glue of its
   own). And for a parametric monomorph the arms come from tyvars, so this
   field-level machinery never sees `Rational` at all -- `elab_structs.c:1425`
   already flags monomorph glue as "separate work".
2. **Do not box at all.** Store the wide payload inline in the union and let
   the monomorph be wider than 16 bytes. Removes the allocation instead of
   owning it, at the cost of a bigger by-value value on every crossing -- the
   same trade SR4 measured (7-13% time for 2.2x memory) and deliberately did
   not take, so it wants measuring before it is chosen.
3. **Scope-exit drop.** RM1's mechanism, keyed on the monomorph binding
   rather than the carrier. Cheapest of the three, and the narrowest: it
   covers a let-bound result whose uses are accessor-only, and leaves the
   escaping cases (returned, stored) alone.

Direction 1 is the one that scales, because the box has a single owner and the
machinery for single-owner release already exists.

## Direction 3, built: it reaches 2 of the 9 (2026-08-30)

The scope-exit drop (RM1's mechanism keyed on the monomorph BINDING, freeing
the live arm's payload box through a `switch` on the tag) is in, and it is
kept because it is measured rather than assumed: **465 -> 361 bytes**, the
9 leaking fixtures down to 7. It fires in exactly two --
`polymorphic-ok-err-value-struct-payload` (both arms; now fully ASan-clean and
carrying `requires.leak-check`, so the fix has 32 bytes of teeth) and
`byvalue-option-over-parametric-monomorph`. Predicate:
`adt_field_is_ros_pointer_box` over the substituted field type and nothing
else; conditions and escape walk identical to RM1's carrier drop (fresh
producer, by-value binding, accessor-only uses, trailing-only). Snapshot churn:
one fixture. Suite 2748/0 + that one regenerated; leak-check 63/0/0.

A first version of this was reverted the same day after measuring 465 -> 465.
Two findings from that round explain both the miss and the residue:

**The consumption shape is mostly not a let binding.** `rational-basics`
leaks through `(res-ok? (rat/of 3 4))` -- the Result is an argument to a USER
function, not a binding. RM1's accessor-argument mechanism does not reach it
either, and correctly so: `res-ok?` is not a known reader, and a by-value
struct argument could in principle have its arm pointer retained by the
callee. Freeing there needs a non-retention fact about the callee, which is
what `nonretain_param_mask` supplies for closures and would have to be
extended to cover this.

**The predicate discrepancy is resolved, and it was self-inflicted.** The
first attempt tested `type_is_wide_byval_adt(fres) &&
!adt_field_is_ros_pointer_box(def, &fres)` at the drop site -- copied from the
monomorph ctor emitter's `wide_box[]`, which is the B4 rule for a DIFFERENT
boxing. The arm in question is boxed by exactly the predicate that expression
NEGATES. The correct drop-site test is `adt_field_is_ros_pointer_box(def,
&resolved)` over the substituted field type, and nothing else; asked that way
it agrees with the typedef and the ctor-argument path (all three key on the
same rule).

## Direction 3, widened: 5 of the 9 (2026-09-02)

The let-scope drop above reached 2 fixtures because, as the section below
says, the dominant consumption shape is an ARGUMENT, not a binding. Two
mechanisms now cover that shape, and the measurement moved from
**465 -> 213 bytes** (9 leaking fixtures -> 4). Same sweep as before: the 11
fixtures whose emitted C carries a malloc'ing `*__spec__*` sum ctor, built
with ASan, bytes counted per fixture; the spec-ctor frames themselves account
for 104 of the remaining 213.

**Argument position: an inferred non-retaining sum-parameter mask.** A
`defn` whose parameter is a stdlib `Option`/`Result` and whose body only ever
reads that parameter through the known accessors (`some?`, `ok?`, `unwrap`,
`ok-val`, ...) -- the same `box_uses_confined` walk RM1 uses, widened so an
EX_CALL through a reader name or through a callee that itself carries the mask
counts as confined -- gets the bit set in `Binding.nonretain_sum_param_mask`
(`elab_fns.c`, mirroring `nonretain_ptr_param_mask`, including its
scalar-result gate). `elab_call.c` then stamps `sum_box_drop_after` on a
fresh-producer argument in a masked slot exactly as it does for the accessor
readers, and the hoist in `emit_expr.c` pushes the value-struct payload free
onto the pending list. `(res-ok? (rat/of 3 4))` -- the `rational-basics` shape
-- now frees. Inline-C bodies zero the mask, as they do the other two.

**Void consumers: statement-position drain.** A call in statement position
whose result is `: void` never reaches `emit_value`, so the frees its
arguments had pushed stayed on the pending list and were emitted at the wrong
scope (or never). `emit_stmt.c`'s `EX_CALL`/`EX_CALLCC` arms now bracket the
call with `emit_pending_drops_mark`/`emit_pending_drops_drain` (exported from
`emit_expr.c`), draining the `any`, carrier-sum, and value-struct-payload
lists back to the mark right after the call. `outcome : void` in
`rational-overflow` was the trigger.

Now fully ASan-clean and carrying `requires.leak-check`: `rational-basics`
(112 B of teeth) and `rational-overflow`, alongside
`polymorphic-ok-err-value-struct-payload` from the first round. `rational-arith`
and `instance-method-return-carrier-bridge` are also clean of this class.

**The remaining 4 are by design, not by miss:**

- `conv-defstruct-result-struct-field-typedef-order` and
  `result-over-struct-with-option-field-typedef-order`: the consumer is
  `describe [r : (Result User cstr)] : cstr`. The mask's scalar-result gate
  excludes `cstr` results (a returned `cstr` could alias the payload), exactly
  as the `ptr` mask does; lifting that needs an aliasing fact the checker does
  not have.
- `constrained-instance-element-dispatch` and `nested-construct-byvalue-decode`:
  `enc`/`dec` are dictionary-dispatched class methods, and the instance bodies
  are inline C. Dispatch goes through a dictionary slot, so there is no
  `Binding` at the call site to carry a mask, and inline C zeros it anyway.

Both residues are the argument for the glue route (drop glue on the monomorph,
freeing the arm at the consumer's scope exit regardless of what the consumer
is), which is still the fix that scales. Suite 2749/0, leak-check 65/0/0,
option-niche seam 9/0, SR4 seam 24/0.

## Direction 3, third round: 7 of the 9 (2026-09-02, later)

**465 -> 125 bytes** on the same sweep (9 leaking fixtures -> 2; the spec-ctor
frames themselves now account for 16 bytes). Three changes, each found by
reading the next residue rather than assumed:

**The `cstr` result gate did not transfer.** The pointer-scalar mask excludes
a `cstr` result because there the PARAM is the cstr and can be returned as-is.
A sum param cannot become a `cstr` result except by copying a payload word or
struct out through a reader, and a cstr word points at characters, never into
the arm box the drop frees. The sum mask now admits `TY_CSTR` results, which
is exactly the `describe [r : (Result User cstr)] : cstr` shape.

**The let-scope walk did not consult the mask.** The argument-position stamp
(elab) did, but the let-bound shape goes through `binding_escapes_impl`,
whose call arm only knew `nonretain_param_mask`. Under the sum walk it now
also skips a `b` argument in a slot the callee's `nonretain_sum_param_mask`
covers.

**`EX_REINTERPRET` was an unmodeled kind.** The escape walk fell to its
conservative `default` on the carrier-retyping wrap elab puts around an
argument to a polymorphic accessor (`(unwrap o1)` on an erased
`(Option User)`), so every such use read as an escape. The walk now models
the reinterpret as its operand, and the accessor / masked-callee argument
check peels a non-retaining reinterpret before asking whether the argument
is `b`. This is an RM1 fix as much as a payload one: it moved the erased
sweep 7364 -> 7200 and cleared `hkt-partial-app-wildcard-byvalue`.

Freeing that carrier cell then exposed the next layer: the erased
`some__spec__int64_t_tur_adt_User` mallocs the `User` copy and stores the
pointer in the cell, so a shallow cell free turned a 24-byte indirect leak
into a direct one. `emit_carrier_sum_free` now frees the live arm through the
cell first (the same tag walk as the by-value monomorph drop, reached via
`((tur_adt_Option *)(intptr_t)o)->`) when the carrier's static type has a
boxed value-struct payload, and stays shallow otherwise. Both the let-scope
list and the pending list carry the type for this.

Fully ASan-clean and carrying `requires.leak-check` from this round:
`conv-defstruct-result-struct-field-typedef-order`,
`result-over-struct-with-option-field-typedef-order`,
`hkt-partial-app-wildcard-byvalue`.

**The remaining 2 are the class-method shape** and stay by design:
`constrained-instance-element-dispatch` (`(enc (some box))`) and
`nested-construct-byvalue-decode` (`ob (ok-val (:: (dec 0) ...))`). A class
method call is rewritten to `elab_method_call` and dispatched through a
dictionary slot; there is no `Binding` at the call site to carry a mask or a
freshness flag, and the instance bodies here are inline C in any case. The
glue route is the fix for these. Suite 2749/0, leak-check 68/0/0, both
seams green.

## Direction 3, fourth round: 9 of 9 (2026-09-03) -- and the glue route, assessed

**465 -> 0 bytes** of this class on the same sweep.  The two class-method
fixtures were the last, and both were the SITE, not the mechanism:

**Class-method consumers never ran the stamp.**  `elab_call.c`'s
drop-after stamp lives in the ordinary call's argument loop; a class-method
call is built by `elab_method_call` (elab_typeclasses.c) and never passes
through it, so `(enc (some (make-struct Box :n 99)))` had no stamp to
resolve even though the Option instance's mask for `x` was inferred and set.
The same three facts are now asked there: a non-suspending consumer, a
fresh producer in the slot, and the RESOLVED instance's
`nonretain_sum_param_mask` bit.  Static dispatch (receiver head concrete --
`(Option a)` is enough, since the Option instance is the one that runs
whatever `a` is) stamps `sum_box_drop_after` as before.  A receiver that IS
the abstract class var is stamped `sum_box_drop_after_dyn` and admitted
per monomorph by the consumer's argument loop after
`emit_reresolve_method_fndef` -- the same re-resolution the freshness
question already uses -- against that binding's mask
(`EmitCtx.sum_drop_admit`).

**A let binding through a copying reader.**  `ob (ok-val (:: (dec 0)
(Result (Option Box) cstr)))` was never "fresh": `ok-val`'s own body is a
match returning its binder.  But `ok-val` / `err-val` / `unwrap` COPY the
payload out, so when their argument is a fresh temp the copy is the only
holder of any arm box the payload carries -- and a nested sum payload is
stored INLINE in the outer's union (the ros rule boxes only a non-parametric
value struct), so the outer's tag walk can never reach the same box.
`emit_init_owns_fresh_sum` peels those three readers for both let-scope
drops.  `unwrap-or` is excluded: its None path returns the DEFAULT, which may
be a live binding.

**A return-dispatched producer's cell was freed shallow.**  The new fixture
found one more: `(:: (dec tag) (Result A cstr))` inside the Option
instance, resolved to `Dec [Box]`, carries the abstract class variable as
its own type, so `emit_carrier_sum_free` could not see the boxed arm and
freed only the cell, leaking the `ok__spec__int64_t_tur_adt_Box` copy.  The
instance's declared result (`(Result a cstr)`) with `a` substituted by the
re-resolved dispatch type is the cell's real layout; it is built as an
owned spine and the pending list frees it at the drain.  It is never
resolved through the active spec, which binds `a` to the OUTER receiver.

Pinned by `tests/fixtures/sum-payload-drop-dictionary-dispatch`
(`requires.leak-check`: main-level and generic-body static dispatch, the
let-bound reader shape, and the return-dispatched cell) and
`tests/fixtures/sum-payload-dictionary-retaining-instance-not-dropped`
(a `hold [x] x` instance keeps its argument; asserts the VALUE read through
the retained box).  The dynamic-receiver admission has no in-tree program:
every spelling of "a method returning the class variable mints an Option
over a value struct" trips a pre-existing construct-seam miscompile, filed
as [return-dispatched-sum-mint-in-constrained-instance-miscompiles](../reported/return-dispatched-sum-mint-in-constrained-instance-miscompiles.md).
The path is polarity-safe -- it frees only when the re-resolved instance's
inferred mask says the slot is not retained -- and the type fuzzer (seeds 1
and 7) is clean over it.

What is left in the two original fixtures is out of class:
`constrained-instance-element-dispatch` leaks the `cstr` buffers its
inline-C `enc` instances malloc and never free (user code), and
`nested-construct-byvalue-decode` leaks the Box its inline-C `Dec [Box]`
mallocs behind `tur_box_ok` -- a shape
[inline-c-results-guide.md](../guides/inline-c-results-guide.md) says is not
constructible from inline C, so its ownership is undocumented and the
compiler does not free it.

### The glue route, assessed

Direction 1 -- "drop glue on the monomorph, freeing the arm at the
consumer's scope exit regardless of what the consumer is" -- was the fix
this report kept naming as the one that scales.  Looked at against the
tree, it decomposes into two things, one of which already exists and one of
which is not this report's:

- **The glue exists.**  `boxed_struct_payload_walk` (emit_expr.c) IS the
  per-monomorph drop glue: a `switch` on the tag freeing the ros-boxed arm,
  keyed on the one predicate the typedef and ctor key on, reachable by value
  and through a carrier cell.  Every drop site in all four rounds calls it.
  A separately emitted `drop_glue_tur_adt_Result__Rational__ArithError`
  function would only add a symbol for the `rc/of` control-block path,
  which no in-tree program takes for a monomorph.
- **"Regardless of what the consumer is" is an ownership discipline, not
  glue.**  Freeing at the consumer's scope exit needs the consumer to OWN
  its parameter -- a move on call, with the caller forbidden to touch the
  value afterwards -- which is the `needs_drop_glue` move-only discipline
  (`Binding.is_moved`, TUR-E0005) applied to every `(Result Struct E)`.
  That is a language-visible change to the two most-used types: a value
  passed to any function could not be read again.  The residue never needed
  it; it needed the existing per-site analysis to reach the class-method
  site.

So the route as written is declined; what was built instead closes the
class.

## Related

- [carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md)
  -- the erased-path sibling, whose SR2a narrowing this report qualifies.
- [reclamation-plan.md](../upcoming/reclamation-plan.md) -- RM1 (built, erased
  path) and RM2 (the recursive spine, which `re-string`'s `ctor_RxCons` leaks
  belong to, not to this report).
