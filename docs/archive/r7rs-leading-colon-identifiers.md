# `#lang r7rs`: an identifier that starts with `:` does not read as a symbol in source

**RESOLVED 2026-09-26.** In a user Scheme source, a token that starts with `:`
now reads as an identifier (`scheme_user_source`, src/compiler/reader.c; first named
`scheme_colon_is_identifier`, generalised when brackets followed).
So `':x` is the symbol `:x`, `:::` works as a custom ellipsis, and `:x` can be
bound, defined, or named as a macro (SRFI 42's `:range`). The source reader
and `read` agree. Fixtures: `r7rs-colon-identifiers` (both back ends) and
`errors/r7rs-colon-is-not-a-keyword`.

**Correction to the filing below:** this was not an accident. r7rs-lang-plan
R1 kept Turmeric keywords readable in `#lang r7rs` on purpose, and its R4 note
says "`:::` reads as a keyword under this reader". Scheme code used them as
map keys through the seam (`(map-get m :k)`). The resolution follows the
project owner's rule that Turmeric features should not leak into R7RS: it
does NOT fall back to the keyword for an unbound `:k`. Instead:

- A keyword and the symbol of the same name are one runtime value
  (`(eq? :k 'k)` was `#t`), so seam code writes `'k`, and nothing is lost.
  The fixtures that used `:k` (`r7rs-gc-seam`, `r7rs-stdlib-seam`,
  `r7rs-keyword-seed`, `r7rs-reader-lexemes`) now do. An unbound `:k` in a
  Scheme file is "unbound symbol ':k'" with a help line naming `'k`
  (src/compiler/elab_toplevel.c).
- Turmeric's `:` survives in two places:
  - the Turmeric-shaped Scheme sources (the prelude, `stdlib/r7rs/`, and the
    REPL's pinned preload), by the same test as scheme_lower.c's
    `prelude_span`;
  - the inside of a `#map{...}` / `#set{...}` literal, whose keys are
    keywords by the literal's own grammar.

  A `:` or `::` standing alone still reads as Turmeric's annotation and
  ascription. Those, `#map{}` itself, and the rest of the Turmeric surface
  still visible in Scheme source are tracked in
  docs/reported/r7rs-turmeric-syntax-leaks.md.

The r7rs fixtures pass on both harnesses. The r7rs import tests and chibi's
conformance suite were run on the change; their results are recorded in the
commit that landed it.


**Severity:** medium. R7RS 7.1.1 makes `:` an ordinary `<initial>`, so `:x`,
`:::` and `:list` are identifiers. In a `#lang r7rs` source file they are not:
a quoted `:x` is the symbol `x`, and `:::` or a parameter named `:x` is an
error. The runtime `read` gets all of them right, so the source reader and
`read` disagree about the same text. Found while planning SRFI support
(docs/upcoming/r7rs-srfi-plan.md). It blocks SRFI 42 outright, since every
generator is named `:list`, `:range`, `:vector`, `:parallel` and so on. It also
blocks the `:::` custom ellipsis that R7RS 4.3.2 (from SRFI 46) allows.
chibi's R7RS suite never spells such an identifier, which is why the
conformance count did not show it.

## Repro

Measured 2026-09-26 at fdd51fc9, Debug build. Each line below is a separate
program after `#lang r7rs` and
`(import (scheme base) (scheme write) (scheme read))`. The results are the same
on the compiled back end and under `tur --interpret`:

| Source | Result | R7RS says |
|---|---|---|
| `(write ':x)` | `x` | `:x` |
| `(write (symbol->string ':x))` | `"x"` | `":x"` |
| `(write ':::)` | error: unbound symbol ':' | `:::` |
| `(define (f :x) :x) (write (f 3))` | error: lambda formal must be an identifier | `3` |
| `(define-syntax m (syntax-rules ::: () ((_ x :::) (list x :::))))` | error: syntax-rules expects a literals list | a macro |
| `(write (read (open-input-string "(a:b :c ::: x:)")))` | `(a:b :c ::: x:)` | same (correct) |

A colon *inside* an identifier is fine: `'a:b` and `char-set:letter` read and
bind normally, so SRFI 14's `char-set:...` names are not affected. The
custom-ellipsis machinery works when the ellipsis is not colon-led:
`(syntax-rules ooo () ((_ x ooo) (list x ooo)))` expands correctly.

## Root cause (not yet pinned to a line)

The Scheme reader variant (`READER_R7RS`, src/compiler/reader.c) still sends a
leading `:` down Turmeric's keyword / type-annotation path, which the Turmeric
readers need and the Scheme reader does not. The runtime reader
(stdlib/r7rs/read.tur) has its own tokenizer, so it is unaffected. Find the
colon arm the Scheme variant shares with the Turmeric readers.

## Fix directions

- In the Scheme reader variant, read a token that starts with `:` as an
  ordinary identifier, exactly as `read` does. `#lang r7rs` has no keyword
  syntax and no `:` type annotations in its own surface. The prelude is read
  as `#lang r7rs` too, though, and is written in Turmeric's shapes
  (`[s i : int]`, the `(:: v any)` cast). So check what the prelude and the
  on-demand library files depend on, and keep those working. The annotation
  `:` is a separate token after a space, and `::` in head position is the cast
  form. The identifier case glues the `:` to the next character, which may be
  enough to tell them apart. Note that `':::` currently fails as "unbound
  symbol ':'", which suggests the token is being split around a `::`.
- Add a fixture `r7rs-colon-identifiers` pinning every row of the table above
  on both back ends, including `:::` as a custom ellipsis.
- When this closes, SRFI 42 (r7rs-srfi-plan S7) is unblocked.
