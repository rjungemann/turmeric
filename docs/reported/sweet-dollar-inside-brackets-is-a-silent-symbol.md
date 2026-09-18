# `$` inside any bracket is silently parsed as a bare symbol

**Summary:** In a sweet-exp file, a `$` rest-of-line marker that appears inside
`(...)`, `[...]`, or `{...}` is not rewritten and not diagnosed -- it survives
into the AST as an ordinary symbol named `$`, changing the shape of the form
with no error and no warning.

**Severity:** Medium. Wrong parse, zero feedback. The program either fails far
downstream with a confusing error about `$`, or -- if the surrounding form
tolerates an extra element -- silently means something else.

## Minimal repro

`d2.sweet`:

```
def g
  (fn [msg : cstr] : unit
    log/error $ str("a" msg))
```

`d2.tur` -- what the author meant:

```
(def g (fn [msg : cstr] : unit (log/error (str "a" msg))))
```

```sh
$ tur parse-check d2.tur d2.sweet
tur: parse-check mismatch between d2.tur and d2.sweet
--- d2.tur ---
(def g (fn [msg :cstr] :unit (log/error (str "a" msg))))
--- d2.sweet ---
(def g (fn [msg :cstr] :unit log/error $ (str "a" msg)))
```

Note the `$` in the produced AST. The `(fn ...)` body is now three flat
elements instead of one call.

At top level the same line works as documented, so the failure is purely a
function of being inside a bracket:

```
defn f [msg : cstr] : unit
  log/error $ str("a" msg)         ; correct: (log/error (str "a" msg))
```

## Root cause

`src/compiler/reader.c:4266`

```c
if (bd == 0 && c == '$' &&
    (i + 1 >= end || src[i + 1] == ' ' || src[i + 1] == '\t')) {
    /* Replace `$ <rest>` with `(<rest>)`. */
```

`bd` is the bracket depth maintained by the sweet-exp preprocessor. The whole
indentation layer -- line grouping and `$` alike -- is deliberately switched
off inside brackets, because a `(...)` form is plain s-expression territory
where whitespace is insignificant. That part is correct and intended (the
design note at `src/compiler/reader.c:3709` says `$` applies "at top level (not
inside brackets/string/comment)").

What is missing is the diagnostic. When the preprocessor declines to rewrite a
`$`, the token is copied through verbatim, and the reader downstream has no
reason to think a bare `$` symbol is anything but an identifier.

## Why it matters now

This is the sharp edge of the sweet-exp style rule that `fn` and `handle` keep
traditional parens (`docs/guides/syntax-guide.md`, "What still uses traditional
parens"). Wrapping a lambda in parens switches off the indentation layer for
its body, so a body written with `$` -- which looked fine a moment earlier --
goes inert. It cost one real conversion in
`docs/guides/contract-types-guide.md` during that pass, and it is the only one
of the indentation-layer constructs that fails *silently*: `when`/`do`/`for`
relying on indentation at least produce a visibly wrong flat form that the
type checker usually rejects, whereas a stray `$` can sail through.

## Fix directions

Preferred: diagnose it. In the preprocessor's `$` branch, when `bd > 0`, emit a
new error (something like `TUR-E0331: '$' has no meaning inside brackets --
the rest-of-line marker only applies where indentation is significant`) with a
note pointing at the delimited rewrite (`f(x)` or `(f x)`). The information is
all in hand at that point: the byte offset, and the fact that the marker was
declined.

Alternative, less good: honor `$` inside brackets too, i.e. drop the `bd == 0`
guard. This would make the marker uniform, but it re-introduces a
whitespace-sensitive construct into a region the language otherwise promises
is whitespace-insensitive, and it would change the meaning of any existing
program with a literal `$` symbol inside a bracket. Not recommended without a
deprecation window.

Either way, `tests/fixtures/` wants a negative fixture (`errors/`) pinning the
chosen behavior.

## Follow-up task: audit the guides and use `$` where it is pertinent

Separate from the diagnostic above, the guides under-use `$`. The `fn`/`handle`
paren pass (see `docs/guides/syntax-guide.md`, "What still uses traditional
parens") moved a lot of code *into* brackets, where `$` is inert -- so it is
worth a deliberate sweep of where `$` still belongs and reads better.

Scope: every ```sweet-exp block in `docs/guides/*.md`.

The shape `$` is for -- a call whose single argument is itself a call with
space-separated arguments, at a position where the indentation layer is live
(i.e. NOT inside `(...)`, `[...]`, or `{...}`):

```
; before
println(str-concat("Hello, " name))

; after
println $ str-concat "Hello, " name
```

Rules for the sweep:

- **Only outside brackets.** Inside a parenthesised form `$` is inert (this
  report). A `$` sweep must not touch a `(fn ...)` or `(handle ...)` body.
- **Single argument only.** `$` takes the rest of the line as one argument, so
  a call with two or more arguments is not a candidate:
  `seq/map((fn [x] *(x x)) seq/range(1 6))` stays as it is.
- **Leave `f((fn ...))` alone unless it reads better.** There are ~42 sites of
  the form `arr((fn [x] +(x 1)))` where `arr $ (fn [x] +(x 1))` is arguably
  cleaner. This is a judgement call per site, not a mechanical rewrite --
  prefer it where the `((` is genuinely dense, skip it where the neoteric call
  is already short.
- **A bare atom after `$` is a zero-arg call.** `f $ g` is `(f (g))`, not
  `(f g)` (SRFI-110). Do not introduce `$` before a bare variable.
- Verify with
  `TUR_STDLIB_DIR="$PWD/stdlib" python3 tools/check-guide-pairs.py docs/guides/ --tur ./build/tur`;
  every pair must stay AST-identical to its ```turmeric sibling.
