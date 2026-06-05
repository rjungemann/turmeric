---
title: `linalg/decomp.tur` has 4 unmatched opens (source bug); parser's EOF diagnostic points at outermost form instead of deepest unclosed paren
category: Reported
severity: low
description: Initial sweep flagged `tur build ../turmeric-spices/spices/linalg/` failing with "unterminated list (missing ')')" anchored at `decomp.tur:185` (`(defn qr [A] : int`) with caret spanning the rest of the file. Investigation: the source genuinely has 4 unmatched `(` across the file (`qr` is missing 2 closes, `lu` is missing 2). The file has only two commits in `turmeric-spices` history and has never built. The compiler is reporting correctly. The remaining concern is purely diagnostic UX: when the reader reaches EOF with unclosed parens, the caret points at the outermost still-open form and spans to EOF, rather than at the deepest unclosed paren -- which would tell the author where to add the missing `)`.
---

# `linalg/decomp.tur:185` -- parser reports unterminated list (source has 4 unmatched opens)

## Resolution status

**Source bug, not a compiler bug.** Reduced from medium to low severity.
Filed to capture the diagnostic-UX finding and the bad-source paper trail.

## Summary

Building `../turmeric-spices/spices/linalg/` against `main` (commit
`c60ba4ca`) produces:

```
./src/linalg/decomp.tur:185:1: error: unterminated list (missing ')')
185 | (defn qr [A] : int
    | ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^... (extends to EOF)
```

Per-form paren accounting confirms the file is genuinely unbalanced:

| Form | Lines | Net `(` -- `)` |
| --- | --- | --- |
| `(defn qr ...)` | 185--282 | +2 (missing 2 closes) |
| `(defn lu ...)` | 97--169 | +2 (missing 2 closes) |
| Whole file | 1--295 | +4 |

The `qr` Q-transform block (lines 259--270) is structurally asymmetric
with the R-transform block (lines 244--256) -- the inner-dotimes
trailing-close count differs by one relative to the deeper nesting
required, and the final `f))))` at line 278 has 4 closes where it needs
6.

History (`git log -- spices/linalg/src/linalg/decomp.tur` in
`../turmeric-spices`):

```
645710a Migrate to spaced type annotations
7366106 Templates; WIP: watch
```

Two commits total; the second is a marker for WIP scaffolding. The file
was added as a draft, mass-edited later by a colon-spacing sweep, and
never actually compiled.

## Compiler-side residue: diagnostic UX

The parser's behavior is correct (refuse the file), but the diagnostic
location is unhelpful:

- **Observed**: the caret anchors at the outermost still-open form
  (`(defn qr` at line 185) and visually extends through every following
  line to EOF.
- **More useful**: anchor at the *deepest still-open* paren -- the last
  `(` that the reader saw without a matching `)`. That position is where
  the missing close belongs and is the actionable hint for the author.
  For this file, the deepest unclosed paren in `qr` is the inner
  `(dotimes [i (- m k)]` at line 261 (Q-transform first inner loop),
  whose body never gets terminated.

A minimal fixture for the diagnostic-UX issue:

```turmeric
;; unclosed-deep.tur
(defn f []
  (let [x 1]
    (let [y 2]
      (+ x y)
  ;; <-- missing one ), then EOF
```

Expected: caret on the unclosed `(let [y 2]`, not on `(defn f []`.

## Investigation method

```sh
# whole-file balance
awk '
{ in_str=0; for(i=1;i<=length($0);i++){
    c=substr($0,i,1)
    if(c=="\"") in_str=!in_str
    if(in_str) continue
    if(c==";") break
    if(c=="(") o++; else if(c==")") cl++
  }
} END{print o, cl, o-cl}' decomp.tur
# => 360 356 4
```

Per-form balance and depth-trace confirmed the missing closes are inside
`qr` and `lu`; `chol`/`chol-free`/`lu-free`/`qr-free` are well-formed in
isolation.

## Recommended action

1. **No compiler change required for correctness.** Optionally, improve
   the "unterminated list" diagnostic to point at the deepest unclosed
   paren; that is a self-contained reader-side improvement worth a few
   lines and earns its keep across every future malformed `.tur` file.
2. **Source fix lives in `turmeric-spices`.** Either delete the WIP
   decompositions or finish balancing them. The
   `docs/upcoming/spices-v0.18-typing-migration-plan.md` plan now
   classifies `linalg` as a source bug and stops blocking the typing-wave
   sweep on it.

## Cross-references

- Originated from
  [`docs/upcoming/spices-v0.18-typing-migration-plan.md`](../upcoming/spices-v0.18-typing-migration-plan.md),
  which initially mis-classified this as a compiler bug. The plan has been
  updated.
