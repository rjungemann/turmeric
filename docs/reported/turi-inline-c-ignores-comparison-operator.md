# turi inline-C pattern executor silently drops trailing comparison operators

**Status:** FIXED. `ic_eval_assign_expr` now parses a full binary-operator
expression (precedence-climbing over the existing operand reader) and
fail-closes on any unrecognised trailing token. Regression coverage:
`tests/fixtures/inline-c-binop/` (asserts `!=`, `==`, `>`, `+`, `*`, `%`,
`<<`, `&&`, `!` match the compiled path; on the turi allowlist). Fix directions
1 and 2 below were both taken.

**Summary:** The interpreter's "simple inline-C" pattern executor evaluated
`return p != 0;` (and `p > 3`, `p == k`, etc.) as just `p` -- it reads the
first operand and ignores the binary operator entirely. This is a **silent
miscompile**: the function returns the wrong value, not an error.

**Severity:** Medium silent miscompile. It is masked in the common
`(when (ptr-some? v) ...)` idiom because the wrong value (the raw pointer)
happens to be truthy/falsy in the same direction as the intended boolean, so
programs "work by luck." Any program that uses the numeric result of such a
helper -- or compares against a non-zero RHS where the operand's truthiness
diverges from the comparison -- gets a wrong answer with no diagnostic.

## Minimal repro

```turmeric
(defn neq0 [p] : int
  ```c
  return p != 0;
  ```)
(defn gt3 [p] : int
  ```c
  return p > 3;
  ```)
(defn main [] : void
  (println (neq0 5))   ; expect 1
  (println (neq0 0))   ; expect 0
  (println (gt3 5))    ; expect 1
  (println (gt3 2)))   ; expect 0
```

Observed vs. expected:

| call       | compiled (`tur run`) | interpreter (`tur --interpret`) |
| ---------- | -------------------- | ------------------------------- |
| `(neq0 5)` | `1`                  | `5`  (WRONG)                     |
| `(neq0 0)` | `0`                  | `0`                             |
| `(gt3 5)`  | `1`                  | `5`  (WRONG)                     |
| `(gt3 2)`  | `0`                  | `2`  (WRONG)                     |

The compiled path is correct; only the interpreter's inline-C shortcut is wrong.

## Root cause

`ic_eval_assign_expr` in `src/turi/eval.c`. After it skipped casts and read a
leading identifier that resolved to a parameter, it handled `.field` and
`->field` accessors, then unconditionally returned the bare operand
(`*out_val = arg->as_int; return true;`). There was no check for a trailing
binary operator (`!=`, `==`, `<`, `>`, `<=`, `>=`, `&&`, `||`, `+`, `-`, ...)
following the operand, so the RHS of the comparison was discarded. The same
omission applied to the integer-literal and `true`/`false`/`NULL` fast paths: a
literal LHS in a comparison would also drop the operator.

This is reached from Pattern 7 in `try_exec_simple_inline_c` (the
`!has_malloc && !has_arrow && has_return && !has_fptr && !has_switch` arm).

## Fix

`ic_eval_assign_expr` was split into:

- `ic_eval_operand` -- the original operand logic (casts, literals, param +
  `.field`/`->field`), now advancing a `const char **` cursor and also handling
  leading unary `!` / `~` / `-`.
- `ic_binop_prec` / `ic_apply_binop` -- a small operator table with C-like
  precedence (two-character operators matched before their one-character
  prefixes so `<=`/`<<` are not read as `<`).
- `ic_eval_binexpr` -- a precedence-climbing, left-associative evaluator over
  operands, returning false (no partial value) the moment any sub-operand fails.
- `ic_eval_assign_expr` -- now a thin wrapper that evaluates one binary
  expression and **fail-closes**: if anything other than trailing whitespace or
  a single `;` remains, it returns false so the caller falls back to the honest
  "inline-C not supported" error instead of a wrong value.

Grouping parens (`(a + b) > 3`) remain unsupported -- the cast-stripper treats a
`(` followed by an identifier as a cast -- but they now fail closed (error)
rather than silently miscompiling. Pointer deref (`*(int64_t *)p`) is likewise
still unsupported (user inline-C, TI7 carve-out).

## Why it was not caught

The canonical generator fixtures (`gen-range`, `gen-if-branch`, `gen-nested`,
`gen-early-return`) ship their own `ptr-some?` (`return p != 0;`) /
`ptr-unwrap` (`return *(int64_t *)(intptr_t)p;`) inline-C helpers. Under the
compiled path these are correct. They are not on the turi allowlist, so the
interpreter never exercised them -- and even if it did, `ptr-some?` returning
the raw non-NULL pointer is still truthy, so `(when (ptr-some? v) ...)` masks
the defect. (`ptr-unwrap`'s deref is separately unhandled and errors out,
which is how the gap first surfaced while landing TI2 generators.)

## Proposed fix directions

1. **Parse a trailing binary operator in `ic_eval_assign_expr`.** After the
   operand is resolved, `ic_skip_ws` and check for a comparison/arithmetic
   operator; if present, recursively evaluate the RHS via the same function and
   fold the result. Restrict to a small, well-defined operator set so the
   executor stays a conservative matcher (anything it does not understand
   should make it return `false` -> fall through to the existing
   "unsupported inline-C" error, never a wrong value).
2. **Fail closed when trailing tokens remain.** Cheaper and safer as a first
   step: if, after reading the operand (and any `.`/`->` accessor), there are
   non-whitespace, non-`;` characters left before the statement terminator,
   return `false` instead of `true`. This turns the silent miscompile into the
   honest "inline-C not supported in interpreter mode" error, which callers can
   then address by using the stdlib helper path. Combine with (1) for the
   common comparison shapes.

## Validation

Add a fixture (or extend an existing interpreter inline-C fixture) covering
`!= 0`, `== k`, `> n`, and a literal-LHS comparison, asserting the interpreter
output matches the compiled output. The repro above is a ready starting point.

## Notes / scope

Found while implementing TI2 (generators) of
`docs/upcoming/v1/turi-parity-post-v1-plan.md`. Out of scope for that change:
the supported turi generator-consumption path uses the `stdlib/gen.tur`
helpers (`gen-some?` / `gen-unwrap` / `gen-none`), which TI2 backs with native
overrides, so generator fixtures do not depend on this inline-C shortcut.
Hand-rolled inline-C pointer helpers remain a compiled-path feature (TI7
carve-out) regardless of this bug.
