---
title: An unknown `#lang` base reports "#lang unknown is not yet implemented", naming neither the token the user wrote nor the valid options
category: Archive
description: `#lang saffron/bogus` (or `#lang foo`) resolves to READER_UNKNOWN, and the error prints reader_type_name of that enum -- the literal word "unknown". The user's token is never echoed and no valid base is suggested, on the one diagnostic most likely to be hit by someone learning the directive.
---

# The unknown-`#lang` error names neither the typo nor the alternatives

**RESOLVED 2026-09-09** via fix directions 1 and 3 together. `detect_lang_dialect`
now threads the base token out through the same `out_bad`/`out_bad_len` pair
the unknown-LAYER path already had, and returns `READER_UNKNOWN` for a base it
does not recognise. Every report site (`tur build`/`run` in `main.c`, the
module loader, the top-level elaborator, and the interpreter) then prints

```
unknown #lang base 'saffron/bogus' -- see `tur lang-layers` for the valid bases
```

as `TUR-E0331`, beside the existing `TUR-E0330` layer wording. "not yet
implemented" is kept for the case it actually describes: a base that resolves
to a known reader with no implementation. Direction 2 (Levenshtein near-miss
suggestions) was not done; the `tur lang-layers` pointer covers it for a list
this short.

`tests/fixtures/errors/saffron-unknown-dialect`, `errors/lang-unknown` and
`errors/lang-not-implemented` now assert the base wording (all three were
unknown bases, not unimplemented ones -- the report predicted exactly that).
A layer-typo control still prints `unknown #lang layer 'bogus-layer'`.
Compiled 2908/0, interpreted 2000/0.

---

**Severity: low (diagnostic quality), but badly placed.** This is the error a
reader meets when they mistype the very first line of a file, and it tells them
nothing they did not already know.

Found while wiring the language axis
([saffron-lang-plan](../upcoming/saffron-lang-plan.md) S1), which adds four new
base spellings and so makes a near-miss more likely -- but the defect is
pre-existing and applies equally to `#lang foo`.

## Repro (2026-09-07)

```turmeric
#lang saffron/bogus
(println "hello")
```

```
$ tur run p.tur
tur: error: #lang unknown is not yet implemented
```

The word "unknown" there is not a description of the problem -- it is
`reader_type_name(READER_UNKNOWN)`, the enum's own name, printed as though it
were the user's token.

## Root cause

`lang_base_from_name` returns `(ReaderType)-1` for a base it does not
recognise, and the caller reports it through the not-implemented path:

```c
/* src/main.c, detect_and_adjust_lang */
if (!reader_type_is_implemented(detected_type)) {
    fprintf(stderr, "tur: error: #lang %s is not yet implemented\n",
            reader_type_name(detected_type));
```

Two separate conditions share one message: a base that does not exist, and a
base that exists but is not implemented. The second is what the wording
describes; the first is what almost always happens.

The base token is not available at the report site -- `detect_lang_dialect`
already has an `out_bad`/`out_bad_len` pair for an unknown *layer* token, but
nothing analogous for the base -- which is why this is a report rather than a
one-line fix.

## Fix directions

1. **Echo the token, and separate the two conditions.** Give
   `detect_lang_dialect` an out-param for the base token (the layer path already
   has the shape to copy), then report an unrecognised base as
   `unknown #lang base 'saffron/bogus'` and keep "not yet implemented" for a
   base that genuinely resolves but has no reader.
2. **Suggest the near misses.** The set of valid bases is small, enumerable
   (`lang_dialects_print` renders it already), and a Levenshtein-1 pass over it
   would catch `saffron/sweat`, `turmeric/nepteric` and the like. Worth doing
   only alongside (1) -- the token has to be in hand first.
3. **Point at `tur lang-layers`**, which lists every valid base and layer. One
   line, and useful even without (1) or (2).

`tests/fixtures/errors/saffron-unknown-dialect` and the older
`errors/lang-unknown` / `errors/lang-not-implemented` all assert only the
substring "not yet implemented", so they will need updating with the fix --
which is itself a sign the message is carrying two meanings.
