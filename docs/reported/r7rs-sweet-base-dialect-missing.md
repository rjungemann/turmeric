# `#lang r7rs/sweet` does not exist -- Scheme is the one language with no sweet-exp base

**Severity:** low. Nothing is wrong; `#lang r7rs` works and so does every
Turmeric and Saffron sweet-exp base. This is an expressiveness hole: the one
language sweet-expressions were actually designed for is the one that cannot
use them.

Filed 2026-09-27, from the Try Turmeric language picker. The picker now groups
its rows by language (Turmeric / Saffron / Scheme) and offers the same two
readers under each -- plain s-expressions and sweet-exp. Scheme is the odd row
out: it has one.

```
TURMERIC
  #lang turmeric
  #lang turmeric/sweet
SAFFRON
  #lang saffron
  #lang saffron/sweet
SCHEME
  #lang r7rs            [experimental]
  #lang r7rs/sweet      <- wanted; does not exist
```

## Repro

```sh
$ cat /tmp/x.tur
#lang r7rs/sweet

display "hi"

$ ./build/tur run /tmp/x.tur
tur: error [TUR-E0331]: unknown #lang base 'r7rs/sweet' -- see `tur dialects`
     for the valid bases
```

Pinned as intended behavior today by
`tests/fixtures/errors/lang-r7rs-no-reader-axis`, whose comment says the same
thing this report is asking to change. `tur dialects` lists nine bases; eight
of them are the Turmeric x {s-expr, curly-infix, neoteric, sweet} and Saffron
x the same cross-product, and the ninth is `r7rs`.

## Root cause

Deliberate, and recorded as such -- this is a deferral being picked up, not an
oversight being reported.

- `src/compiler/lang_dialects.c:55` -- the `LANG_R7RS` trait row sets
  `reader_axis_free = false`, so the language brings exactly one reader
  (`READER_R7RS`) instead of spanning the four Turmeric ones.
- `src/compiler/lang_dialects.c:77` -- `LANG_BASES[]` therefore has a single
  `{ LANG_R7RS, READER_R7RS }` row, and it is the whole base set: an unlisted
  pair is TUR-E0331.
- `docs/upcoming/r7rs-lang-plan.md:1774` (Section 8, Q5) states the deferral in
  as many words: *"`r7rs/sweet` is arguably meaningful -- sweet-expressions
  were designed for Scheme, and SRFI-110 is a Scheme SRFI. Recorded as a
  deliberate deferral rather than an oversight; if it is ever wanted, the
  `LangTraits` table is the place it goes."*

## Fix directions

The table is the easy half. The reader is the question, because `r7rs/sweet`
is not "the Scheme reader plus indentation" -- it is SRFI-110 over Scheme's
lexical syntax, and today's sweet-exp preprocessor sits above the Turmeric
reader, not the Scheme one.

1. Decide what the pair means. SRFI-110 is specified for Scheme, so the target
   is sweet-exp's three tools (indentation, neoteric `f(x)`, rest-of-line `$`)
   over Scheme tokens -- `#t`, `#\a`, `#(...)`, block comments, the R7RS number
   grammar -- not over Turmeric's.
2. Find out whether the existing sweet-exp pass can be re-pointed at the Scheme
   reader, or whether it is entangled with Turmeric tokenization. That
   measurement decides whether this is a day or a week.
3. Then: add `{ LANG_R7RS, READER_R7RS_SWEET }` to `LANG_BASES[]`, give
   `LangTraits` a way to say "this language spans these readers" that is not
   the boolean `reader_axis_free` (a third answer is now needed -- Turmeric's
   four, Scheme's two, and none), and teach `lang_base_spelling`/
   `lang_base_from_name` the new token.
4. Delete `tests/fixtures/errors/lang-r7rs-no-reader-axis` (or repoint it at a
   base that really is unknown, e.g. `r7rs/neoteric`, if the answer to (1) is
   that Scheme gets sweet and nothing else), and add a positive fixture.
5. The Try Turmeric picker needs no change: it renders whatever
   `turi_wasm_lang_registry` lists, grouped by language, filtered to the
   readers in `LANG_READERS_SHOWN` (`s-expr`, `sweet`, `scheme`) -- so a new
   `r7rs/sweet` row appears under SCHEME on its own.

## Guide upkeep

`docs/guides/r7rs-guide.md` says `#lang r7rs` has no reader axis; that sentence
comes out when this lands.
