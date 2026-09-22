# `emit_tail`'s return path carries fewer bridges than `emit_fn_def`'s

**Severity: medium** (latent miscompile, currently unreachable) -- the emitter
has two places that turn a function body's value into a `return`, they disagree
about which carrier/straddle bridges to apply, and which one a body gets is
decided by an unrelated question: whether its tail spine happens to contain a
self tail call. Found 2026-09-22 while landing proper-tail-calls T2.

## The two paths

| Path | Reached when | Site |
|---|---|---|
| `emit_fn_def`'s final `else` | the ordinary case | `src/compiler/emit_fns.c:4560` onward |
| `emit_tail`'s default arm | the body is TCO-routed (`tco_eligible`) | `src/compiler/emit_fns.c:1033` onward |

Both end in `return <v>;`. `emit_fn_def`'s first walks a long ladder of
result-shape cases -- `box_aggregate_result`, `n_dict_clone > 0`, the active ABI
spec's declared result, the int64-carrier straddle keyed on the body's emitted C
type. `emit_tail`'s has a strict subset: the by-value carrier spill (`tail_bv`),
the RSP1 pass-by-pointer deref, and the MB2 tyvar-carrier cast.

`emit_tail`'s own RSP1 comment says "Mirrors emit_tail" in `emit_fn_def` and
vice versa, which is the giveaway: they were meant to agree and were kept in
step by hand.

## Why it does not bite today

`emit_tail` is entered only for a body `tco_mark` routed there, and (since T2)
only when every non-self tail leaf's callee returns exactly the enclosing
function's C type -- which is precisely the condition under which none of the
missing bridges have anything to do. The narrow gate is load-bearing, and it is
narrow *because of this defect*, not for a reason of its own.

## Repro (against the gate as it stood mid-landing)

Marking non-self tail calls by the binding's raw name rather than
`emit_call_name` -- i.e. failing to notice that a monomorphized callee is
emitted under its spec name -- routes these bodies into `emit_tail` and the
missing bridges show up immediately as hard `cc` errors:

```
tests/fixtures/class-superclass-parametric-instance
  error: returning 'tur_adt_Option__int' from a function with
         incompatible result type 'int64_t'
    -> tail call to some__spec__tur_adt_Option__int_int64_t, whose by-value
       aggregate result needs emit_fn_def's spill; emit_tail's tail_bv
       predicate does not recognize the shape.

tests/fixtures/forall-dict-show
  error: incompatible pointer to integer conversion returning 'const char *'
         from a function with result type 'int64_t'
    -> dict-slot dispatch returning const char * into an int64-carrier
       wrapper; emit_fn_def has the straddle bridge, emit_tail does not.
```

Twenty-four fixtures failed this way across the dict, van-Laarhoven lens, and
carrier-bridge families.

## Root cause

Two hand-maintained copies of "how does a result reach the `return`". There is
no shared decision function, so a bridge added to one is not added to the other,
and nothing tells you: the divergence is only observable for a body that is
BOTH TCO-routed AND has one of the exotic result shapes, which no fixture had
before T2 widened the routing.

## Fix direction

Factor the result-shape ladder out of `emit_fn_def` into one function both
paths call -- the same treatment `inline_c_returns_byvalue_adt` already got for
the "by value or carrier?" question, and for the same stated reason ("so the
two hand-duplicated copies cannot drift"). `emit_tail` would then call it in its
default arm instead of carrying three of the cases itself.

That is also the unblock for the other half of proper-tail-calls T2: with the
paths merged, a non-self tail call would no longer need the exact-C-return-type
gate, and mutual recursion whose members return carrier-ABI values could reach C
tail position too. See
[docs/upcoming/proper-tail-calls-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/proper-tail-calls-plan.md)
(T-D2, "What T2 does not reach").
