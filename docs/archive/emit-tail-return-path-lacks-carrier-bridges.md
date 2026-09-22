# `emit_tail`'s return path carries fewer bridges than `emit_fn_def`'s

**Severity: medium** (latent miscompile) -- the emitter has two places that turn
a function body's value into a `return`, they disagree about which
carrier/straddle bridges to apply, and which one a body gets is decided by an
unrelated question: whether its tail spine happens to contain a self tail call.
Found 2026-09-22 while landing proper-tail-calls T2.

**RESOLVED 2026-09-22.** The ladder is now one function,
`emit_fn_return_spelling`, that both paths call. The `let` arm's matching gap
(face 3 below, found on macOS CI after the first fix) is closed too.

## The two paths

| Path | Reached when | Site |
|---|---|---|
| `emit_fn_def`'s final `else` | the ordinary case | `src/compiler/emit_fns.c` |
| `emit_tail`'s default arm | the body is TCO-routed (`tco_eligible`) | `src/compiler/emit_fns.c` |

Both end in `return <v>;`. `emit_fn_def`'s first walks a long ladder of
result-shape cases -- `box_aggregate_result`, `n_dict_clone > 0`, the active ABI
spec's declared result, the int64-carrier straddle keyed on the body's emitted C
type. `emit_tail`'s had a strict subset: the by-value carrier spill (`tail_bv`),
the RSP1 pass-by-pointer deref, and the MB2 tyvar-carrier cast. Three of
sixteen.

`emit_tail`'s own RSP1 comment said "Mirrors emit_tail" in `emit_fn_def` and
vice versa, which is the giveaway: they were meant to agree and were kept in
step by hand.

## Three faces, all the same defect

**Face 1 -- the by-value carrier spill.** A tail call to a monomorphized
constructor:

```
tests/fixtures/class-superclass-parametric-instance
  error: returning 'tur_adt_Option__int' from a function with
         incompatible result type 'int64_t'
```

`some__spec__tur_adt_Option__int_int64_t` returns the aggregate by value and
needs `emit_fn_def`'s heap spill; `emit_tail`'s `tail_bv` predicate does not
recognize the shape.

**Face 2 -- the pointer/carrier straddle.**

```
tests/fixtures/forall-dict-show
  error: incompatible pointer to integer conversion returning 'const char *'
         from a function with result type 'int64_t'
```

A dict-slot dispatch returning `const char *` into an int64-carrier wrapper.
`emit_fn_def` has the bridge; `emit_tail` did not.

Twenty-four fixtures failed across the dict, van-Laarhoven lens, and
carrier-bridge families.

**Face 3 -- the same divergence in the `let` arm, found on CI.** `emit_tail`
also emits a tail-position `let` INLINE rather than through `emit_let_value`
(it has to: the `any`-drop bookkeeping differs), and that copy was missing the
REVERSE straddle -- a bare temp whose recorded C type is a pointer initialising
an `int64_t` binder. `emit_let_value` bridges it (`init_val_recorded_ptr` /
`_voidp`); the inline arm did not:

```
examples/datalog/datalog.tur
  error: incompatible pointer to integer conversion initializing 'int64_t'
         with an expression of type 'tur_adt_Value *'
```

This one is worth dwelling on, because it is how the defect actually behaves in
the wild rather than in a fixture. On the **macOS** CI leg it was a hard error;
on Linux, `-Wint-conversion` is only a warning, so the same emitted C compiled
and the example **ran wrong and exited 2** -- a silent wrong answer, reported as
"checks clean but exited 2 when run". That arm's own header comment already
recorded the same class of miss once before
(`tail-recursive-let-drops-carrier-bridge`), and noted it was "absent the moment
the recursive call left tail position" -- i.e. it knew the copy was partial.

## Why it was latent

`emit_tail` is entered only for a body `tco_mark` routed there, and before T2
that required a self tail call. Bodies with both a self tail call and an exotic
result shape are rare enough that no fixture had one. T2 widened the routing to
every body with a tail call at all, and all three faces appeared at once.

## Fix

`emit_fn_return_spelling` -- one function, sixteen arms, called by both paths.
The same treatment `inline_c_returns_byvalue_adt` already got, for the same
stated reason ("so the two hand-duplicated copies cannot drift"). Its `tail_e`
parameter is the expression whose value is being returned: `fd->body` for
`emit_fn_def`, and the branch/arm actually in tail position for `emit_tail`,
which is the more precise reading of every result-shape question the ladder
asks.

The extraction was verified as a pure no-op first -- **zero** snapshot churn
with both callers still on their old routing -- and only then was the routing
widened.

Face 3 was fixed in place rather than by a second extraction: the `let` arm
exists precisely because its `any`-drop bookkeeping differs from
`emit_let_value`'s, so it cannot simply delegate. It gained the one missing
bridge.

## What this did NOT unblock

The original filing said merging the paths would let "mutual recursion whose
members return carrier-ABI values reach C tail position too." **That was
wrong**, and measured so. A return that needs a spill or a cast is work after
the call, so such a call is genuinely not in tail position -- merging the ladders
does not change that, and should not. What the merge removed is the *workaround*
that stood in for it: T2's marking no longer refuses a whole body when one of
its tail leaves would need a bridge. Such a leaf is now simply emitted with its
hoist and its check, the way it always was, while its siblings become tail
calls. The exact-C-return-type question still gets asked, but at the call's own
emission, where it is a statement about tail position rather than a veto.

## Residual

Two `-Wpointer-integer-compare` warnings in `examples/datalog/datalog.tur`
(`(val) == (INT64_C(-1))` on a `tur_adt_Value *`) are **pre-existing** -- they
reproduce with T2 and T3 both disabled, and the example runs correctly with
them. They are not part of this report and are not fixed here.
