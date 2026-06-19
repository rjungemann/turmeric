# M7 by-value HKT: a CAPTURING closure passed as a typed-fn element param drops its env

**Summary.** Under the by-value HKT path (default), an instance method whose
element mapper is a typed function param (`g : (fn [a] b)`) emits that param as a
bare `int64_t` and bare-calls it `((int64_t(*)(int64_t))(intptr_t)g)(x)` in the
by-value spec body. For a NON-capturing function (a top-level `defn` or a
non-capturing lambda) the int64 holds a raw function pointer and the call is
correct. For a **capturing** closure the int64 holds a fat-closure handle whose
`fn(env, x)` entry needs the env -- the bare raw-pointer call drops it and
**segfaults**. Severity: **hard crash / silent miscompile** for the common
`(fmap xs capturing-closure)` shape.

This is the last gate for migrating Functor/Monad/Applicative/Alternative
`[Result]` (and the rest of the by-value HKT stdlib) to pure-Turmeric bodies:
once a class signature is typed (required for by-value element recovery), every
instance body calls the element fn as `(g x)`, and that call must preserve the
env of a capturing closure.

## Minimal repro

```turmeric
;; bimap shape -- a FULLY-APPLIED ctor head, so NOT the partial-app blocker
;; (that one is resolved); this is the orthogonal capturing-closure gate.
(defclass MyBifunctor [^^p]
  (bimap2 [g : (fn [a] c) h : (fn [b] d) x : (p a b)] : (p c d)))
(definstance MyBifunctor [Result]
  (bimap2 [g h x]
    (if (.is-ok x) (ok (g (.ok-val x))) (err (h (.err-val x))))))

(defn main [] : int
  (let [delta 100
        r (bimap2 (fn [n : int] : int (+ n delta))   ;; CAPTURES delta
                  (fn [n : int] : int n)
                  (:: (ok 21) (Result int int)))]
    (ok-val (:: r (Result int int)))))   ;; TARGET 121
```

```
$ ./build/tur build /tmp/repro.tur -o /tmp/repro && /tmp/repro
Segmentation fault          # the capturing (fn [n] (+ n delta)) called without its env
```

Replacing the capturing lambda with a top-level `defn` (no capture) returns 121
correctly -- confirming the gate is specifically capturing closures.

## Root cause

The element fn param `g : (fn [a] b)` is lowered to the int64 carrier in the
instance-method signature, and the body's `(g x)` emits a raw function-pointer
call rather than a fat-closure dispatch (`TUR_APPLY*`). A fat closure's handle is
a pointer to a `{env, fn}` box; reinterpreting it as a bare `int64_t(*)(int64_t)`
and calling it jumps to the box address, not the thunk.

The non-capturing case works "by luck" because a non-capturing lambda / `defn`
lowers to a bare function pointer that the raw call invokes correctly.

## Proposed fix directions

1. **Mark the typed-fn element param as fat (`^fat`) in the by-value spec** so the
   body call dispatches through the fat protocol (`TUR_APPLY`), and auto-shim a
   bare function pointer into a fat box at the call site (the same plumbing the
   `^fat` bare-function arrow layer already uses). This is the natural home: the
   element param is exactly a `^fat` sink.
2. Alternatively, lower `(fn [a] b)` element params to the `tur_poly_fn_t`
   `{env, fn}` carrier and call `g.fn(g.env, x)`, but that changes the element ABI
   everywhere and is more invasive.

## How to validate a fix

- The repro above exits 121 (capturing closure preserved).
- The partial-app probe with a capturing closure
  (`pmap (:: (ok 21) (Result int int)) (fn [x] (+ x delta))`) exits 121.
- `bash tests/run.sh` stays green flag-on AND flag-off; the eight by-value shape
  probes stay green; existing HKT fixtures emit clean.
