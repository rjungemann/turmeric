# `#lang` line: trailing tokens leak into the body

**Status:** RESOLVED -- `detect_lang` now consumes the remainder of the `#lang`
line (`src/compiler/reader.c`) before setting `out_rest`, so trailing tokens no
longer reach the reader. `p` halts at the newline (matching the existing
no-trailing-token path), leaving the terminator in place so body line numbers
are preserved -- the stripped `#lang` line shows as an empty line 1. Regression
fixture: `tests/fixtures/lang-trailing-tokens/`.

**Severity:** low (cosmetic today; a prerequisite for the `#lang` layers plan).

## Summary

`detect_lang` reads only the **first** name token after `#lang` and leaves the
rest of the line in the source stream handed to the reader. Any token after the
base name is not stripped -- it is parsed as a top-level form.

## Minimal repro

```turmeric
#lang turmeric xyzzy
(defn main [] : int
  (println "hi")
  0)
```

```
$ tur run a.tur
a.tur:1:2: error [TUR-E0003]: unbound symbol 'xyzzy'
1 |  xyzzy
  |  ^^^^^
```

The `xyzzy` token from the `#lang` line is lexed as a top-level symbol.
Removing the trailing token (`#lang turmeric`) compiles and runs fine.

## Root cause

`src/compiler/reader.c:4069-4096` (`detect_lang`): after `#lang`, it extracts a
single non-whitespace run as the language name, then sets
`*out_rest = p` / `*out_rest_len = remaining` pointing at the byte immediately
**after the name** -- i.e. the space + any trailing tokens + the newline + the
whole body. It never advances past the remainder of the `#lang` line. The
callers (`detect_and_adjust_lang`, `src/main.c:140`) treat `out_rest` as the
body, so trailing tokens on line 1 become body source.

## Fix directions

After the base name is read, advance `p` to the end of the `#lang` line (to the
`\n`, consuming it) before setting `out_rest`, so nothing on line 1 reaches the
reader. When the `#lang` layers work lands
([docs/upcoming/lang-layers-plan.md](../upcoming/lang-layers-plan.md), phase
L0), those trailing tokens become the layer list and the same EOL-consumption
is what makes the leak impossible; until then, an unknown trailing token should
be either ignored (skip to EOL) or a dedicated error, not silently reinterpreted
as body source.
