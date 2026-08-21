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

## Resolution (2026-08-21)

One line, in the place the interpreter already documents the trap.
`turi_ok_result_box` (`src/turi/eval.c`) took a bare `int64_t` and always built
the 3-int box -- the exact flattening `native_ok`'s own comment describes and
avoids ("loses the tag of a *heap* payload ... and a downstream field access /
println reads garbage"). The catch-unwind boundary was the one caller that
still went through it.

It takes a `TuriValue` now and applies `native_ok`'s rule: a
STRUCT / CSTR / CLOSURE / FLOAT payload becomes a make-struct `Result` whose
fields hold full TuriValues; int and bool payloads keep the int64 box the
carrier-ABI fixtures depend on. `result_field` already read both shapes, so
every accessor stayed uniform and nothing else changed.

The blast radius was wider than the report's struct repro: a `cstr` payload
came back as a pointer printed as an int, and a `float` payload was
tag-flattened too. Both are correct now.

Both fixtures dropped their `requires.compiled` markers and pass under
`run-turi.sh`: `catch-unwind-aggregate-thunk` (all seven of its cases) and
`schema-reader-json-str-result`, so `#json-str?<T>` works on both engines.

Not fixed, and noted here because it is one tag away: a `bool` payload still
prints `1` under `--interpret` where the compiled path prints `true`. Adding
TURI_BOOL to the heap set would change the box shape for every bool Result, so
it belongs with the display-divergence family
(`ascribe-bool-to-int-prints-differently-per-path`), not here.
