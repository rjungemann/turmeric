# Codegen Parentheses Warnings -- Plan (PW0--PW3)

> **Status:** Done (PW0--PW3). BIN_INFIX / PREFIX_UNARY / VARIADIC_FOLD
> templates trimmed in `emit_core.c`; adversarial fixture added under
> `tests/fixtures/codegen-paren-precedence/`; all `expected.c` snapshots
> regenerated.
>
> **Flag:** None. Pure codegen change; output stays semantically identical.
>
> **Last updated:** 2026-05-31
>
> **Related:**
> - `src/compiler/emit_core.c::emit_builtin_call` (the BIN_INFIX / VARIADIC_FOLD
>   / PREFIX_UNARY branches at lines ~982-1010) -- the over-parenthesizing
>   codegen site.
> - `src/compiler/emit_expr.c::emit_if_value` line 507 -- one of the
>   consumers that adds its own paren wrap around the condition.
> - `src/compiler/builtins.c` -- declares which Turmeric primitives map to
>   `BS_BIN_INFIX` (`=`, `<`, `>`, `<=`, `>=`, `not=`, arithmetic, bitwise,
>   `mod`, ...).

---

## Symptom

Every `(if (= x 0) ...)` Turmeric expression compiles into C that clang
flags with `-Wparentheses-equality`:

```
/tmp/tur-build/main_tur.c:4198:53: warning: equality comparison with
    extraneous parentheses [-Wparentheses-equality]
 4198 |     if (((conn_726) == (INT64_C(0)))) {
      |         ~           ^              ~
```

The warning surfaces on every TLS smoke run, every realistic Turmeric
program, and -- left unfixed -- buries genuine clang diagnostics under
the noise. It is purely cosmetic today; no codegen bug, no UB, no
runtime difference.

## Root cause

Two codegen layers each wrap the same subexpression in parens, so a
single Turmeric form generates three nested paren pairs in the emitted
C:

| Source                       | Emitted                  |
|------------------------------|--------------------------|
| Turmeric: `(= x 0)`          | C: `((x) == (INT64_C(0)))` |
| Turmeric: `(if cond ...)`    | C: `if (cond) { ... }`   |
| **Composition: `(if (= x 0) ...)`** | **C: `if (((x) == (INT64_C(0)))) { ... }`** |

The inner `((...))` triggers clang's heuristic: "extra parens around an
equality look like the author silenced an assignment-in-condition
warning the way `if ((x = y))` does."

The same nesting affects `while`, ternaries, and any context that
itself wraps the condition in parens. `if` is just the most common.

## Design

### Why the outer paren in `BS_BIN_INFIX` exists at all

The shape's docstring (`builtins.h:13`) reads `"(a) <op> (b)"`. The
intent of wrapping the operands in `(...)` is precedence safety: when
the result is composed into a larger expression like `foo + (= a b)`,
without the inner wraps you'd get `(a) == (b) + foo` parsed wrong.

The *outer* pair, by contrast, only guards against the same hazard if
the surrounding context is something like `&` or unary `!`. In every
context where the result lands -- assignment RHS, function argument,
`if`/`while` condition, return value, ternary leg -- the surrounding
syntax already provides the boundary. The outer pair is therefore
purely defensive: removing it cannot break parsing as long as each
operand stays wrapped.

### The fix

Change the BIN_INFIX template from

```c
"((%s) %s (%s))"   // current
```

to

```c
"(%s) %s (%s)"     // proposed
```

That single substitution removes the extra nesting from every operator
declared with `BS_BIN_INFIX` (the full set: `=`, `<`, `>`, `<=`, `>=`,
`not=`, `+`, `-`, `*`, `/`, `mod`, `bit-and`, `bit-or`, `bit-xor`,
`shift-left`, `shift-right`, `and`, `or` -- across each numeric type
the builtins table instantiates).

`if (((x) == (y)))` becomes `if ((x) == (y))`. The condition has
exactly one paren layer (the one `if (...)` itself contributes), the
operands stay wrapped for precedence, and clang stops warning.

### Parallel cleanup for the other shapes

While in the area, two sibling templates have the same harmless
over-wrap and benefit from the same trim:

| Shape              | Current                                       | Proposed                              |
|--------------------|-----------------------------------------------|---------------------------------------|
| `BS_BIN_INFIX`     | `((a) OP (b))`                                | `(a) OP (b)`                          |
| `BS_PREFIX_UNARY`  | `(OP(a))`                                     | `OP(a)`                               |
| `BS_VARIADIC_FOLD` | `((a) OP (b)) OP (c)) OP (d)` (builds nested) | `((a) OP (b)) OP (c)` (drop outer)    |

`PREFIX_UNARY` covers `not`, `bit-not`, unary `-`. These are
`!(x)` / `~(x)` / `-(x)` -- already self-bounding tokens so the outer
paren adds nothing.

`VARIADIC_FOLD` is the only one where the inner parens *do* carry
their weight (they group each fold step). Only the outermost wrap is
redundant.

### Why not just `-Wno-parentheses-equality`?

Three reasons:

1. **It hides real cases too.** If a user writes `if ((x = compute()))`
   in inline-C and meant `==`, the warning is genuinely useful. Disabling
   it globally trains us to ignore a useful diagnostic.
2. **It only addresses clang.** GCC's `-Wparentheses` is broader and
   flags similar patterns; the codegen fix silences both compilers.
3. **The output is plainly nicer to read.** Generated C with three
   nested paren pairs around every comparison is genuinely harder to
   debug; humans grep generated C more often than the codebase
   acknowledges.

## Verification strategy

The change cannot alter program semantics -- removing redundant
parentheses from an expression preserves parsing as long as the
operands remain parenthesized. Verification is therefore behavioural,
not syntactic:

1. `bash tests/run.sh` -- 1129/1129 must stay green; same exit codes,
   same diagnostics.
2. **Fixture-snapshot regeneration.** `tests/fixtures/*/expected.c`
   captures the codegen; many fixtures will diff. The CLAUDE.md
   "Fixture Snapshots" rule applies: regenerate via the documented
   loop and commit the snapshot updates in the same PR.
3. **Warning-count audit.** Build the tls-roundtrip and ctx-lifecycle
   spice fixtures; count `-Wparentheses-equality` lines in the cc
   output before and after. Expected: drop to zero on Turmeric-emitted
   code (inline-C may still hit them; that's a user choice).
4. **One adversarial composition test.** Add a fixture like
   `(let [a 1 b 2 c 3] (if (and (= a 1) (or (< b c) (= b 2))) ...))`
   and confirm it both compiles and produces the expected output. This
   exercises BIN_INFIX as an operand of another BIN_INFIX, which is
   where missing parens would bite if the analysis above were wrong.

## Phases

| Step | Task |
|---|---|
| PW0 | Add the adversarial fixture under `tests/fixtures/codegen-paren-precedence/` (or extend an existing fixture) with `=`, `<`, `and`, `or`, and a nested arithmetic case. Verify it passes against current codegen first -- the baseline must work. |
| PW1 | Patch `emit_core.c::emit_builtin_call` for the three shapes: BIN_INFIX, PREFIX_UNARY, VARIADIC_FOLD. One commit, three buf_printf templates. |
| PW2 | Regenerate `tests/fixtures/*/expected.c` snapshots via the documented loop. Spot-check 2-3 diffs to confirm only paren-density changed, no logic shifted. |
| PW3 | Re-run `bash tests/run.sh` (must show 1129+/0); re-run both tls fixtures and confirm `-Wparentheses-equality` is gone. |

PW0 + PW3 are bookend safety checks. PW1 is the meaningful change.
PW2 is the mandatory snapshot churn.

## Risks and open questions

- **Risk: hidden consumer that requires the outer wrap.** Searched
  `emit_core.c` / `emit_expr.c` for sites that concatenate a builtin's
  result without rewrapping. None found that depend on the outer pair
  for correctness; all wrap their own context (e.g. `if (%s)`, `%s = %s;`,
  function-arg `(%s)`). Confidence is high but not absolute -- PW0's
  fixture is the safety net.
- **Risk: snapshot churn obscures real review.** Mitigated by landing
  PW1 and PW2 in one PR with a description that calls out
  "codegen-only paren removal; snapshots regenerated mechanically."
  Reviewers can then skim the snapshot diff for shape-only changes
  rather than scrutinize every byte.
- **Open: should we also drop the operand parens?** I.e. emit `a OP b`
  instead of `(a) OP (b)`. **No.** The operand expressions can be
  arbitrary -- `(a + 1)` or `(foo() ? x : y)` -- and the operand wrap
  is what makes BIN_INFIX composable. Dropping it would break the
  precedence guarantee that justifies BIN_INFIX's existence.
- **Open: inline-C still emits the warning when users write `if ((x == y))`.**
  Out of scope: that's user-written C in inline blocks, not codegen
  output. Document it once in the inline-C guide; do not modify.
