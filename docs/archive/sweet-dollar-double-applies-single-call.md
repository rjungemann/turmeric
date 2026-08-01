# Sweet-exp `$` double-applies when the rest-of-line is already one complete call

> **RESOLVED 2026-08-01 -- Fix direction 1 (suppress the redundant wrap).**
> `sweet_emit_content` now shape-tests the rest-of-line and emits it unwrapped
> when it is already one complete delimited expression, so `$` composes with
> neoteric, parenthesised forms, curly-infix and data literals. Both repros
> below check clean, as do the two documented examples. Details in
> *Resolution* at the end of this document.

**Severity:** medium (expressiveness / docs). `$` is one of the three
sweet-exp tools the project conventions tell you to reach for, and the
conventions' own chained example does not compile. There is a workaround
(drop the `$`), so nothing is blocked.

Verified against `./build/tur` at **v0.32.2** (Debug), on branch
`claude/composite-type-alias-gap-yd70bn`.

## Summary

`$` is documented as "rest-of-line replaces the surrounding outer `(...)`" --
it wraps everything to its right in one pair of parens. The rewrite is purely
textual and unconditional, so when the rest-of-line is *already* a single
complete expression, the wrap adds a second application layer:

| Source | Becomes | Correct? |
|---|---|---|
| `println $ str-concat "a" name` | `(println (str-concat "a" name))` | yes -- rest is a bare sequence |
| `println $ g(7)` | `(println ((g 7)))` | **no** -- double application |
| `println $ (g)` | `(println ((g)))` | **no** -- same |

So `$` composes with a bare space-separated sequence but not with a neoteric
call or a parenthesised expression -- exactly the two spellings the rest of
the sweet-exp style encourages.

## Repro

```turmeric
#lang sweet-exp

defn g [x : int] : int
  x

defn main [] : int
  println $ g(7)
  0
```
```
error: expression in call head has type `int`, which is not callable
  println $ g(7)
             ^^^
```

`(g 7)` is an `int`, and the extra parens try to call it.

Dropping the `$` -- `println(g(7))` -- checks clean, as does the bare-sequence
form `println $ g 7`. The defect is only the interaction.

## The documented example is affected

`CLAUDE.md`'s sweet-exp section teaches chained `$`:

```turmeric
println $ normalize $ vec3(1.0 0.0 0.0)
```

That shape does not compile -- the trailing `vec3(...)` is a neoteric call, so
it gets double-applied:

```turmeric
#lang sweet-exp
defn norm [x : int] : int
  x
defn v3 [a : int b : int c : int] : int
  {a + b + c}
defn main [] : int
  println $ norm $ v3(1 2 3)
  0
```
```
error: expression in call head has type `int`, which is not callable
  println $ norm $ v3(1 2 3)
                      ^^^^^^
```

The neighbouring example in the same section,
`println $ str-concat "Hello, " name`, is correct -- its rest-of-line is a
bare sequence.

## Root cause

`sweet_emit_content` (`src/compiler/reader.c:3875`) rewrites `$` to
`(<rest-of-line>)` as a byte-level transform while tracking string, block
comment and inline-C state. It never inspects the shape of the rest-of-line,
so it cannot tell "a sequence that needs wrapping" from "an expression that is
already wrapped".

## Fix directions

1. **Suppress the wrap when the rest-of-line is already exactly one
   expression** -- i.e. when it is a single neoteric call (`f(...)`) or a
   single parenthesised form, with nothing before or after it on the line.
   This is the minimal rule and makes both documented examples work. It has to
   run after the paren-depth scan the emitter already does, since the test is
   "does one balanced group span the whole remainder."
2. **Reject it** with a diagnostic that names the redundancy ("`$` before a
   complete call is redundant -- write `println(g(7))` or `println $ g 7`"),
   and fix `CLAUDE.md`. Cheaper and unambiguous, but it leaves `$` unable to
   compose with neoteric, which is a real ergonomic loss for the chained form.
3. Either way, add sweet-exp fixtures covering `$` in front of each
   rest-of-line shape -- bare sequence, neoteric call, parenthesised form,
   chained `$` -- since none currently exist.

Option 1 matches the documented intent; option 2 is the honest cheap answer if
the shape test turns out to be awkward at the byte level.

## Found while

Validating `docs/guides/logic-programming-guide.md` code blocks for
[composite-type-alias-gap.md](history/composite-type-alias-gap.md).
Confirmed unrelated to that change -- it reproduces identically with no
`defalias` in the file.

## Resolution

**Fix direction 1.** Option 2 (reject with a diagnostic) was the cheap answer,
but the shape test turned out not to be awkward at the byte level, and the
guides had already committed to option 1's semantics in prose -- both
`docs/guides/syntax-guide.md`'s cheat sheet (`f $ g(x)` => `(f (g x))`) and
`docs/guides/binding-forms-guide.md`'s `println $ if(even?(10) "even" "odd")`
document the composing behaviour. Option 2 would have meant retracting
published surface rather than delivering it.

### The shape test

`sweet_dollar_rest_is_delimited()` (`src/compiler/reader.c`) answers "is
`[start, end)` already exactly one complete, delimited expression?" It trims
surrounding whitespace, then splits the range into

- a **prefix run** -- everything before the first `(`, `[` or `{`. Whitespace,
  a string quote, a stray closer, a line continuation or a second `$` anywhere
  in it means the rest is a token *sequence*, not one expression, and the test
  fails immediately.
- one **balanced group**, scanned string-aware, which must close exactly at
  `end`.

An empty prefix is a parenthesised form / curly-infix group / bracket literal;
a non-empty one glued to the delimiter is a neoteric call (`g(7)`), a reader
dispatch (`#map{...}`) or a quote sigil (`'(1 2)`). The `$` branch of
`sweet_emit_content` consults it and skips the `(` / `)` insertion when it
answers yes; the recursion for chained `$` is unchanged, so each `$` on a line
decides independently.

The test runs on the raw bytes of the rest-of-line, *before* the recursive
`sweet_emit_content` call, which is what makes it cheap -- no second paren-depth
pass, no reliance on the emitter's own state.

### What did not change

A **bare atom** after `$` is still wrapped: `f $ g` remains `(f (g))`. That is
SRFI-110's specified reading and it is a zero-argument call, not a double
application, so it is not part of this defect. A rest-of-line containing a `\`
line continuation is conservatively treated as a sequence (the prefix scan
rejects `\`), matching the pre-fix behaviour for the one shape where it was
already correct.

### Coverage

`tests/fixtures/sweet-exp-dollar-shapes/` -- fix direction 3 -- exercises `$`
against every rest-of-line shape in one program: bare sequence, bare atom,
neoteric call, parenthesised form, curly-infix group, chained `$`, a call with
a trailing line comment, and a continuation-spanning sequence. Full suite:
2501 passed, 0 failed.

### Docs

- `CLAUDE.md` -- the sweet-exp `$` section gained the suppression rule and the
  bare-atom exception. Its chained example, `println $ normalize $
  vec3(1.0 0.0 0.0)`, now compiles as written.
- `docs/guides/syntax-guide.md` -- the section introducing tool 3 never showed
  a `$` at all: its sweet-exp pane gave the neoteric spelling. Replaced with
  the `$` form, plus a worked list of the shapes and the bare-atom note.
