---
title: M7 layer-4 by-value emit does not yet handle the Monad `bind` body shape
  (tail branch is a `(m b)`-returning call, not an in-body construct)
severity: medium (blocks the by-value monadic HKT instances -- Monad `bind`,
  MonadError, Applicative `ap` -- for Phase 4.2; flag-gated, default-OFF, so no
  effect on the shipped path; not a miscompile -- the probe fails to compile)
status: open
---

# M7 layer-4: by-value emit for the Monad `bind` body shape

## One-line summary

With the kind-check prerequisite resolved
(`docs/archive/m7-hkt-fn-returning-applied-type-kind-mismatch.md`), the Monad
`bind` probe now elaborates under `TUR_M7_HKT=1` but the layer-4 by-value
instance-method emit does not engage: the instance method still emits the int64
carrier return while the by-value consumer reads `Option__int` (the same
carrier-vs-by-value mismatch the Functor `fmap` shape had before layer-4
landed). So `docs/upcoming/v2/m7-hkt-probe-bind.tur` fails to compile instead of
exiting 21.

## Repro

```sh
TUR_M7_HKT=1 ./build/tur run docs/upcoming/v2/m7-hkt-probe-bind.tur
```

Observed: gcc warns "array subscript 'tur_option_t[0]' is partly outside array
bounds" / "used uninitialized" at the `unwrap` call site, because
`__inst_MyMonad_mbind_Option` emits
`{ int64_t *__tur_ret_p = malloc(sizeof(int64_t)); ... return ...; }` (carrier)
while the consumer reads the result as a by-value `Option__int`.

## Root cause

The layer-4 by-value path engages only when the elaborator attaches the HKT
element-tyvar `abi_bindings` to the dispatch call, which is gated on
`m7_body_constructs_byvalue` (`elab_typeclasses.c`). That gate requires the
instance body's tail (through if/do/let) to be a `#{Construct}` call
(`some`/`none`/`ok`/...). The `bind` body is

```turmeric
(if (some? ma) (k (.value ma)) (none))
```

whose then-branch `(k (.value ma))` is a **call to the continuation `k`**
(returning `(m b)`), not an in-body construct -- so the gate rejects the whole
body and no by-value spec is interned.

## Fix directions

1. **Gate:** extend `m7_body_constructs_byvalue` to also admit a tail branch
   that is a call whose result type is the method's own `(f b)` result family
   (so bind's `(k ...)` qualifies), while still excluding tails that delegate to
   a *carrier* helper (the `Bifunctor [Result] -> result-bimap [container :int]`
   case deliberately excluded today). The distinguishing signal is that `k` is a
   fn-typed **parameter** returning the result family by value, vs. a global
   carrier-returning defn.
2. **Emit:** ensure the continuation `k` (a `(fn [a] (m b))` value) is invoked
   so it returns the `(m b)` struct **by value**. This is the closure-ABI half:
   `k`'s result must be the by-value `Option__int`, not the int64 carrier. The
   fmap shape only needed `k`'s result to be a raw element `b`; the monadic
   shape needs it to be a by-value aggregate, which interacts with how
   fn-valued arguments are lowered (the `:fn` / fat-closure carrier vs a typed
   by-value-returning thunk).

## Validation

- `TUR_M7_HKT=1 ./build/tur run docs/upcoming/v2/m7-hkt-probe-bind.tur` exits 21.
- The Functor `fmap` probe and its element-type variants stay green.
- `bash tests/run.sh` stays 1683/0 (flag-off inert); no flag-on regressions.
