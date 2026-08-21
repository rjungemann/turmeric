---
title: Float division by zero aborts instead of producing IEEE inf/NaN
category: Report
description: BS_DIV_CHECK applies the integer division guard to float division too, so 7.1 / 0.0 aborts the process rather than evaluating to inf. Also emits a branch per division and a -Wliteral-conversion warning on constant divisors.
---

# Float division by zero aborts instead of producing IEEE inf/NaN

**Severity:** medium. Correct for `:int`; wrong for `:float`, where division
by zero is well-defined in IEEE 754. Any numeric code that relies on `inf` /
`NaN` propagation (limits, normalization guards, `1/0 -> inf` sentinels) gets
a process abort instead of a value. Also a small per-division cost in every
float-heavy loop.

**Found:** 2026-08-20, while writing the ECS integrate benchmark
(`turmeric-spices/spices/ecs/bench/`) -- the generated C for the setup loop
drew `-Wliteral-conversion` warnings, which led here.

**Compiler:** `./build/tur` v0.37.0 (Debug).

## Repro

```turmeric
(defn fdiv [a : float b : float] : float (/ a b))
(defn main [] : int
  (println (fdiv 7.1 0.0))
  0)
```

```
$ tur run fdiv.tur
division by zero
```

Expected under IEEE 754: `inf`. (And `0.0 / 0.0` should be `NaN`, not an
abort.) The int analogue -- `(idiv 7 0)` -- correctly aborts, since integer
division by zero is UB in C and there is no value to produce.

## Root cause

`src/compiler/emit_cps_ir.c:5359` (`BS_DIV_CHECK`) emits one guard shape for
every division, without consulting the operand type:

```c
case BS_DIV_CHECK:
    buf_printf(&b,
        "((%s) ? ((%s) / (%s)) : "
        "(fprintf(stderr, \"division by zero\\n\"), abort(), 0))",
        as[1], as[0], as[1]);
    break;
```

So a `double` divisor is used as the condition of a ternary. Three
consequences, in order of severity:

1. **Wrong semantics for floats.** `b != 0.0` is a fine test, but the branch
   it guards should not exist for floating point -- the hardware already
   produces the right answer.
2. **A branch per division.** In a float-heavy inner loop this is a real
   (if small) cost the C the user would have written does not pay.
3. **`-Wliteral-conversion` noise on constant divisors.** `(/ f 8.0)` emits
   `((8.0) ? ((f) / (8.0)) : ...)`, and clang reports
   `implicit conversion from 'double' to 'bool' changes value from 8 to true`.
   Real turmeric programs doing constant float division emit warnings from
   generated code the user cannot fix.

## Fix directions

Gate the guard on the operand type at the `BS_DIV_CHECK` site: keep it for the
integer kinds, and emit a bare `(a) / (b)` for `:float` / `:float32`. That
fixes all three symptoms at once and is a strictly smaller emission.

If aborting on float division by zero is instead *intended* (a deliberate
"no silent inf" policy), then two things should follow: it needs to be
documented as a language decision rather than left looking like the integer
guard leaking, and the constant-divisor case should be folded at compile time
so ordinary code stops emitting warnings.

Worth deciding which, because the two readings point opposite ways.

## Adjacent

`docs/reported/user-defn-named-div-collides-with-libc.md` was found in the
same session and by the same repro file -- the first attempt named the
function `div`, which does not compile at all.
