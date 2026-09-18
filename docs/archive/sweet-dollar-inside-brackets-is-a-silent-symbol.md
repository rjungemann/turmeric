# `$` inside any bracket is silently parsed as a bare symbol

**RESOLVED 2026-09-17.** Fixed by the report's preferred direction -- diagnose
it -- as **TUR-E0332**, emitted from the reader rather than the preprocessor.
The `bd == 0` guard stays exactly as it was: a parenthesised form really is
plain s-expression territory with no "rest of the line" to delimit, so
declining the rewrite was never the bug. The missing piece was the diagnostic.

The reader turned out to be the better site than the preprocessor's `$` branch.
Every *other* decline path rewrites to something -- `$` at EOL or before a
comment wraps an empty rest, `$x` interns as the symbol `$x` -- so a **bare
`$` arriving from a `READER_SWEET` file means exactly one thing**, which makes
the check a two-line guard in `read_symbol_or_minus` with no new plumbing, and
the existing span machinery maps the caret back through the xform map onto the
user's original source for free. Plain `.tur` files are untouched; a `$`
identifier there is still legal.

Pinned by `tests/fixtures/errors/sweet-dollar-inside-brackets`, and verified on
all three bracket kinds and on the compiled and interpreted paths alike (one
shared reader, so no `requires.*` marker). The follow-up guide audit the report
carried was split out to
[docs/upcoming/sweet-dollar-guide-audit-plan.md](../upcoming/sweet-dollar-guide-audit-plan.md)
rather than archived with it.

---

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

## Follow-up task (split out, not archived)

The guide audit this report carried -- sweep the guides and use `$` where it is
pertinent -- is a docs task, not a finding, so it moved to
[docs/upcoming/sweet-dollar-guide-audit-plan.md](../upcoming/sweet-dollar-guide-audit-plan.md)
when this report was archived.
