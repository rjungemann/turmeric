# A `float32` value closing a multi-expression body gets a `double` temp

**Severity: low.** A precision warning, not a wrong answer at the values seen so
far -- the emitted C narrows back on return, and the literals involved are
exactly representable. It is visible only under GCC (whose `-Wfloat-conversion`
covers double -> float; clang splits that out as
`-Wimplicit-float-conversion`), which is how it stayed quiet.

**Status: RESOLVED 2026-09-13.** Fixed at ELABORATION rather than in the
emitter, which is one layer above where this report pointed: the emitter types
the block temp from `last->type`, and `last->type` was the literal's own default
(`TY_FLOAT`).  `rc_narrow_float_literal_tail_to_return`
(`elab_core.c`) retypes a float literal in TAIL position from the declared
`: float32` result and carries the new type out through the `EX_DO` / `EX_LET` /
`EX_LETREC` wrappers that report it.  It runs beside
`rc_widen_int_literal_to_float_return`, its int-literal counterpart, at both the
`defn` and the typeclass-method site.

`EX_IF` is deliberately not walked.  Its arms are already unified with each
other, so retyping the join from one literal arm would leave the other arm's
type behind -- the very "a temp or join taking a type one side cannot satisfy"
shape this report groups itself with.  Only `TY_FLOAT32` is narrowed: `float`
and `float64` are both `double` in C, so there is nothing to fix there.

Result: `float __t; __t = ((float)2.5); return __t;`, one rounding where the
source says one.

**The ratchet exclusion is now EMPTY, not merely narrowed** -- which is what
this report asked for ("it currently also hides any genuine double -> float
confusion").  The knob is kept as a named one-liner in `tests/run.sh` so
re-widening is one edit with its history attached; note that it keyed on the
DESTINATION, so it also hid int-carrier-into-float-slot.  Swept clean across the
corpus before the exclusion came off -- 2974 passed, 0 failed -- which is the
procedure this ratchet was established with.  `tests/check-cc-warn-ratchet.sh`'s
comment about the exclusion was updated with it; its canaries convert to an
INTEGER and are unaffected by the knob either way, which is the point of them.

Pinned by `tests/fixtures/float32-block-tail-literal`: the `do` and `let` tails,
a nested `do`+`let`, a non-exactly-representable literal (`0.1` -- the value
that would round twice), the single-expression control, and a `: float` sibling
that must KEEP its `double` temp.  The value assertions pass either way, as this
report notes; the warning is what pins it, and with the exclusion empty the
warning now fails the suite.

Found 2026-09-12 when the `-Wfloat-conversion` ratchet landed
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
