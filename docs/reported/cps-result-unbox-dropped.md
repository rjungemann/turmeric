# CPS-transformed functions drop the box->struct unbox for an inline-C `Result`

**Severity:** high (silent-until-`cc` miscompile; blocks the natural inline-C
constructor idiom the guide documents, and the failure mode is a wall of C
type errors with no `.tur` line attribution)
**Found:** 2026-08-18, building `spices/secret` (`secret/core`) against
`tur` v0.35.0 (`2748f5e8a`).

## Summary

An inline-C body that returns `(Result A B)` yields the **boxed** carrier
(`tur_result_box_t *` widened to `int64_t`). The emitted caller normally
converts that to the by-value struct:

```c
tur_result_box_t *__t192 = (tur_result_box_t *)(intptr_t)(__ps_191);
tur_adt_Result__Secret__cstr r_1426 = (tur_adt_Result__Secret__cstr){
    .is_ok = __t192->is_ok, .ok_val = (int64_t)(__t192->ok_val), ... };
```

In a function the **CPS transform** touches, that conversion is dropped and
the `int64_t` is assigned straight to the struct:

```c
static int64_t ..._t_hyrandom_hyentropy__cps(DK *__kont) {
    tur_adt_Result__Secret__cstr r_1442;
    ...
    int64_t __ps_202 = (secret__core__secret_hyrandom_ex(INT64_C(64)));
    r_1442 = __ps_202;              /* <-- no unbox */
```

`cc` then rejects it:

```
error: assigning to 'tur_adt_Result__Secret__cstr'
       from incompatible type 'int64_t'
```

The trigger for the CPS transform in practice is calling a **higher-order
function**, which is why this surfaced immediately: `secret/core`'s
`with-secret` takes a `(fn [ptr<void> int] int)`, so every test that both
unwrapped a constructor's `Result` and read the secret hit it.

## Minimal repro

```turmeric
(defmodule repro

(defopaque H :int)

;; inline-C constructor -> boxed Result carrier
(defn mk [n : int] : (Result H cstr)
  ```c
  if (n <= 0) return tur_err_ptr((void *)"bad");
  return tur_ok_ptr((void *)(intptr_t)n);
  ```)

(defn hof [h : H f : (fn [int] int)] : int (f 7))
(defn cb  [x : int] : int x)

;; OK: binds the Result, no higher-order call.
(defn no-hof [] : int
  (let [r (mk 3)]
    (if (ok? r) 1 0)))

;; BROKEN: binds the Result AND makes a higher-order call.
(defn with-hof [] : int
  (let [r (mk 3)]
    (if (ok? r)
      (hof (ok-val r) cb)
      0)))

(defn main [] : int (do (no-hof) (with-hof) 0)))
```

```
$ tur run repro.tur
.../repro_tur.c:7269:12: error: assigning to 'tur_adt_Result__H__cstr'
    (aka 'struct tur_adt_Result__H__cstr') from incompatible type 'int64_t'
1 error generated.
```

Exactly one error, in `with-hof`; `no-hof` compiles clean. Removing the
`hof` call from `with-hof` makes it compile.

## CPS is contagious, so splitting does not help

The obvious workaround -- move the higher-order call into its own defn and
call that instead -- does **not** work; the caller is CPS-transformed too:

```turmeric
(defn hof-wrapper [h : H] : int (hof h cb))

(defn with-hof-split [] : int
  (let [r (mk 3)]
    (if (ok? r) (hof-wrapper (ok-val r)) 0)))   ;; still fails
```

## Root cause direction

The boxed-carrier -> struct conversion is applied where the ordinary
emitter materializes the call result, but the CPS/trampoline emitter has its
own path for binding a call result into a pre-declared slot and does not
consult the same "does this need unboxing" predicate. Note the struct local
is hoisted to the top of the `__cps` function (`tur_adt_Result__Secret__cstr
r_1442;`) and assigned later -- so the fix likely belongs at that assignment
site, not at the declaration.

## Workaround in use

`spices/secret/src/secret/core.tur` keeps every inline-C body to raw pointer
work and constructs all `Result` values in Turmeric with `ok`/`err`:

```turmeric
(defn secret-random! [n : int] #fx{Rand} : (Result Secret cstr)
  (if (< n 1)
    (err "secret-random!: length must be positive")
    (let [h (secret-alloc-raw n)]
      (if (= h 0)
        (err "secret-random!: allocation failed")
        (if (secret-fill-random-raw! h n)
          (ok (secret-of-raw h))
          (do (secret-free-raw! h)
              (err "secret-random!: OS CSPRNG failed")))))))
```

This is arguably the better factoring anyway, but it should be a choice, not
a requirement -- `docs/guides/inline-c-results-guide.md` actively recommends
the pattern that breaks, so the guide is currently steering people into this.
