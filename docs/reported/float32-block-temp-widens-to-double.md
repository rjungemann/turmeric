# A `float32` value closing a multi-expression body gets a `double` temp

**Severity: low.** A precision warning, not a wrong answer at the values seen so
far -- the emitted C narrows back on return, and the literals involved are
exactly representable. It is visible only under GCC (whose `-Wfloat-conversion`
covers double -> float; clang splits that out as
`-Wimplicit-float-conversion`), which is how it stayed quiet.

**Status:** open. Found 2026-09-12 when the `-Wfloat-conversion` ratchet landed
and the Linux leg failed `forall-dict-float-result` while macOS passed.

## Repro

```turmeric
(defn f [] : float32 (do (println 1) 2.5))
(defn main [] : int (println (f)) 0)
```

```
warning: implicit conversion loses floating-point precision: 'double' to 'float'
    return __t278;
```

```c
static float f(void) {
        ...
        double __t278;      /* should be float */
        __t278 = 2.5;
        return __t278;      /* double -> float */
}
```

A **single**-expression body is fine:

```turmeric
(defn f [] : float32 2.5)      ;; no warning; emitted as float throughout
```

So the block/sequence temp does not take the enclosing function's declared
result type -- it defaults the float literal to `double` and lets the `return`
narrow.

## Why it is worth fixing rather than suppressing

The value is right today because the literals are exact. A `: float32` body
ending in a literal that is *not* exactly representable would round twice --
once into the `double` temp, once on the narrowing return -- which is a
different answer from rounding once. Nothing in the tree does that yet.

It is also the third emitter issue this session in the same area: an `if` over a
Map handle unified on the Map pointer, a by-value struct parameter as an `if` arm
was dereferenced, and this. All three are a temp or join taking a type one side
cannot satisfy.

## Where to look

`fresh_tmp` (`emit_core.c`) only mints the name; each use site picks the type.
The one here is whatever emits a multi-expression body's trailing value. The fix
is to type that temp from the function's declared result rather than from the
value's own default.

## Interaction with the ratchet

`tests/run.sh`'s emitted-C warning ratchet EXCLUDES float-conversion warnings
whose destination is a floating type, precisely so this does not fail the suite
on GCC. **Narrow that exclusion when this lands** -- it currently also hides any
genuine double -> float confusion.

## Fixture owed

The two-line repro above, asserting the value. Note it needs a build under GCC
or clang with `-Wimplicit-float-conversion` to observe the warning; the value
assertion alone passes either way today.
