# A typed `fmap` on a heterogeneous Result/Either prints a cstr payload as a raw word

**Severity: high** -- a silent wrong answer on the TYPED path, no diagnostic,
no panic. Found 2026-09-10 while fixing `saffron-dynamic-surface-pass` M2, as a
baseline measurement of code the M2 change deliberately leaves untouched.

## Repro

```turmeric
(defn main [] : int
  (let [e : (Result int cstr) (err "bad")
        n (fmap e (fn [x : int] : int (* x 2)))]
    (match n (Ok v) (println v) (Err s) (println s))
    0))
```

Prints a number of the shape `94884486443026` -- the string's address -- where
`bad` is expected. The `(ok 21)` arm of the same program prints `42`
correctly, because an int payload IS an int64.

Either shows the same thing on its varying arm:

```turmeric
(load "stdlib/either.tur")
(defn main [] : int
  (let [e : (Either int cstr) (Right "x")
        m (fmap e (fn [s : cstr] : cstr s))]
    (match m (Left a) (println a) (Right b) (println b))
    0))
```

Prints `94257618870290`, not `x`. `(Left 7)` prints `7`.

## Root cause (measured)

Both instances are partially-applied heads -- `Functor [(Result _ B)]`,
`Functor [(Either E)]` -- and both `fmap` calls ride the erased carrier
`__inst_Functor_fmap_<X>_tyvar`. On that path the dispatch RESULT TYPE never
grounds: it stays the def-less `(type-app ? ?)` shell, so the `match` on it has
no payload types and every arm binder defaults to the int carrier. The `Err s`
/ `Right b` binder is therefore an `int64_t`, and `println` prints the pointer.

The un-grounding is the same missing binding M2 was about: the instance's own
head tyvar (`B` / `E`) is not a class variable, so the class-method unification
never binds it and `m7_byvalue_grounded` stays false. M2's fix binds it, but
only where doing so is slot-safe -- a by-value-bodied method on a HOMOGENEOUS
receiver (the Saffron all-`any` case), because T4's hole erasure reversed the
class's reading of `(f a)` for a hole-at-0 head (`a := cstr`, the err arm, on
`(Result int cstr)` -- measured) and reconstructs `(f b)` with the arms
swapped. A heterogeneous receiver is exactly the case that cannot be grounded
by value without a hole-preserving representation of the class-variable
binding, so it deliberately stays on the carrier, where this bug lives.

Either is a different sub-case: its `fmap` body delegates to `either-map`, so
it is not by-value-expressible (`m7_body_byvalue_ok == 0`) and can never take
the by-value route regardless of the receiver.

## Why the obvious fix is not safe

There IS an arm for "ground the precise result type for the CONSUMER while the
producer stays on the carrier" -- the `byval_agg` branch in the M7 dispatch
code (`method-result-functor-inference`), which commits `result_type =
substituted` without minting a spec and relies on the consumer to bridge the
carrier to the aggregate. Binding Either's `E` un-gated reached that arm and
produced `tur_adt_Either__int__cstr m = __ps_N;` -- an invalid initializer,
because the bridge does not cover a let-init. So that arm's consumer-side
bridge is incomplete, and that is the real fix location for this bug: make it
cover every consumer position (let-init included), then ground the head tyvar
for the carrier-bodied and heterogeneous cases so the consumer sees real
payload types.

Do NOT fix this by grounding heterogeneous hole-at-0 heads by value: the slot
swap turns the printed-address bug into a miscompiled body (`g` called on the
err arm).

## Guides to update when fixed

- `docs/guides/typeclass-guide.md` -- the HKT instance section should say that
  a partially-applied instance head is on the carrier path and what that
  implies for payload types, until this is fixed.
