# turi: catch-unwind over an aggregate-returning thunk yields the handle, not the value

**Severity: medium** (silent wrong answer under `--interpret`). The
interpreter's counterpart to
[catch-unwind-aggregate-return-miscompiled](../archive/catch-unwind-aggregate-return-miscompiled.md),
found 2026-08-21 while fixing that one: the compiled path is correct now, the
interpreted path is not, so the two disagree on the same program.

## Repro

```turmeric
(defstruct Q [x : int y : int])
(defn f [] : (Result Q int) (catch-unwind (fn [] : Q (make-struct Q 3 4))))
(defn main [] : int
  (let [r (f)]
    (if (ok? r) (println (.x (ok-val r))) (println -1)))
  0)
(main)
```

```
$ tur run repro.tur          # compiled
3
$ tur --interpret repro.tur  # interpreted
91328184803632
```

The number is the struct's runtime handle: `.x` read the pointer rather than
the field. As on the compiled path, it is the SUCCESS path that is wrong and no
panic need be involved -- and the same program with `(ok (make-struct Q 3 4))`
in place of the catch prints `3` under both engines, so `(Result Q int)` and
`.x` are fine on their own.

## Root cause

Not investigated past the boundary above. The compiled defect was that the
boundary called the thunk through an `int64_t`-returning function-pointer cast
while the thunk returned the aggregate by value; the interpreter has no such
cast, so the mechanism is a different one -- most likely the catch boundary
boxing the thunk's TuriValue result as an opaque handle rather than keeping the
struct value (`native_catch_unwind` / the result-box construction in
`src/turi/eval.c`).

## Blast radius

Two fixtures carry a `requires.compiled` marker naming this report; removing
the markers is how to re-check it.

- `tests/fixtures/catch-unwind-aggregate-thunk/` -- both catch forms, a
  `defstruct` and a `defdata` payload, a capturing-closure thunk and the panic
  path. A ready-made probe.
- `tests/fixtures/schema-reader-json-str-result/` -- the `#json-str?<T>`
  reader, whose expansion is a catch-unwind around a typed decode, so every
  successful decode lands on this. Its ERR path runs correctly under turi (the
  interpreter's `schema-decode-abort` native raises a catchable panic now, in
  step with `stdlib/schema.tur`); only the OK path is wrong.

So this defect is what stands between the interpreter and a working
`#json-str?<T>`, not just an obscure catch-unwind corner.

## Guides to update when fixed

- none (no guide documents a divergence here)
