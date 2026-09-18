# Audit the guides and use `$` where it is pertinent

**DONE 2026-09-17 -- and the plan's premise was partly wrong, so the executed
scope is much smaller than what is written below.**

The plan asserted that "`$` is one of sweet-exp's three tools, and the guides
barely demonstrate it." Measuring before acting:

- **The syntax guide already teaches `$` definitively** -- a dedicated
  "Rest-of-line `$`" tool section with a worked `turmeric`/`sweet-exp` pair, a
  five-row table covering every rest-of-line shape (bare token sequence,
  neoteric call, parenthesised form, curly-infix group, chained `$`), and the
  SRFI-110 bare-atom gotcha (`f $ g` is `(f (g))`). It is pinned by
  `tests/fixtures/sweet-exp-dollar-shapes`. Nothing was needed here.
- **`$` appears in 35 of 973 sweet-exp blocks, across 13 guides** -- thin at
  ~3.6%, but "barely" overstated it.

The one gap that did hold up: **the tutorials used `$` zero times** while
teaching sweet-exp. `quickstart.md`, `repl-tutorial.md`,
`custom-effects-tutorial.md` and `snake-game-tutorial.md` were all at zero, and
those are where a newcomer forms habits.

## What was executed

A mechanical scan found 89 candidate sites (a depth-0 line that is one
single-argument neoteric call wrapping a call with >= 2 arguments). Per the
plan's own "prefer breadth over depth", **9 were converted, all in the three
teaching docs**:

- `quickstart.md` -- `unwrap $ safe-div 10 2`, `println $ vec-get v 1`,
  `println $ vec-get squares j`, `println $ str-concat "Hello, " ...`
- `repl-tutorial.md` -- the same three shapes, kept consistent with quickstart
  since the two documents mirror each other
- `custom-effects-tutorial.md` -- `perform $ Log "info" "starting"` / `"done"`

All nine are length-neutral, so every trailing-comment column stayed put.

Deliberately **skipped**:

- **`area(Rect(6 7))`** and the `snake-game-tutorial.md`
  `GameState(Snake(vec(Segment(400 300) ...) 1))` nests. `$` would flatten only
  the OUTERMOST of four constructor levels, making it look unlike the three
  inside it. Capitalised constructors read better with their call parens.
- **All 10 `f((fn ...))` sites** (`seq/map((fn [x] *(x x)))`,
  `fresh((fn [x] goal))`, ...). The plan says not to convert these on
  principle, they live on reference pages rather than teaching docs, and the
  syntax guide's shape table already teaches `$ (fn ...)` composition.
- The other ~70 reference-page candidates, for the same reason: a third
  rewrite pass over blocks already churned twice buys nothing.

Verified: all 876 guide pairs stay AST-identical under `tur parse-check`, and
no `$` landed at bracket depth > 0 (which would now be TUR-E0332).

---

Split out of
[docs/archive/sweet-dollar-inside-brackets-is-a-silent-symbol.md](../archive/sweet-dollar-inside-brackets-is-a-silent-symbol.md)
when that report was resolved as TUR-E0332. The defect is fixed; this is the
docs half, which is a style task rather than a finding.

## Why

`$` is one of sweet-exp's three tools, and the guides barely demonstrate it.
The `fn`/`handle` paren pass made that worse in one direction and better in
another:

- It moved a lot of guide code *into* brackets, where `$` is now a hard error
  (TUR-E0332). Those sites are correctly `$`-free and must stay that way.
- What remains outside brackets is where `$` belongs, and much of it is still
  written with nested parens that `$` would flatten.

So the goal is **not** "put `$` back". It is: make sure the tool is shown where
it genuinely reads best, so a reader learning sweet-exp from the guides sees
all three tools rather than two.

## Scope

Every ```sweet-exp block in `docs/guides/*.md`.

The shape `$` is for -- a call whose single argument is itself a call with
space-separated arguments, at a position where the indentation layer is live:

```
; before
println(str-concat("Hello, " name))

; after
println $ str-concat "Hello, " name
```

## Rules

- **Only outside brackets.** Inside `(...)`, `[...]` or `{...}` a `$` is now
  TUR-E0332. Never touch a `(fn ...)` or `(handle ...)` body.
- **Single argument only.** `$` takes the rest of the line as one argument, so
  a call with two or more arguments is not a candidate:
  `seq/map((fn [x] *(x x)) seq/range(1 6))` stays as it is.
- **`f((fn ...))` is a judgement call, not a mechanical rewrite.** There are
  ~42 sites of the form `arr((fn [x] +(x 1)))` where `arr $ (fn [x] +(x 1))`
  is arguably cleaner. Prefer `$` where the `((` is genuinely dense; skip it
  where the neoteric call is already short. Do not convert all 42 on principle
  -- that trades one uniform density for another and churns the guides again.
- **A bare atom after `$` is a zero-arg call.** `f $ g` is `(f (g))`, not
  `(f g)` (SRFI-110). Do not introduce `$` before a bare variable.
- Prefer breadth over depth: a couple of well-chosen `$` examples in the
  syntax guide and the tutorials teach the tool better than 42 mechanical
  rewrites scattered through reference pages.

## Verification

```sh
TUR_STDLIB_DIR="$PWD/stdlib" python3 tools/check-guide-pairs.py docs/guides/ --tur ./build/tur
```

Every pair must stay AST-identical to its ```turmeric sibling -- `tur
parse-check` is what makes a `$` rewrite safe, since a wrongly-placed `$`
changes the shape rather than erroring (outside brackets, where it is still
legal).

## Not worth doing

Converting guide prose that merely *mentions* `$`, and the deliberate
"BROKEN"/"Bad" examples in `docs/guides/syntax-guide.md` that exist to show
what TUR-E0332 rejects.
