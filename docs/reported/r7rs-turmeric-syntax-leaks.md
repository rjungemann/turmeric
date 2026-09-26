# `#lang r7rs`: Turmeric syntax and names leak into Scheme source

**Severity:** medium. A `#lang r7rs` program is supposed to reach Turmeric
through one door, `(import (turmeric <module>))`. In practice, much of
Turmeric's own surface is also live in Scheme source:

- its reader extensions;
- its special forms;
- every name in the ~34 auto-loaded stdlib files, with no import at all.

Each piece is harmless while it goes unused. In use, it teaches newcomers
things that are not Scheme, and it is not portable to any other R7RS. It also
takes names and lexical space that R7RS gives the program: `box` (which SRFI
111 defines differently), `^x`, `true`, `[` `]`.

**The rule this report works toward** (the project owner's, 2026-09-26): R7RS
can import Turmeric libraries, but Turmeric features should not leak into
R7RS where that can be avoided. Where a Turmeric capability is needed from
Scheme, the sanctioned route is a Turmeric module imported through
`(turmeric ...)`, not Turmeric syntax in the Scheme file.

**Exempt by design:** the *Turmeric-shaped* Scheme sources, which are the
prelude, the on-demand library files under `stdlib/r7rs/`, and the REPL's
pinned preload. They are the dialect's implementation and are written in
Turmeric's shapes on purpose (their headers say so).
`scheme_lower.c`'s `prelude_span` draws that line, and every fix below keeps
it.

**Where this came from.** r7rs-lang-plan R1 kept these on purpose:
"keywords, `[...]`, `#map{...}`, inline C and `^tailcall` all still read,
and `true`/`false`/`nil` are still literals" (docs/upcoming/r7rs-lang-plan.md,
the R1 "What shipped" note). R9 added Turmeric forms typed at an r7rs REPL
prompt. This report reverses R1's choice for *user* Scheme source.

## Already resolved

- **Keywords.** A leading `:` is now an R7RS identifier in user Scheme
  source, resolved 2026-09-26: docs/archive/r7rs-leading-colon-identifiers.md.
  A map key passed through the seam is written as the symbol `'k`, which is
  the runtime value of the keyword `:k`. `#map{...}` keys still read as
  keywords, because they are inside a Turmeric literal (item 2 below).

## Repro

Measured 2026-09-26 on the branch that resolved the keyword item, Debug
build. Each probe is one program after `#lang r7rs` and
`(import (scheme base) (scheme write))`. Results are the same under
`tur run` and `tur --interpret` except where noted.

| # | Leak | Probe | Result today | R7RS says |
|---|---|---|---|---|
| 1 | `[...]` is a Turmeric vector | `(write [1 2 3])` | `#(1 2 3)` | `[` and `]` are reserved (7.1.1) |
| 1 | | `(write (let ([x 1]) x))` | error: binding must be (name init) | reserved; Racket, Chez and Guile read `[...]` as parentheses, and tutorials use it |
| 2 | Turmeric `#` literals | `(write #map{:a 1})`, `(write #set{1 2})` | `#<Map>`, `#<Set>` | no such syntax |
| 2 | | `#rat{1 2}`, `#cx{1 2}` | Turmeric diagnostics (TUR-E0284; unknown `complex/of`) | no such syntax (R7RS has `1/2` and `1+2i`, which work) |
| 2 | | `(write #?(:tur 1 :default 2))` | `#<nil>` | no such syntax |
| 3 | Inline C | `(defn g [] : int` + a c fence `return 42;` + `)` then `(write (g))` | `42` | -- |
| 4 | `^` metadata | `(if (= n 0) 'done ^tailcall (f (- n 1)))` | `done` | `^tailcall` is an identifier (`^` is an `<initial>`); every call is already a proper tail call |
| 5 | `true`/`false`/`nil` are literals | `(write (list true false))`, `(write nil)` | `(#t #f)`, `#<nil>` | unbound identifiers (quoted they are symbols already: R10) |
| 6 | `@` deref | `(define b (box 1)) (write @b)` | `#<ptr>` | `@` cannot start an identifier; `box` is unbound |
| 7 | Turmeric special forms | `(defn f [x : int] : int (* x 2)) (write (f 21))` | `42` | unbound `defn` |
| 7 | | `(write ((fn [x] (* x 2)) 21))`, `(write (match 3 3 'three _ 'other))`, `(write (:: 5 int))` | `42`, `three`, `5` | unbound |
| 7 | Turmeric macros | `(write (-> 5 (+ 1)))` | a Turmeric macro's diagnostic ("-> expected symbol or list") | unbound `->` |
| 8 | Auto-loaded stdlib names, no import | `(println "hi")`, `(write (vec-len (vec-new)))`, `(write (unwrap-or (some 5) 0))` | `hi`, `0`, `5` | unbound: a program sees only what it imports or defines (5.1, 5.2) |
| 9 | Back ends disagree on an unknown name | `(write (str-concat "a" "b"))` (not auto-loaded) | compiled: error, unknown function; `--interpret`: warning TUR-W0040 "will runtime-dispatch", then `"ab"` | an error on both |
| 10 | Turmeric forms at the REPL | a Turmeric form typed at a `tur repl --lang r7rs` prompt is evaluated as Turmeric (R9, deliberate) | works | -- |

Curly-infix (`{1 + 2}` reads as `(+ 1 2)`) is **not** on this list. It is
SRFI 105, a Scheme SRFI, and R7RS reserves `{` `}` for extensions like it.
r7rs-srfi-plan lists it as built in.

## Root cause

One reader and one lowering serve both languages, and the Scheme variant is a
flag on the Turmeric one.

- **Reader** (src/compiler/reader.c, `scheme_enabled`): the Scheme lexemes are
  added on top, and every Turmeric lexeme that Scheme does not *contradict*
  stays. The Reader struct's own comment says so: "Everything the Turmeric
  reader has that Scheme does not contradict stays available (keywords,
  `[...]`, `#map{...}`, inline C, `^tailcall`)". That covers items 1-6.
- **Lowering** (src/compiler/scheme_lower.c): a form whose head is a Turmeric
  special form and not a Scheme syntax name passes through "untouched"
  (`turmeric_form`, `tur_name_is_reserved_special_form`), which is item 7.
  Auto-loaded stdlib names are global in the unit, and
  `(import (turmeric stdlib/<autoloaded>))` is a no-op
  (`stdlib_autoloaded`). The rename table only redirects the R7RS names, so
  any other free identifier falls through to whatever Turmeric has by that
  name, which is item 8.
- **Interpreter**: an unknown name is resolved at run time (TUR-W0040) instead
  of being refused, so a name that is not imported still resolves if the
  interpreter has a native for it (item 9).

## What depends on today's behaviour

Found by grep over every `#lang r7rs` / `.scm` fixture; each needs a look
when its item is fixed:

- `r7rs-reader-lexemes`: pins R1's list, and is written as a Turmeric
  `(defn main [] : int ...)` using `println`, `[...]` and `#map{}`.
- `r7rs-elaborates-as-saffron`: R1's exit criterion, "a `#lang r7rs` file
  elaborates as the equivalent `#lang saffron` file does". Its program is
  Turmeric-shaped by construction, so item 7 retires it or rewrites it as a
  Scheme program with a Saffron twin.
- `r7rs-reader-forms` (a parse-check pair of Scheme against Turmeric
  spelling).
- `errors/lang-r7rs-no-reader-axis`, `errors/r7rs-reader-dotted-malformed`:
  their bodies are `(defn main [] : int ...)`.
- `r7rs-gc-seam`, `r7rs-keyword-seed`: `#map{...}`. The keyword-seed
  fixture's point is a keyword record and the symbol seeder in one unit, so it
  needs its keyword from an imported Turmeric module instead.
- `r7rs-stdlib-seam`, `r7rs-bignum-int-seam`, `r7rs-import-sets`,
  `r7rs-gc-seam`: Turmeric stdlib calls. Most already import the module
  through `(turmeric ...)`; item 8 makes that import required.
- `tests/run-r7rs-import.sh`, `tests/r7rs/run-conformance.py`'s harness, the
  LSP r7rs diagnostics test, `run-fmt.sh`, `run-editor-syntax.sh`: to check.
- The guide's `(turmeric ...)` example imports `stdlib/vec`, a no-op today;
  it becomes the import that grants `vec-new`.

## Fix directions

In rough order of value and independence:

1. **Item 8, the namespace.** In user Scheme source, a free identifier
   resolves only to:
   - a `(scheme ...)` name (or an `(srfi N)` name, per r7rs-srfi-plan);
   - the program's own definitions;
   - a name imported through `(turmeric ...)`.

   `(import (turmeric stdlib/vec))` stops being a no-op and becomes the
   import that grants the names. Anything else is "unbound variable", with a
   help line naming the `(turmeric ...)` import that would provide it (the
   lowering knows which auto-loaded file defines it). This is the largest
   item and the most valuable, because it also frees `box` for SRFI 111.
2. **Item 9** falls out of 8 for the interpreter: an unbound Scheme variable
   is refused at elaboration, as on the compiled back end.
3. **Items 2, 3, 6** (`#` literals, inline C, `@`): the Scheme reader refuses
   them in user source, with a message that points at the seam: "Turmeric's
   `#map{...}` is not Scheme syntax; build the map with `map-assoc` from
   `(turmeric stdlib/map)`", and "inline C belongs in a Turmeric module; import
   it with `(turmeric ...)`".
4. **Items 4, 5**: `^x`, `true`, `false` and `nil` become ordinary identifiers
   in user source. The R10 `PROV_SCHEME_WORD` stamp already knows which words
   the Scheme reader made.
5. **Item 7**: in user Scheme source, a Turmeric special-form name is an
   ordinary identifier (unbound unless the program defines it). The
   `turmeric_form` pass-through stays for Turmeric-shaped sources only.
6. **Item 1 needs a decision**: refuse `[...]` ("R7RS reserves `[` `]`"), or
   read it as parentheses as Racket, Chez and Guile do. Reading it as
   parentheses matches the Racket-flavoured tutorials a newcomer will paste,
   and takes nothing from Turmeric, so that is the recommendation. Either
   way, not as a Turmeric vector.
7. **Item 10 needs a decision**: keep Turmeric forms at an r7rs REPL prompt as
   a convenience, or require switching with `#lang turmeric` at the prompt
   (which the REPL already supports for `#lang r7rs`). The rule above points
   to the switch.

Each step changes what a Scheme program means, as r7rs-lang-plan's Section 9
tasks did, and should land the same way: fixtures on both back ends, and the
chibi conformance count must not drop.

## Guide upkeep

docs/guides/r7rs-guide.md does not list any of this under "Where it differs
from R7RS", because it treats the leaks as extensions. Until each item is
fixed, add a bullet there: "Turmeric syntax and stdlib names are visible in a
Scheme file; do not rely on them, they are being removed." Link it here, and
trim the bullet as items close.
