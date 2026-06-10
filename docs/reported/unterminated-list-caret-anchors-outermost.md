---
title: "unterminated list" caret anchors the outermost open form, not the deepest unclosed `(`
category: Defect (diagnostics UX)
severity: low (correctness unaffected; debugging cost is real -- multi-screen carets on real files)
status: open
discovered: 2026-06-09 (resurfaced from archived history entry)
related:
  - docs/reported/resolved-paper-trail.md#linalgdecomptur-unterminated-list-source-bug-not-compiler
  - docs/archive/history/linalg-decomp-qr-parser-unterminated-list.md (archived original)
---

## Summary

When the reader hits EOF with unclosed `(`, the `error: unterminated list
(missing ')')` diagnostic anchors its caret at the **outermost** still-open
form -- which on real files routinely spans hundreds of lines and produces a
multi-screen `^^^^^^...` ribbon that buries the actual problem. Pointing at
the **deepest** unclosed `(` would land the caret near (or on) the actual
typo.

This was noted as "low-priority compiler residue" in
`resolved-paper-trail.md:232-236` when the linalg/decomp.tur breakage was
rediagnosed as a source bug. It was not given its own report at the time, so
it resurfaces every time the diagnostic fires. Refiling per the project's
"never sweep latent defects under the rug" rule.

## Repro

Resurfaced today against `../turmeric-spices/spices/linalg/src/linalg/decomp.tur`
(4 unmatched `(` across `qr` and `lu`, per the archived analysis):

```
$ /Users/rjungemann/Projects/turmeric/build/tur build .
./src/linalg/decomp.tur:185:1: error: unterminated list (missing ')')
185 | (defn qr [A] : int
    | ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^...
            (caret ribbon continues for the rest of the file -- ~3 kB of '^')
186 |   (let [m (mat-rows A)
187 |         n (mat-cols A)]
tur: failed to compile ./src/linalg/decomp.tur to header
```

The caret length corresponds to "line 185 col 1 through EOF," because that's
the span of the outermost unclosed form. The actual extra `(` lives deep
inside `qr`'s body, invisible from this caret.

## Observed vs. expected

- **Observed**: caret pinned to the outermost open form's opening token,
  underlining everything from there to EOF.
- **Expected**: caret pinned to the *innermost* still-open `(` at EOF (i.e.,
  the top of the reader's open-paren stack). A user scanning rightward from
  that caret would find the unclosed sub-form by inspection -- typically a
  `let` / `dotimes` / `when` with one trailing `(` too many.

## Root-cause direction

The reader keeps a stack of open `(` spans as it advances. On EOF with depth
> 0, it currently reports the *bottom* of the stack (the outermost frame).
The fix is to report the *top* of the stack -- one-line change at the EOF
detection site in the reader. Adding a secondary `DIAG_NOTE` pointing at the
outermost frame ("started parsing this form here") would preserve the
existing context for users who want the bird's-eye view.

Files to inspect (path conventions consistent with the rest of
`src/compiler/`):

- `src/compiler/reader.c` -- the open-paren stack and the EOF-with-open-frames
  branch that emits the diagnostic.
- Look for the call site that emits `"unterminated list (missing ')')"` and
  swap the span argument from `stack[0]` to `stack[top]` (or equivalent).

## Proposed fix

1. In the reader's EOF handler, anchor the primary `DIAG_ERROR` on the *top*
   of the open-paren stack.
2. Optionally add `DIAG_NOTE` at the *bottom* of the stack: "started this
   form here" -- so users still see the outer container that ended up
   reaching EOF.
3. (Stretch) If the stack depth at EOF is N, an additional one-line
   `DIAG_HELP` reporting "N unclosed '(' at end of file" preempts the common
   "looks balanced to me" confusion -- the linalg case is N=4.

## Validation

- Add a fixture under `tests/fixtures/reader-unterminated-list-caret/` whose
  `input.tur` opens three nested `(` (e.g. `(defn f [] (let [x (` then EOF).
  Snapshot `expected.stderr` to require the caret on the innermost `(` (col
  of the last `(` on the last non-empty line), not on the outer `(defn`.
- Manually re-run `tur build .` from `../turmeric-spices/spices/linalg/` and
  confirm the caret lands inside `qr`'s body rather than on `(defn qr ...`.
- `bash tests/run.sh` must remain zero-FAIL.

## Why this is a real defect, not a polish item

The archived entry classified the linalg failure as "source bug, not
compiler" -- accurate for the *root cause*, but the *user-visible failure
mode* still routes through this diagnostic. Every future user who introduces
an extra `(` -- in any spice, in any new fixture, in any LSP edit session --
hits a multi-screen caret ribbon and has to count parens by hand. The cost
compounds. Documenting it here so it isn't dropped a second time.
