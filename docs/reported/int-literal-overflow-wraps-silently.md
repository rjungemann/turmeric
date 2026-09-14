# An integer literal past the int64 range wraps silently, and `-9223372036854775808` is UB

**Severity: medium** -- the headline is a **silent wrong answer**: an
out-of-range decimal literal is accepted with no diagnostic and compiles to a
wrapped value. The two UBSan findings below are the same code and come along
for free.

**Status:** open. Found 2026-09-14 writing `spices/msgpack`'s integer-format
tests, which assert the int64 boundary values on both the encode and decode
sides and so put every one of these literals in a source file.

## Repro

```turmeric
(defn main [] : int
  (println 9223372036854775808)     ;; INT64_MAX + 1
  (println 99999999999999999999)    ;; far past the range
  (println -9223372036854775808)    ;; INT64_MIN -- legal, and the UB case
  0)
```

```
$ tur check p.tur
$ echo $?
0                      # no diagnostic of any kind for either overflow

$ tur run p.tur
-9223372036854775808   # 9223372036854775808 wrapped
7766279631452241919    # 99999999999999999999 wrapped
-9223372036854775808   # correct, but see below
```

Two of the three printed values are wrong, and nothing said so.

### The same code, under the Debug build's UBSan

`-9223372036854775808` **alone** -- a perfectly legal literal that produces the
right answer -- trips both sanitizer sites:

```
$ tur check min.tur
src/compiler/reader.c:320:30: runtime error: signed integer overflow:
    9223372036854775800 + 8 cannot be represented in type 'int64_t'
src/compiler/reader.c:407:30: runtime error: negation of -9223372036854775808
    cannot be represented in type 'int64_t'; cast to an unsigned type to
    negate this value to itself
```

`9223372036854775807` alone is clean, which confirms the mechanism: the
magnitude, not the value, is what overflows.

### A third symptom, in the emitted C

`INT64_MIN` is emitted as `INT64_C(-9223372036854775808)`, which expands to
`-9223372036854775808LL` -- the literal `9223372036854775808` parsed first (too
large for a signed type) and then negated. clang warns:

```
warning: integer literal is too large to be represented in a signed integer
    type, interpreting as unsigned [-Wimplicitly-unsigned-literal]
```

Not one of the three warnings AppleClang 21 turns into a hard error on the
macOS CI leg, so it is noise today rather than a broken build -- but it is
noise on every program that mentions `INT64_MIN`.

## Root cause

The sign is lexed separately and applied *after* the magnitude is accumulated,
so `INT64_MIN`'s magnitude is one past `INT64_MAX` for the whole accumulation:

- `src/compiler/reader.c:320` -- `ival = ival * 10 + (int64_t)digit;`
  accumulates into a signed `int64_t` with no range check. This both overflows
  (UB) and is where an out-of-range literal silently becomes a wrapped value.
- `src/compiler/reader.c:407` -- `if (sign < 0) ival = -ival;` negates, which
  is UB for `INT64_MIN` specifically.

The fixed-width suffix paths right below `:407` already do exactly the right
thing (`lit_suf == LIT_SUF_I8 && (ival < -128 || ival > 127)` emits
"integer literal overflows int8 range"); the default int64 path is the one with
no check.

## Fix directions

1. Accumulate into `uint64_t`. After each digit, compare against the bound for
   the sign in hand -- `(uint64_t)INT64_MAX` when `sign >= 0`, and
   `(uint64_t)INT64_MAX + 1` when `sign < 0` -- and on exceeding it emit a
   diagnostic in the shape the suffix paths already use
   ("integer literal overflows int64 range (-9223372036854775808..9223372036854775807)").
   That kills the silent wrap and the `:320` UB together.
2. Convert without negating: `ival = (sign < 0) ? (int64_t)(~mag + 1) : (int64_t)mag;`
   which is well-defined for the `INT64_MIN` magnitude. That kills the `:407`
   UB.
3. Emit `INT64_MIN` as `(-9223372036854775807LL - 1)` (or `INT64_MIN`) rather
   than `INT64_C(-9223372036854775808)`, which removes the
   `-Wimplicitly-unsigned-literal` warning from every program that uses it.
4. Fixtures: a positive `errors/` fixture per overflow direction asserting the
   new diagnostic, and a happy-path fixture that round-trips `INT64_MIN` and
   `INT64_MAX` through `println`. The happy-path one is what keeps a fix for
   (1) from over-rejecting `INT64_MIN`.

## Workaround in use

None, and none is needed -- `spices/msgpack` writes `-9223372036854775808` and
`9223372036854775807` literally and gets correct answers today. The cost is
that every Debug-build run of that spice's suite prints the two UBSan lines
above, which is exactly the noise that trains a reader to skim sanitizer output.

**When this report is resolved**, nothing has to be removed from the spice:
`spices/msgpack/tests/decode-primitives.tur` already carries the `int64 min`,
`int64 max` and "a uint64 above int64 max is an error" cases as the in-tree
witnesses, and they should keep passing unchanged. The UBSan lines disappearing
from that suite's output is the visible confirmation.
