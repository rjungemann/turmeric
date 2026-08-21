# `(:: <int> :float)` reinterprets the bit pattern instead of converting

**Severity: medium** -- silently wrong arithmetic, no diagnostic. Found
incidentally while writing an F4 fixture for jit-ffi-c2mir-plan.
**Status: RESOLVED for literals.** The expression case is split out to
docs/reported/ascribe-int-to-float-expression-ambiguity.md -- see "Scope"
below, which is the substantive part of this write-up.

## Repro

```turmeric
(defn main [] : int
  (println (:: 3 :float))   ; printed 1.4822e-323, expected 3
  0)
```

`1.4822e-323` is the double with bit pattern `0x0000000000000003` -- the
integer 3 reinterpreted, not converted.

## Root cause

src/compiler/elab_types.c, `elab_ascribe`. Between two scalar kinds of the
same width it built an `EX_REINTERPRET`:

```c
if (src_size > 0 && dst_size > 0 && src_size == dst_size) {
    Expr *r = expr_new(e->arena, EX_REINTERPRET, ...);
```

`:int` and `:float` are both 8 bytes, so every int-to-float ascription took
that arm.

**This was an accident, not a semantic**, and the proof is that it did not
hold across widths: on the same build `(:: 3 :float32)` printed **3**, because
8 != 4 misses the rule entirely. One operator disagreeing with itself purely
by target width is the tell. The neighbouring `(:: true :int32)` also
converts, so reinterpretation was never the operator's stated meaning.

## Resolution

An integer **literal** ascribed to a float kind now folds to the float
literal of that value. This mirrors the `EX_FLOAT_LIT` arm immediately above
it, which retypes a float literal ascribed to a float kind for the same
reason: a literal is the one shape where the intent is unambiguous.

Integer-typed ascription wrappers are peeled first, so a stepped
`(:: (:: 3 i32) f32)` reads the same as a direct one. Each hop is checked to
hold the value exactly. That guard cannot fire today -- integer-to-integer
`::` is erased and does not narrow, so `(:: 300 :int8)` prints 300, not 44 --
and it is there so the arm does not silently become wrong the day narrowing
lands.

## Scope -- why the expression case is NOT fixed here

`::` int-to-float is genuinely overloaded and **nothing in the static types
separates the two meanings**. Both of these have a statically `:int` operand:

- `(:: (.n m) :float)` where `n : int` -- a real integer, wants conversion.
- `(:: (list-head c) :float)` -- float bits in an int64 carrier, wants
  reinterpretation.

The reinterpret reading is load-bearing: typed slots, variadic rest
collection, and the cons/HAMT carriers all round-trip floats through an
`:int`-typed slot and read them back this way. Measured rather than assumed --
forcing conversion for every int-to-float ascription failed exactly 4
fixtures, all of them that carrier path:
`typed-slots/ascribe-reinterpret`, `typed-slots/fmap-float-list`,
`variadic-float-cons-collect`, and one line of
`ascribe-bool-to-numeric-prints`.

So the original report's second repro -- the `Mixed` struct-field case --
still returns 0.25 rather than 3.25. That half needs a decision about which
meaning keeps the `::` spelling, which is a design change rather than a fix;
it is filed with the three options in
docs/reported/ascribe-int-to-float-expression-ambiguity.md.

## A shipped fixture asserted the old behavior

`tests/fixtures/ascribe-bool-to-numeric-prints` carried:

```turmeric
;; `::` over an int operand stays a bit-level reinterpret, not a conversion
(println (:: 1 :float))        ; 4.94066e-324
```

That line is updated to `1`, and the fixture now also asserts
`(:: 1 :float32)` beside it -- which printed `1` all along, and is the
contradiction that shows the old assertion was pinning an accident rather
than a decision. The rewritten comment records this, so a maintainer who
disagrees has the reasoning in front of them rather than a silent flip.

## Tests

- `tests/fixtures/ascribe-int-literal-to-float` -- all three float widths, a
  stepped ascription, sign and zero, and two arithmetic probes. Per CLAUDE.md's
  float rule the arithmetic probes carry non-zero fractional parts
  (`3 + 0.25 = 3.25`, `4 * 0.5 = 2`) so a surviving reinterpret shows as a
  changed value; a denormal contributes nothing to a sum, and reading as a
  *dropped* term is exactly what made the original bug easy to miss.
- `tests/fixtures/ascribe-bool-to-numeric-prints` -- updated as above.

Suites: run.sh 2670 passed / 0 failed; run-turi.sh 1841 passed / 0 failed --
the tree-walker agrees with the compiled path on every line.
