# `(:: b :int)` on a bool prints `1`/`0` compiled and `true`/`false` interpreted

**Severity:** low. A both-paths divergence in a narrow spot: an explicit
ascription of a bool to `:int`. The compiled answer is the right one; the
interpreter ignores the ascription for printing purposes.

## Repro

```turmeric
(defn main [] : int
  (println (:: true :int))     ; compiled: 1     interpreted: true
  (println (:: false :int))    ; compiled: 0     interpreted: false
  0)
```

```
$ tur run t.tur
1
0
$ tur --interpret t.tur
true
false
```

## Why it matters

Not for the value -- nothing downstream of the ascription disagrees, only the
rendering. It matters because it silently splits a fixture across the two
paths: a fixture written with `(:: someBool :int)` cannot have one
`expected.stdout` that both harnesses accept, and the mismatch reads as a
product bug in whichever harness you look at second.

Printing the bool without the ascription agrees on both paths (`true`/`false`),
which is the workaround.

## Fix direction

Decide which is right and make the other match. The compiled path is the more
defensible reading -- an ascription to `:int` asked for an int -- so the
interpreter's `println` should consult the ascribed type rather than the
runtime tag of the value it holds. Worth checking whether the same is true for
other scalar ascriptions (`(:: 1 :bool)`, char/int) before fixing just this
pair.

## Found while

Executing
[fixture-dirs-with-loose-tur-files-pass-without-running](../archive/fixture-dirs-with-loose-tur-files-pass-without-running.md),
writing assertions for the revived `tests/fixtures/stm/` fixtures. A TVar's
value is an int64 boxed as `ptr<void>` that `println` will not take, so the
fixtures read values back with `(:: v :int)`; doing the same to the bool from
`tvar/cas` is what surfaced this.

---

## Execution -- RESOLVED 2026-08-06

Fixed in `src/turi/eval.c`, one hunk, at the rendering site. The report's own
recommendation -- *"the interpreter's `println` should consult the ascribed type
rather than the runtime tag of the value it holds"* -- turned out to be exactly
right, and the two wrong ways of doing it are worth recording because both look
more natural.

### It was wider than `:int`

The report's "worth checking whether the same is true for other scalar
ascriptions" was the right instinct. Sweeping every numeric target from a bool
operand: **all ten diverge** -- `int`, `int64`, `int8`, `int16`, `int32`,
`uint8`, `uint16`, `uint32`, `uint64`, `float`, `float32`. There was no
`TURI_BOOL` case anywhere in the interpreter's ascription re-tag, so a bool
operand kept its tag under every one of them.

The reverse direction (`(:: 1 :bool)`) already agreed, which is why the defect
looked narrower than it was: that direction *is* implemented.

`(:: <bool> :cstr)` is excluded on purpose. The compiled path emits
`puts(true)` and **segfaults**, so there is no behaviour to match; that is a
`::`-is-unchecked gap, not a printing divergence, and is not fixed here.

### Two natural-looking fixes that are wrong, and why

**1. Convert at the ascription.** This is the obvious reading of "the
interpreter should honour the ascribed type", and it is wrong: in the
tree-walker **a value's tag IS its type**, and the elaborator synthesizes an
int-carrier ascription for an ordinary `(vec-push! vb true)` into a
`(Vec bool)`. Re-tagging the bool to an int there loses the element type, and
later method dispatch picks the wrong instance --
`constrained-generic-instance-vec-element-unascribed` printed `1` where the
answer is `2` (Tag[int] selected over Tag[bool]), and
`van-laarhoven-lens-wide-functor-show` printed `nonneg` for `true`.

Narrowing it to the *written* type (`e->type.kind` rather than the tyvar-grounded
`ascribe_effective_kind`) does not save it: the offending node is a compiler
-synthesized reinterpret whose written type is a literal `:int`, at the span of
the `true` the user pushed. There is no flag distinguishing it from a
hand-written one.

**2. Mirror the existing int->float re-tag with float->int.** `(:: 7.1 :int)`
does diverge (the compiled path prints the reinterpreted bit pattern, the
interpreter prints `7.1`) and the two directions look symmetric. They are not:
in the interpreter an `:int` ascription over a `TURI_FLOAT` is the CARRIER
spelling in generic code -- "this word is the carrier" -- not a request to
expose the bits. Adding it fails **16** fixtures (ten `typed-slots` ones,
`poly-closure-result-tyvar-float`, `refine-type-arg-peeled`, ...). The
int -> `:float` direction is sound only because the carrier really does hold the
float's bits there. That divergence is therefore left as it is, with the
asymmetry now written down in the code next to the arm that tempts you.

### The fix

`println` is **overload-resolved by static type** -- `builtins.c` carries a row
per argument type (`BS_PRINTLN_INT`, `BS_PRINTLN_BOOL`, `BS_PRINTLN_FLOAT`,
...) -- so the elaborated AST already records which one `(:: b :int)` selected.
The interpreter was throwing that away: its handler switches on the runtime tag,
with the comment *"so eval mode works despite type-inference gaps."*

That default is kept. The one case where the chosen shape is strictly more
informative than the tag -- a `TURI_BOOL` value under an integer or float
println shape, which the elaborator could only have selected from a static
numeric type -- now prints the number. Printing is the single place the static
type can win without anything downstream depending on the tag, which is what
makes this the right site and the ascription the wrong one.

### Verification

Values were never wrong, only the rendering: `(let [n (:: true :int)] (+ n 41))`
gave `42` on both paths before and after. Pinned by
`tests/fixtures/ascribe-bool-to-numeric-prints/` -- all ten numeric targets, the
reverse direction, an unascribed bool, the int-operand bit-reinterpret that must
stay a reinterpret, and the arithmetic use.

Suites: `bash tests/run.sh` 2590 passed, 0 failed; `bash tests/run-turi.sh`
1777 passed, 0 failed, 705 skipped. No snapshot churn -- the compiled path is
untouched.
