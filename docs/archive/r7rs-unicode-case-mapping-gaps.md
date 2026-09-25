# `#lang r7rs`: four gaps in the `(scheme char)` case mapping tables

**RESOLVED 2026-09-25, archived.** The generator reads the Unicode
Character Database files now (`tools/gen-r7rs-unicode.py --ucd`), fetched by
`tools/fetch-ucd.sh` from ICU's copy at one pinned release tag
(`release-76-1`, Unicode 16.0.0; unicode.org is not reachable from every
build box, and the open-i18n mirror stops at 12.0.0). Simple case mappings
come from UnicodeData.txt and CaseFolding.txt's C+S entries, full ones from
SpecialCasing.txt's unconditional entries and CaseFolding.txt's F entries,
`char-alphabetic?` is the Alphabetic property (Other_Alphabetic included),
and `string-downcase` applies Final_Sigma with the Cased and Case_Ignorable
properties. The Unicode version moves only when the tag in the fetch script
does. Pinned by `tests/fixtures/r7rs-unicode-case` (both back ends); the
chibi count is unchanged. Original report follows.

**Severity:** low. Both back ends (the tables are one generated C block
shared by both, `tests/check-r7rs-unicode-sync.sh`). Four documented
differences from Unicode, each a consequence of how
`tools/gen-r7rs-unicode.py` derives its tables from Python's `unicodedata`
rather than from the UCD files. Documented in docs/guides/r7rs-guide.md
and r7rs-lang-plan 9.3; no chibi test reaches any of them (the final-sigma
tests there accept both answers).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme char))
(write (string-downcase "\x39F;\x394;\x3A5;\x3A3;\x3A3;\x395;\x3A5;\x3A3;"))  ; ΟΔΥΣΣΕΥΣ
(newline)
(write (char-upcase #\x1F80))     ; ᾀ
(write (char-upcase #\x1FB3))     ; ᾳ
(newline)
(write (char-alphabetic? #\x0345)) ; COMBINING GREEK YPOGEGRAMMENI
(newline)
```

```
$ tur run case.tur             # and tur --interpret
"οδυσσευσ"                     ; want "οδυσσευς" (final sigma)
#\ᾀ#\ᾳ                         ; want #\ᾈ (U+1F88) and #\ᾼ (U+1FBC)
#f                             ; want #t (Other_Alphabetic)
```

Measured 2026-09-25 against `./build/tur` v0.51.0 (Debug), tables from
Unicode 14.0.0.

## The four gaps

1. **`string-downcase` skips the Final_Sigma context.** SpecialCasing.txt
   maps U+03A3 to U+03C2 when it ends a word. The generator's docstring
   says the tables are "without the context-sensitive final sigma"
   (tools/gen-r7rs-unicode.py:26): full mapping is applied one character
   at a time with `str.upper()` / `str.lower()`, and Python's `str.lower()`
   does apply Final_Sigma over a whole string, but the generator only ever
   calls it on single characters.
2. **A character's simple case mapping is taken from its full mapping, and
   only when that is one character** (gen-r7rs-unicode.py:22-24). Python's
   `unicodedata` has no simple-mapping API, so the generator uses
   `chr(c).upper()` and keeps the result when it is one code point. For
   the Greek letters with ypogegrammeni (U+1F80-U+1FAF, U+1FB3, U+1FC3,
   U+1FF3, ...) the full uppercase is two characters (`ἈΙ`), so the
   simple mapping falls back to the character itself, where
   UnicodeData.txt field 12 gives U+1F88. 75 lowercase letters have a
   multi-character full uppercase (measured with the same `unicodedata`,
   14.0.0); for each of them `char-upcase` answers itself where the UCD
   gives a simple mapping (most are these Greek letters; for U+00DF and
   the ligatures the self-mapping is right). `string-upcase` is right,
   since it uses the full mapping.
3. **`char-alphabetic?` is General Category L* and Nl only**
   (gen-r7rs-unicode.py:28-29). R7RS 6.6 defines it as the Unicode
   Alphabetic property, which also includes Other_Alphabetic
   (DerivedCoreProperties.txt): combining marks such as U+0345, the
   Indic vowel signs and the other Mn/Mc/So characters that carry it.
4. **The Unicode version is whatever Python generated the tables** --
   14.0.0 today. Not a bug, but a drift: a newer Python moves it silently
   on regeneration, and the guide states the version as a fact.

## Fix directions

- Generate from the UCD files instead of `unicodedata`: `UnicodeData.txt`
  (simple mappings, fields 12-14; General Category), `SpecialCasing.txt`
  (full mappings and the Final_Sigma condition), `DerivedCoreProperties.txt`
  (Alphabetic, Uppercase, Lowercase), `PropList.txt` (White_Space). Pin the
  version in the generator, not in the interpreter that runs it, and record
  it in both emitted files as now.
- Final_Sigma is a per-string pass in `string-downcase` / `string-foldcase`
  (foldcase maps to U+03C3 always, per CaseFolding.txt, so only downcase
  changes): U+03A3 becomes U+03C2 when preceded by a cased letter and not
  followed by one, skipping case-ignorable characters on both sides.
- `char-alphabetic?` reads the Alphabetic property table; `char-upcase` /
  `char-downcase` / `char-foldcase` read the simple mappings.
- The sync check (`tests/check-r7rs-unicode-sync.sh`) keeps the two copies
  agreeing; a fixture pins the three answers above.
