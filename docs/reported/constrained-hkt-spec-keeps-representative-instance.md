---
status: open
severity: high
discovered: 2026-07-29
area: compiler (ABI specialization / instance re-resolution)
---

# A constrained-poly spec dispatches through the representative instance

## Summary

A constrained kind-polymorphic fn resolves its class methods against a
*representative* instance at elaboration time, and emit-side re-resolution is
supposed to specialize that to the concrete type per monomorphization. For a
higher-kinded class it does not fire, so the `__spec__` keeps calling whichever
instance the environment yielded first.

With the autoloaded stdlib that is `Monad [Result]` and `Applicative [Schema]`,
so a poly fn instantiated at `Option` runs Option values through the Result and
Schema instances. It mostly produces right answers by layout coincidence, and
reads out of bounds when the layouts stop agreeing.

**A fix was attempted and reverted** -- the lever is identified and the obstacle
is concrete. See [Attempted fix](#attempted-fix) before starting.

## Scope -- what is and is not affected

Not everything is broken, and the working cases pin down the mechanism:

| Shape | Dispatch | Correct? |
|---|---|---|
| Method call in a rank-2 `forall` body (dict-passed) | dict slot load | yes |
| Return-directed `pure` in the poly fn's OWN body | re-resolved per spec | yes |
| Receiver-directed `bind` in a monomorphized spec | representative | **no** |
| `pure` inside a LIFTED continuation (`(bind x (fn [v] (pure ...)))`) | representative | **no** |

The dict-passed path is genuinely correct -- `hkt-constrained-pure-two-instances`
compiles one body, instantiates it at two Applicatives whose `pure` differs, and
gets 107/207. Only the *monomorphized* path misdispatches.

## Repro 1 -- receiver-directed `bind`, out-of-bounds read

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
     3853 | ... (err((int64_t)((tur_adt_Result *)(intptr_t)(ma))->err_val))
    -1

    $ ./build/tur emit-c /tmp/r.tur | grep -o "__inst_Monad_bind_[A-Za-z0-9_]*" | sort -u
    __inst_Monad_bind_Result_tyvar

There is no `__inst_Monad_bind_Option` in the output at all. The printed value
is right; the out-of-bounds load at offset 16 of a 2-word Option is the bug.

## Repro 2 -- `pure` in a lifted continuation, wrong instance

    $ ./build/tur emit-c /tmp/r.tur | grep -A2 "static int64_t __fn_"
    static int64_t __fn_1304(int64_t v) {
        __auto_type __ps_53 = (__inst_Applicative_pure_Schema((v) + (INT64_C(1))));

`Applicative [Schema]`'s `pure` is `schema/always`, which mallocs
`{12, value, 0, 0}`. The caller reads that pointer as an `Option` --
`is_some` lands on the tag word `12` (nonzero, so "some") and `value` on the
payload. The result is numerically correct **entirely by coincidence**.

This is why `tests/fixtures/hkt-constrained-byvalue-bind-pure` was rewritten to
`(defn just-pure [^m] [^Applicative m x : (m int)] : (m int) (pure 7))`, which
genuinely emits `__inst_Applicative_pure_Option`. Do not put a `bind`-then-`pure`
shape in a fixture until this is fixed -- it will pass while being wrong.

## Root cause

`emit_dispatch_tyvar` (`emit_core.c`) identifies the dispatch variable in three
positions -- ascribed receiver, bare receiver, call result -- and each looks for
a **bare `TY_TYVAR`**. That is a kind-`*` assumption. A `defclass Monad [^m]`
method called on the abstract constructor has receiver `(m int)` and result
`(m b)`: TY_APP spines whose **head** is the dispatch variable. None of the three
checks sees it, `emit_reresolve_disp_type` returns false, and the spec keeps the
baked representative.

The instance is chosen by environment order, which is why `Result` beats
`Option` for `Monad` and `Schema` wins `Applicative`.

## Attempted fix

Two changes, both reverted; the diagnosis they produced is the useful part.

**A. Teach `emit_dispatch_tyvar` the higher-kinded case** -- if the receiver (or
result) is a TY_APP whose head is a tyvar, hand back the whole spine.
`emit_resolve_type` then grounds it to `(Option int)` for the active spec and
instance selection matches head-wise. This works: `bind` re-resolved to
`__inst_Monad_bind_Option` and the out-of-bounds read disappeared. It is
deliberately inert for the scan-time predicate, which re-checks `TY_TYVAR`.

**B. Keep higher-kinded instance methods on the carrier ABI.** Change A alone
makes the ABI machinery mint a *by-value* spec of a **carrier-written** instance
method: `__inst_Monad_bind_Option__spec__int64_t_tur_adt_Option__int_int64_t`,
whose cloned body feeds a `tur_adt_Option__int` to `some_qu(int64_t)`. Guarding
`emit_abi_try_nested_instance_dispatch_redirect` on "class is HKT" fixed that
one route.

**Why it was reverted.** Change A makes HKT re-resolution succeed *globally*, and
by-value spec minting for carrier-written HKT instance methods has **more than
one route**. Guard B covered the redirect; a second route still produced
`__inst_Functor_fmap_Identity__spec__int64_t_tur_adt_Identity__int_int64_t` and
broke all 11 `van-laarhoven-lens-wide-*` fixtures with the identical shape
(`run_hyid(int64_t)` fed a `tur_adt_Identity__int`). Bisected: change A alone
breaks them; change B alone is inert.

The van Laarhoven wide-functor instances are *deliberately* by-value -- Path A/B
exist to mint those twins -- so the guard cannot be "never mint for HKT". The
discriminator has to be the re-dispatched method's own receiver ABI: carrier-
written (`Monad [Option]`) must not be cloned by value; by-value-written (the VL
`Identity` functor) must. A `type_uses_carrier_abi(redisp->param_types[0])` test
did not distinguish them, so that needs its own investigation.

## Fix directions

1. Start from change A -- it is the right lever and is small.
2. Enumerate **every** route that mints a by-value spec of an instance method
   (at minimum `emit_abi_try_nested_instance_dispatch_redirect` and whatever
   produces the `__inst_Functor_fmap_*__spec__*` twin) and apply one shared
   predicate at all of them.
3. Find a discriminator that separates a carrier-written instance method from a
   by-value-written one. The VL fixtures are the acceptance gate: all 11
   `van-laarhoven-lens-wide-*` must stay green.
4. Separately, `pure` inside a lifted continuation needs the closure re-emitted
   per spec. Relaxing `emit_call_dispatches_on_spec_tyvar` to walk to the app
   head was tried and had no effect -- a different mechanism governs that lambda,
   so find it first.
5. Prefer a representative whose layout is widest, or refuse to pick one when
   candidate layouts differ, so a re-resolution miss degrades to a compile error
   instead of an out-of-bounds read.

## Related

- [constrained-hkt-byvalue-carriers.md](constrained-hkt-byvalue-carriers.md)
- `docs/archive/history/constrained-hkt-pure-return-dispatch.md` -- the gap-1
  fix, whose representative selection this inherits.
