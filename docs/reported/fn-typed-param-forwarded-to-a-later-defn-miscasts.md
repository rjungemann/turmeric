# Forwarding a function-typed parameter to a later-defined defn emits a broken cast

**Severity: medium-high** -- a hard **build failure** in `cc`, not a wrong
answer, so nothing ships broken. What makes it expensive is that the trigger is
**source order**: the same two functions compile or fail depending on which one
is written first, and the error names a C type (`tur_poly_fn_t`) that does not
appear anywhere in the program.

**Status:** open. Found 2026-09-12 building crdt-spice-plan C3's `ORMap`, whose
`ormap-merge-with` takes the value-merge as a parameter and forwards it to its
fold helpers.

## Repro

```turmeric
(defstruct S [v : int])
(defn add [a : int b : int] : int (+ a b))

(defn caller [g : (fn [int int] int) ^borrow a : S ^borrow b : S] : int
  (let [x (.v a) y (.v b)
        r (helper g x y 0 true)]         ;; forwards g to a LATER defn
    r))

(defn helper [g : (fn [int int] int) x : int y : int acc : int first : bool] : int
  (if first (helper g x y (g x y) false) acc))

(defn main [] : int (println (caller add (S 3) (S 4))) 0)
```

```
error: operand of type 'tur_poly_fn_t' where arithmetic or pointer type is required
    (helper((void *)(intptr_t)(g), x_1616, y_1617, INT64_C(0), true));
```

**Move `helper` above `caller` and the identical program prints `7`.** That is
the whole difference.

## Mechanism

Both orderings emit the same forward declaration:

```c
static int64_t helper(tur_poly_fn_t, int64_t, int64_t, int64_t, bool);
```

The call sites differ:

```c
/* helper defined AFTER caller  -- broken */
helper((void *)(intptr_t)(g), x_1616, y_1617, INT64_C(0), true)
/* helper defined BEFORE caller -- fine   */
helper(g, x_1621, y_1622, INT64_C(0), true)
```

So the emitter applies its erasing carrier cast (`(void *)(intptr_t)`) when it
cannot see that the callee's parameter is a real `tur_poly_fn_t`. The signature
it consults is evidently populated as definitions are *emitted*, so a callee
that is only forward-declared at that point falls back to the opaque-pointer
convention -- and `tur_poly_fn_t` is a struct, which no cast to `void *` is
valid on.

The forward declaration already carries the correct parameter type, so the
information is present; the call-site emitter is reading the wrong table.

## Fix direction

Resolve the callee's parameter types from the same declaration pass that emits
the forward declarations, rather than from definitions emitted so far. A
narrower fix: suppress the erasing cast when the target parameter type is
`tur_poly_fn_t`, which is never a carrier.

## Workaround

Define the helpers **before** the function that forwards into them. That is
what `crdt/ormap` does, with a comment pointing here so it is not "tidied" back
into caller-first order.

## Not this

The bug is not about `^borrow`, the struct parameters, the `let`, or recursion
in the helper -- each was added to the repro separately and none triggers it
alone. Forwarding a function-typed parameter to an **earlier**-defined function
is correct in every combination tried.

## Fixtures owed

- The repro above in both orders, asserting `7` twice, so a fix cannot be
  mistaken for the ordering workaround.
