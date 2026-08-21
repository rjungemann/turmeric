# logic-programming-guide.md presents an API summary that does not exist

**RESOLVED 2026-08-20** for the named defect (the API Summary). The
narrative sections remain a design sketch and are now labelled as one --
see "What is NOT done" below.

**Severity: medium** (docs) -- the guide's "API Summary" (`choice-point`,
`run`, `run*`, `do-backtrack`, `constraint`, `return`, `(instance Clone ...)`
syntax) misleads users into calling nonexistent forms. Found in the 2026-08-20
docs audit.

## Repro

`grep -rn "choice-point\|do-backtrack" stdlib/ src/` -> no hits;
`(run 1 [x] ...)` from the guide does not elaborate.

## Root cause

The guide predates `stdlib/logic.tur`; its examples are a design sketch over
the real primitives. `cloneable-reset`/`cloneable-shift` exist
(src/compiler/elab_call.c:2328); the miniKanren engine ships as
`stdlib/logic.tur` with `mzero`/`mreturn`/`mplus`/`mbind`/`fresh`/`run-logic`.

## Fix direction

Either implement a thin `choice-point`/`run`/`do-backtrack` macro layer over
`stdlib/logic.tur` + cloneable continuations, or rewrite the guide's examples
onto the `tur/logic` names (and `definstance` instead of `instance`).

## Guides to update when fixed

- docs/guides/logic-programming-guide.md (primary)
- docs/guides/tur-logic-guide.md (cross-link when resolved)


---

## Resolution

Took the report's second option -- rewrite onto the real `tur/logic` names --
rather than building the `choice-point`/`run`/`do-backtrack` macro layer. The
layer is a feature; the guide lying about what ships is a bug, and the bug is
what was worth fixing now.

### API Summary rewritten against `stdlib/logic.tur`

Every form in it is real and fixture-covered: terms (`term-int`, `term-var`,
`term-pair`, `term-nil` + accessors), goals (`lequal`, `succeed`, `fail`,
`conjoined`, `disjoined`, `fresh`), running (`run-logic`, `bt-length`,
`stream-empty?`, `first-state`, `logic-walk`), the stream interface
(`mzero`/`mreturn`/`mplus`/`mbind` -- **not** bare `return`/`bind`), and
substitutions (`subs-empty`, `logic-unify`, `subst-lookup`). The
`(instance Clone ...)` spelling the report flagged is corrected to
`definstance`.

The two worked examples are lifted from `tests/fixtures/logic-query` and
`tests/fixtures/logic-reify`, and were **run as written in the guide**, not
transcribed on faith -- 3, then 10 and 20, exactly as the guide claims.

`tools/check-guide-pairs.py` goes 10 pairs -> **16 pairs, 16 ok**. That check
is a parse-check for AST equality between each `turmeric` block and its
`sweet-exp` twin, so the six new sweet-exp translations are verified
equivalent rather than eyeballed.

### A status note at the top of the guide

The narrative sections are a design sketch over primitives that never got a
surface. The guide now says so before the reader reaches any of it: which
spellings do not ship (`choice-point`, `run`/`run*`, `do-backtrack`,
`constraint`, bare `return`/`bind`), which module does
(`stdlib/logic.tur`), that `cloneable-reset`/`cloneable-shift` underneath are
real, and where to go instead (the API Summary and `tur-logic-guide.md`).

## What is NOT done

The narrative body still *shows* sketch code -- 12 `choice-point`, 6
`do-backtrack`, 8 bare `return`, 7 bare `bind`. Those examples are now
explicitly flagged as non-shipping, so the report's stated harm ("misleads
users into calling nonexistent forms") is guarded, but a reader who skims past
the note and copies a block still gets code that will not elaborate.

Fully retiring that leaves two options, unchanged from the original report:

1. Rewrite the whole narrative onto `tur/logic` -- most of a 570-line guide,
   and the sketch's pedagogy (choice points, cut, constraint propagation) does
   not map one-to-one onto the goal/stream API.
2. Build the macro layer the sketch describes over `stdlib/logic.tur` +
   cloneable continuations, making the guide true instead of rewriting it.

(2) is the better end state -- the sketch reads well and `run`/`run*` is the
ergonomic surface `run-logic` lacks -- but it is a feature, not a docs fix.

## Verification

- `tools/check-guide-pairs.py docs/guides/logic-programming-guide.md` --
  16 pairs found, 16 ok, 0 failed
- Both guide examples executed: `3`, and `10` / `20`
