# A by-value struct parameter used directly as an `if` arm is dereferenced

**Severity: medium.** A check/build divergence: `tur check` is clean and cc
rejects the emitted C. Not a wrong answer -- it never compiles -- but the
diagnostic names an internal temporary and points at C, not at the `if`.

**Status:** open. Found 2026-09-11 writing crdt-spice-plan C2's convergence
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
