# `tur run` Justfile Assignment Evaluation Plan

**Status:** resolved 2026-07-02
**Track:** one-track-to-v1 -- `tur run` compatibility hole
**Author:** investigation 2026-07-02

## Resolution (2026-07-02)

Landed in `src/compiler/justrun.c`:

- `parse_rhs_expr_text` -- balanced RHS scanner (parens/braces/brackets,
  string literals with escapes, `#` comments outside brackets).
- `eval_rhs` + `re_primary`/`re_expr`/`re_concat` -- recursive-descent
  evaluator covering string literals, function calls (delegating to
  `eval_builtin`), variable lookups (with env-var fallback), `/` and `+`
  concat, and `if EXPR (==|!=) EXPR { EXPR } else { EXPR }` conditionals.
- Removed the top-level `if` fail-close from `check_unsupported`;
  conditional-in-RHS is handled by the evaluator, and a bare top-level
  `if` isn't a just statement.

Regression harness: `tests/run-tur-run-rhs-eval.sh` (wired into
`CMakeLists.txt` as `tur_run_rhs_eval`) covers eight scenarios --
balanced-parens function call, `/` concat, `+` concat, host-OS
conditional, the distilled scite nested case, env-var override, forward-
reference error, and bare-ident RHS resolution.

Verified manually with the scite reproducer:

```
preset := env_var_or_default("TROWEL_PRESET", if os() == "macos" { "macos-debug" } else { "linux-debug" })
build_dir := "build" / preset
```

`tur run show` now interpolates `preset=macos-debug` /
`build_dir=build/macos-debug` correctly, and `TROWEL_PRESET=<x>` overrides
as expected.

## TL;DR

`tur run`'s embedded Justfile interpreter (`src/compiler/justrun.c`) stores
`name := VALUE` assignments as raw byte-fragments and interpolates them
verbatim into recipe command lines. The RHS is never parsed as an expression
or evaluated. This falls over on any Justfile whose variable RHS is more than
a single bare word or quoted string -- including the real-world
`../turmeric-scite/Justfile`, which uses `env_var_or_default(...)` combined
with a conditional expression and `/` path concatenation.

Reproducer (from that repo):

```
preset := env_var_or_default("TROWEL_PRESET", if os() == "macos" { "macos-debug" } else { "linux-debug" })
build_dir := "build" / preset
```

`tur run editor` currently emits

```
cmake --preset env_var_or_default("TROWEL_PRESET",
sh: -c: line 0: syntax error near unexpected token `('
```

because `parse_value` (line 212-216) is a bare-word scanner: it stops at the
first ASCII space and stores the truncated fragment. `{{preset}}` in the
recipe body then interpolates that fragment verbatim into the shell command
via `jenv_get` (line 967) with no evaluation step.

Plan: extend `justrun.c` with (a) a balanced RHS scanner, (b) an assignment-
time expression evaluator that reuses the existing `{{...}}` built-in
machinery, and (c) a conditional-expression primitive. Ship it as one
coordinated change -- the three gaps cannot land separately.

## Current state (verified 2026-07-02)

Read sites in `src/compiler/justrun.c` (all line numbers as of `main`
@ 96f262471):

- **Gap 1 -- `parse_value` truncates at whitespace.** Lines 211-216:
  ```
  const char *start = p;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#') p++;
  ```
  No balancing of `(` `)` `{` `}` `[` `]` and no awareness of `"..."` /
  `'...'` inside a bare-word RHS. So `env_var_or_default("TROWEL_PRESET",`
  is captured as the entire value.

- **Gap 2 -- assignment RHS is never evaluated.** Lines 610-660 read the RHS
  with `parse_value` and stash it in `jf->vars[]` unchanged. Lines 966-971
  (`eval_expr`) then look up the variable via `jenv_get` and hand back the
  stored string. The `{{...}}` interpolation path only evaluates expressions
  that appear literally *inside braces in a recipe body*; it never re-parses
  a stored variable value. Even a Gap-1-fixed capture of the full
  `env_var_or_default(...)` text would still be emitted verbatim to the shell.

- **Gap 3 -- top-level conditionals are explicitly rejected.** Lines 353-365
  fail-close on `if COND { ... } else { ... }` at column 0 with
  `unsupported Justfile feature ... conditional expression`. The failure is
  intentional (a friendly "install just" message), but real Justfiles like
  scite's use the conditional as a subexpression *inside* a function-call
  arg, where the current parser doesn't even reach the check -- Gap 1
  swallows it first.

- **Related smaller gaps** in the same file, worth folding into the same
  patch so a single pass leaves the interpreter coherent:
  - String concat operators `/` and `+` in RHS are not parsed.
  - Backtick command substitution in RHS is detected and rejected at
    lines 335-350 (fine as a fail-close for now, but noted).
  - Multi-line RHS wrapping (`\\`-continuation) is not handled.
  - Recipe bodies starting with `#!/usr/bin/env <shell>` shebang are worth
    auditing for exec-vs-pipe semantics, though not required for scite.

## Design

Introduce a single AST-shaped intermediate for the RHS of an assignment,
evaluate it at parse time, and store the resulting **string** in `JVar`.
That keeps the interpolation path unchanged and localizes the new code.

### 1. Balanced RHS scanner (`parse_rhs_expr_text`)

New helper alongside `parse_value` that reads until end-of-logical-line
while balancing `()`, `{}`, `[]`, and skipping over `"..."` / `'...'`
literals (respecting `\"` and `\'` escapes). Honors `#` as a comment
delimiter *only* when not inside a bracket group or string. Handles
backslash-newline continuation. Returns the raw expression text.

Usage: replace the `parse_value(val_start, &end)` call at lines 631 and
656 with `parse_rhs_expr_text(...)`. `parse_value` itself stays put for
recipe-parameter defaults, where the current semantics are correct.

### 2. Assignment-time expression evaluator (`eval_rhs`)

Small recursive-descent evaluator for the just expression grammar we
actually see in the wild:

```
expr    := concat
concat  := primary ( ('/' | '+') primary )*
primary := STRING
         | ident '(' arglist? ')'         -- function call
         | ident                           -- variable lookup
         | 'if' expr COMPOP expr '{' expr '}' 'else' '{' expr '}'
COMPOP  := '==' | '!='
arglist := expr ( ',' expr )*
```

Reuses `eval_builtin` for function calls (same built-in set already
exposed to `{{...}}`). Variable lookups walk `jf->vars[]` (assignments
are evaluated top-to-bottom, so forward references are already illegal
in just). The `if/else` conditional evaluates only `==` / `!=` on
strings; that covers scite's `os() == "macos"` case and every other
real-world Justfile RHS we have seen. Additional operators can be added
one-by-one as the next Justfile in the wild demands them.

Called from the same two assignment sites (post-scanner). The result is
a `char *` that replaces the raw-text value stored in `JVar.value`.

### 3. Remove the top-level conditional fail-close

Lines 353-365 exit early on `if COND { ... } else { ... }` at column 0.
Once (2) lands, a *bare* top-level `if` is still not a valid statement in
just -- it only appears as a subexpression -- so the check can be
deleted, not rewritten.

### 4. Error surface

`eval_rhs` failures (unknown function, unset `env_var`, malformed
conditional) print `tur run: <file>:<line>: <message>` and abort the
recipe run with the current non-zero exit convention. Include a hint to
install upstream `just` for anything still unsupported, matching the
existing tone at lines 311-317.

## Test strategy

Add fixtures under `tests/fixtures/tur-run-*` covering:

- **rhs-balanced-parens:** `x := f("a", "b")` interpolates `a-b` in body.
- **rhs-slash-concat:** `p := "build" / "debug"` -> `build/debug`.
- **rhs-conditional:** `p := if os() == "macos" { "m" } else { "l" }` --
  gated by `requires` on host os or asserts one of two accepted outputs.
- **rhs-nested:** `p := env_var_or_default("X", if os() == "macos" { "a" } else { "b" })` -- the scite case, distilled.
- **rhs-forward-ref-error:** references-later-var yields a clear
  `<file>:<line>: unknown variable` message.

Snapshot the interpolated command line via a tiny `--just-dump-vars`
debug flag, or via a recipe that just echoes `{{p}}`. Prefer the recipe-
echo approach -- no new CLI surface -- so the fixture doubles as an
end-to-end run.

The compatibility target for this PR is: `tur run editor` succeeds in a
checkout of `../turmeric-scite` (with CMake + Qt available). That is the
motivating case and belongs in the PR description as a manual verification
step -- it is not a fixture (heavy external toolchain).

## Non-goals

- Full just parity. Modules/imports, backtick command substitution, and
  `[private]` / `[group:...]` attributes stay rejected with the current
  "install just" message. Anything the scite Justfile does not require is
  out of scope for this patch.
- A staged rollout. This is a small, self-contained interpreter fix on the
  one-track-to-v1; ship it as a single PR with fixtures.

## Risks

- **Silent behavior drift for existing `tur run` callers.** The current
  parser stores raw bytes, so any Justfile that happens to have a bare-
  word RHS containing `/` (rare) would previously interpolate that raw
  slash and would now attempt string concat. Mitigation: only apply the
  concat operator between *primaries*, so a bare word without adjacent
  primary tokens still round-trips as-is. Add a fixture asserting this.
- **eval_builtin already handles `env_var` failure by returning `NULL`.**
  Wire `eval_rhs` to bubble that up as an assignment-time error rather
  than storing an empty string.

## Estimated size

~250-350 lines added in `justrun.c` (scanner + evaluator + tests hook),
minus the ~13 deleted lines of the conditional fail-close. Five to seven
fixtures under `tests/fixtures/`. One PR.
