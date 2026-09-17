# `let`-bound erasing ascription `(:: w (Vec int))` emits an uncast int->pointer init

**RESOLVED 2026-09-17.** Root cause as filed, at three sites rather than one.
The binder-init arm that bridges an int64 init into a pointer binder keyed on
the ascription's OUTER type (`init_cn`, a pointer -- "nothing to bridge") and
on the local-var side table (a parameter is never recorded there), and both
said no.  The question that was missing is the INNERMOST value under the
ascription chain: when the binder c-names to a pointer and that value's
resolved type c-names to the int64 carrier, the init is a word and wants
`(T)(intptr_t)(word)`.  That is now `emit_let_init_is_erased_word_to_ptr`
(`src/compiler/emit_expr.c`), asked by `emit_let_value`, `emit_letrec_value`
AND `emit_tail`'s inline tail-position `let` arm -- the same three sites as
`emit_let_init_carrier_bridge_type`, for the same reason: the self-tail-
recursive shape emits its `let` straight into the back-edge loop, and the
report's repro with the recursion in tail position hit the third copy.  The
cast is value-preserving in both directions, so a shape that was right before
stays right.  Pinned by `tests/fixtures/let-bound-erasing-ascription-int-to-
pointer` (both the plain and the tail-recursive shape); the full suite was
green with no snapshot drift.

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
