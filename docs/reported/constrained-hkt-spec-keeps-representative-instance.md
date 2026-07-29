---
status: open
severity: high
discovered: 2026-07-29
area: compiler (ABI specialization / instance re-resolution)
---

# A constrained-poly spec dispatches through the representative instance, not its own type

## Summary

A constrained kind-polymorphic fn resolves its class methods against a
*representative* instance at elaboration time, and emit-side re-resolution is
supposed to specialize that to the concrete type per monomorphization. For a
constrained poly fn it does not: the `__spec__` keeps calling the representative
`__inst_<Class>_<method>_tyvar`, which is whichever instance the environment
happened to yield first.

With the autoloaded stdlib that representative is `Monad [Result]`, so a poly
fn instantiated at `(Option int)` runs `Option` values through the `Result`
instance. It *appears* to work because the two layouts share a prefix --
`Option{bool is_some; int64 value}` vs `Result{bool is_ok; int64 ok_val; int64
err_val}` -- so the `some`/`ok` path reads the right words by coincidence. The
`none` path does not: it reads `err_val` at offset 16, past the end of a 2-word
Option.

This was masked until the carrier-ABI fix landed, because every by-value
instantiation segfaulted before reaching it.

## Repro

    $ cat > /tmp/r.tur <<'EOF'
    (defn bind-then-pure [^m] [^Monad m ^Applicative m x : (m int)] : (m int)
      (bind x (fn [v] (pure (+ v 1)))))
    (defn nothing [] : (Option int) (none))
    (defn main [] : int
      (println (unwrap-or (bind-then-pure (nothing)) -1))
      0)
    EOF
    $ ./build/tur run /tmp/r.tur
    ... warning: array subscript 'tur_adt_Result[0]' is partly outside array
        bounds of 'tur_adt_Option__int[1]' [-Warray-bounds=]
     3853 | __auto_type __ps_28 = (err((int64_t)((tur_adt_Result *)(intptr_t)(ma))->err_val));
    -1

The printed value is correct; the out-of-bounds read is the bug. Confirm the
callee with:

    $ ./build/tur emit-c /tmp/r.tur | grep -o "__inst_Monad_bind_[A-Za-z0-9_]*" | sort -u
    __inst_Monad_bind_Result_tyvar

There is no `__inst_Monad_bind_Option` in the output at all, even though the
only instantiation is at `Option`.

## Why it is dangerous

The failure mode is a silent wrong-layout read, not a crash. Option and Result
happen to be prefix-compatible, so today it mostly produces right answers and
one out-of-bounds load. Any user-defined by-value functor whose layout differs
would read genuine garbage, and the `-Warray-bounds` warning only fires when the
optimizer can see both types -- it is not a reliable signal.

## Root cause

Not pinpointed. The representative-instance mechanism is deliberate (see
`elab_method_call`'s `obj_is_abstract_tyvar` path and the matching
return-dispatch branch): pick a carrier-compatible instance so the polymorphic
base clone is well-typed C, then let `emit_reresolve_method_call`
(`emit_core.c`) specialize per ABI specialization. For a *constrained poly fn*
the re-resolution is not firing -- the spec is minted (the emitted symbol is
`bind_then_pure__spec__tur_adt_Option__int_...`, so the specialization knows the
concrete type) but the method callee inside it is untouched.

The instance is also chosen by environment order rather than by anything about
the call, which is why `Result` wins over `Option`.

## Fix directions

1. Find why `emit_reresolve_method_call` does not rewrite the callee inside a
   constrained-poly `__spec__`. The spec carries the concrete result type in its
   own mangled name, so the information is present.
2. Prefer a representative whose layout is *widest* (or refuse to pick one when
   candidate layouts differ) so a re-resolution miss degrades to a compile error
   rather than an out-of-bounds read.
3. Fixture: the repro above, asserting the `Option` instance is the one emitted
   -- an output-only assertion passes today and would not catch a regression.

## Related

- [constrained-hkt-byvalue-carriers.md](constrained-hkt-byvalue-carriers.md) --
  the remaining carrier gap (monomorphized spec return).
- `docs/archive/history/constrained-hkt-pure-return-dispatch.md` -- the gap-1
  fix, which uses the same representative mechanism on the return-dispatch side
  and would inherit any fix here.
