# `let`-bound erasing ascription `(:: w (Vec int))` emits an uncast int->pointer init

**Severity:** medium -- a hard `-Wint-conversion` error on macOS clang
(AppleClang 21), only a warning on Linux gcc, so it ships green on Linux CI.

## Repro

```turmeric
(defn first-word [words : int] : int
  (let [v (:: words (Vec int))]
    (vec-get v 0)))

(defn main [] : int
  (let [v (vec-new)]
    (vec-push! v 7)
    (println (first-word (:: v :int)))
    0))
```

`tur build` on macOS:

```
error: incompatible integer to pointer conversion initializing
'tur_adt_Vec__int *' with an expression of type 'int64_t' [-Wint-conversion]
```

Emitted C (direct emitter):

```c
static int64_t first_hyword(int64_t words) {
        ...
            tur_adt_Vec__int * v_1608 = words;
```

The same ascription in call-argument position -- `(vec-get (:: words (Vec int)) 0)`
-- is cast correctly.

## Root cause

The direct emitter's `let` binder init for an erasing ascription from the int64
carrier to a concrete pointer type does not bridge through `intptr_t`.  Sibling
of the CPS inline-join delivery straddle fixed alongside this report in
`src/compiler/emit_cps_ir.c` (`cty_word_straddle`), which is what broke
turmeric-spices `plot (macos-latest)`.

## Fix direction

At the `let` binder init, when the binder C type is a pointer and the value's C
type is `int64_t`/`intptr_t` (or the reverse), emit `(T)(intptr_t)(value)`.
Add the repro above as a fixture.
