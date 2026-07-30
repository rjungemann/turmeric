# Sweet-exp `$` double-applies when the rest-of-line is already one complete call

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
[composite-type-alias-gap.md](../archive/composite-type-alias-gap.md).
Confirmed unrelated to that change -- it reproduces identically with no
`defalias` in the file.
