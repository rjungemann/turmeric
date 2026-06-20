# `letrec` self-recursive float accumulator collapses to the int carrier

**Status:** RESOLVED. Filed from turmeric-spices Track C (linalg) where
`la-vec-dot` / `la-vec-norm` returned `0` for any input. Fixed here in
turmeric main; one-line fix plus a regression fixture.

- Fix: `src/compiler/elab_forms.c`, `elab_letrec` Pass A scalar return-type peek.
- Regression fixture:
  `tests/fixtures/letrec-self-recursive-carrier-float-return/`.
- `bash tests/run.sh`: 1728 passed, 0 failed (with `../turmeric-spices` cloned).

This is the inline-scalar follow-on to the W1 fix
(`docs/archive/letrec-self-recursive-carrier-and-vec-new-fresh-arg-unify.md`),
which covered struct / `(Vec T)` / ADT carrier returns but not `float`.

---

## Symptom (in the spice)

`linalg`'s `la-vec-dot` and `la-vec-norm` accumulated a running float sum in a
`letrec`-bound self-recursive closure and returned `0` for every input. The
spice author traced it to "the `^mut acc` accumulator is captured by value into
the closure, so `set!` doesn't escape," tried threading the accumulator as a
return value instead, found that the recursive self-call fails to type-check
(`then=float else=int`), added a `(:: (go ...) :float)` ascription to force it
through -- and then it **still** returned `0` at runtime. The shipped
workaround accumulates into a 1-element heap `(Vec float)` cell whose
shared-pointer writes escape the closure.

## Minimal repro

```turmeric
(defn sum-floats [n : int] : float
  (letrec [go (fn [i : int  acc : float] : float
                (if (>= i n) acc
                  (go (+ i 1) (+ acc 1.5))))]
    (go 0 0.0)))
;; before: error: if branches have mismatched types: then=float else=int
```

Forcing it past the type checker exposes the second half of the bug:

```turmeric
(if (>= i n) acc
  (:: (go (+ i 1) (+ acc 1.5)) :float))   ;; type-checks ...
;; ... but sum-floats 4 returns 0.00 at runtime instead of 6.00
```

The identical shape with an `int` (or, post-W1, a struct / `(Vec T)`)
accumulator works.

## Root cause

`elab_letrec` (`src/compiler/elab_forms.c`) pre-registers each binding in
**Pass A** with a placeholder `TY_FN` stub whose return type comes from a
lightweight keyword peek. That peek resolved only `int / bool / void / nil /
cstr` and defaulted everything else to the int64 carrier (`TY_INT`). The W1 fix
added an override that stamps the real result onto the placeholder -- but only
for `TY_STRUCT` / `TY_APP` / `TY_ADT`. `float` is an **inline carrier scalar**,
so it matched neither the scalar fast-path nor the W1 override, and fell through
to `TY_INT`.

Consequences, both from the same collapsed placeholder:

1. **Type error.** The self-call inside the body (elaborated in Pass B) reads
   the `TY_INT` placeholder, so `(go ...)` comes back typed `int`. An `if`
   whose other arm is the real `float` return then fails with a spurious
   `then=float else=int` mismatch.

2. **Runtime corruption when ascribed.** Adding `(:: (go ...) :float)` to
   silence (1) tells emit the operand is an int64 carrier word that needs
   decoding to `double`. But the emitted C function already returns a native
   `double`. The carrier->concrete decode (`src/compiler/emit_core.c`, the
   `carrier_is_inline` branch) emits

   ```c
   __t19 = ((union { int64_t s; double d; }){.s = __fn_1014(...)}).d;
   ```

   Assigning a `double` return into the `int64_t s` field is a **numeric
   truncation** (6.0 -> 6), and reading `.d` then reinterprets integer 6's
   bit pattern as a `double` (~3e-323), which prints as `0.00`. So it
   type-checks but silently returns garbage-near-zero.

## Fix

Resolve `float` (and its `float64` alias) in the Pass A scalar peek, so the
placeholder return is `TY_FLOAT`. The self-call then types `float` (no
ascription, no branch mismatch), and because the placeholder kind now matches
the emitted `double` return, no carrier decode is inserted and the value
survives. This mirrors the top-level forward-decl pass
(`src/compiler/elab_module.c`, which already resolves `float`) and #460's RR1.

```c
else if (rl == 5 && memcmp(rn, "float",   5) == 0) ret_kind = TY_FLOAT;
else if (rl == 7 && memcmp(rn, "float64", 7) == 0) ret_kind = TY_FLOAT;
```

## Out of scope

- **`float32` self-recursion.** `float32` lives in a separate register class
  (xmm vs general-purpose) and is policed by the `TUR-E0707` guard, which
  rejects the int/float bridge as a hard compile error rather than miscompiling
  it. It is deliberately left out of the scalar fast-path -- it never silently
  corrupts, so it is a separate, lower-priority gap.
- **Captured-`^mut`-via-`set!` accumulators** (the author's first attempt) are
  a distinct, by-design limitation: `set!` on a value captured by a closure
  does not write back to the enclosing frame for *any* type (int included).
  Thread state as a return value (now working for `float`) or accumulate into a
  heap cell. Not addressed here.
