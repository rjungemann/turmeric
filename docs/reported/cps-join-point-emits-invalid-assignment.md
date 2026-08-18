# CPS join point emits `0 = <value>;` and jumps to an undeclared label

**Severity:** high (emits C that cannot compile; no `.tur`-level diagnostic,
so it reads as a mysterious `cc invocation failed`)
**Found:** 2026-08-18, writing `spices/secret`'s test suite against
`tur` v0.35.0 (`2748f5e8a`).

## Summary

When a `let` binding's value is a **higher-order call** and the `let` body is
a `do` that ends in a value, the CPS transform lifts the continuation into a
join-point function -- and that join point assigns its result to the integer
literal `0` and `goto`s a label that was never emitted:

```c
typedef struct { int64_t f0; } b4__all_j0_env;
static intptr_t b4__all_j0(intptr_t env, intptr_t nz_1347__slot, DK *__kont) {
    b4__all_j0_env *__cap = (b4__all_j0_env *)(intptr_t)env;
    int64_t k_1340 = __cap->f0;
    int64_t nz_1347 = (int64_t)(nz_1347__slot);
    int64_t __t6;
    bool __t5;
    b4__release(k_1340);
    __t6 = 0;
    __t5 = (nz_1347) > (INT64_C(0));
    0 = __t5;        /* <-- not an lvalue */
    goto L4;         /* <-- L4 is never defined */
}
```

```
error: expression is not assignable
error: use of undeclared label 'L4'
```

Both errors are the same defect: the join point was built without a
destination slot or a return label, so the emitter fell back to the literal
`0` for the slot and to a label id that only exists in the parent frame.

## Minimal repro

```turmeric
(defmodule b4

(defn mk [n : int] : (Result int cstr)
  (if (< n 1) (err "bad") (ok n)))

(defn release [x : int] : void
  ```c
  (void)x;
  ```)

(defn hof [x : int f : (fn [int] int)] : int (f x))
(defn cb  [x : int] : int x)
(defn check [desc : cstr result : bool] : bool result)

(defn all [] : void
  (do
    (check "a"
      (let [r (mk 64)]
        (if (ok? r)
          (let [k  (ok-val r)
                nz (hof k cb)]        ;; higher-order call in a let binding
            (do
              (release k)             ;; void call
              (> nz 0)))              ;; ...then a value
          false)))
    (check "b" true)))

(defn main [] : int (do (all) 0)))
```

```
$ tur run b4.tur
.../b4_tur.c:7284:7: error: expression is not assignable
.../b4_tur.c:7285:10: error: use of undeclared label 'L4'
2 errors generated.
```

## Shape that triggers it

All of these seem to be needed:

- an enclosing `defn` returning `:void` whose body is a `do` of several calls;
- inside it, a call taking the result of a `let`/`if` as an argument;
- a `let` binding whose value is a **higher-order** call (this is what forces
  the CPS transform and the join point);
- a `let` body that is a `do` starting with a void call and ending in a value.

Dropping the higher-order call, or hoisting the inner `let` into its own
top-level `defn`, both make it go away.

## Workaround in use

`spices/secret/tests/core_test.tur` gives every test case its own top-level
`defn` returning `:bool`, and the `describe`/`it` block only calls them:

```turmeric
(defn t-random-entropy [] : bool
  (let [r (secret-random! 64)] ...))

(defn __all-tests [] : void
  (describe "secret-random!"
    (it "fills the payload with entropy" (t-random-entropy))))
```

That is the layout the linalg and sdf-raylib suites already use, so it is not
a burden -- but it is currently load-bearing rather than stylistic, and
nothing warns you.

## Related

Same session, same CPS emitter, distinct defect:
[`cps-result-unbox-dropped.md`](../archive/cps-result-unbox-dropped.md).
