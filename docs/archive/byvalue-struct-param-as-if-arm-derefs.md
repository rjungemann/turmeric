# A by-value struct parameter used directly as an `if` arm is dereferenced

**Severity: medium.** A check/build divergence: `tur check` is clean and cc
rejects the emitted C. Not a wrong answer -- it never compiles -- but the
diagnostic names an internal temporary and points at C, not at the `if`.

**Status: RESOLVED 2026-09-13.** `emit_arm_is_byval_agg_var`
(`src/compiler/emit_expr.c`) -- the DECLINE that says "this arm already holds
the merge temp's aggregate, do not bridge it" -- keyed off
`emit_type_is_byvalue_sum`, which requires `n_ctors > 1`.  A lowered
`defstruct` is a SINGLE-variant by-value product, so the decline never fired
for it and `emit_if_value` bridged the bare parameter carrier->concrete.
Widened to `emit_type_is_byvalue_adt` (any by-value product); the real gate is
the C-type-name equality that follows, so this removes bridges and can never
add one -- which is why the "keyed on `emit_type_is_byvalue_adt` these rules
broke 27 fixtures" warning on the SR1 *bridges* does not transfer to a decline.
The `g_sr1_sum_byvalue` seam gate stays on the multi-variant half only: a
single-variant product has ridden the by-value ABI since B3, with or without
SR1.  Pinned by `tests/fixtures/byvalue-struct-param-if-arm`, which carries
both broken shapes, the else-arm mirror (not in the original filing, and broken
the same way), the three shapes that already worked, and the tail-recursive
fold.  Suite: 2971 passed, 0 failed.

Found 2026-09-11 writing crdt-spice-plan C2's convergence
fuzzer, where the natural "apply this op, or don't" helper is exactly this
shape.

## Repro

```turmeric
(defstruct S [a : int])

(defn pick [x : S b : bool] : S
  (if b x (S 1)))          ;; `x` is a by-value struct PARAMETER

(defn main [] : int (println (.a (pick (S 7) true))) 0)
```

```
error: operand of type 'tur_adt_S' (aka 'struct tur_adt_S') where arithmetic
  or pointer type is required
 7739 |   __t267 = (*(tur_adt_S *)(intptr_t)(x));
```

The emitted code dereferences the by-value struct as though it were a handle.

## The trigger is the bare parameter, measured

| `if` arms | result |
| --- | --- |
| parameter vs constructor -- `(if b x (S 1))` | **BROKEN** |
| parameter vs call -- `(if b x (mk 1))` | **BROKEN** |
| call vs constructor -- `(if b (idS x) (S 1))` | ok |
| constructor vs constructor -- `(if b (S 7) (S 1))` | ok |

So it is the **bare parameter as an arm**, not the other side. Wrapping it in
any call -- even an identity `(defn idS [s : S] : S s)` -- compiles and runs.

## Not universal: a tail-recursive fold is fine

```turmeric
(defstruct H [m : int])
(defn step [acc : H n : int] : H
  (if (= n 0) acc (step (H (+ (.m acc) 1)) (- n 1))))   ;; prints 3, correct
```

Same shape -- bare struct parameter as an arm -- and it compiles. The
difference is that the other arm is a tail self-call, so something on the
tail-call path types the join differently. Worth knowing before assuming a fix
in the general `if` unifier covers it, and worth a second fixture either way.

## Why it matters

Any "transform this value, or pass it through unchanged" helper over a
by-value struct hits it, which is an extremely ordinary shape -- it is how a
fold's skip case is written. `spices/crdt`'s fuzzer routes both arms through
calls with a comment pointing here.

Two neighbours found in the same session suggest a family rather than an
isolated bug: an `if` over a Map HANDLE unified on the Map pointer and assigned
an int64 into it, and this one unifies a by-value struct as a pointer. Both are
the `if`-branch type unifier picking a representation one arm cannot satisfy.

## Fixtures owed

The repro above, plus the tail-recursive variant asserting it still works, so a
fix cannot regress the path that already does.
