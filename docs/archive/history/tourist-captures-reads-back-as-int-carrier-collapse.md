---
title: defmodule body elaboration bailed after the FIRST erroring form, hiding every later diagnostic (root cause behind the tourist `swap_reject_test` "only captures-count errors" report)
category: Elaborator error recovery -- module-body diagnostics suppressed after first error
severity: Medium. A `(defmodule ...)` with N independent errors reported only
  the first one, then stopped elaborating the rest of the body. This silently
  hid regressions in any later form -- e.g. the tourist `swap_reject_test`
  negative fixture (every defn is a deliberate swap-rejection probe) emitted 1
  of its ~19 expected TUR-E0001s, so a real type-safety regression at probe
  #2..#19 would pass unnoticed. It also blocked unrelated error fixtures whose
  expected diagnostic lives in a form after an earlier (e.g. import-induced)
  error from ever being reached.
status: RESOLVED
---

## RESOLUTION (2026-06-20)

Root cause was NOT a `Captures` carrier collapse (the original hypothesis in
this report, written before the tourist spice was available locally). The
`captures-count` diagnostic the report quoted -- `expected Captures, got int` --
is the *correct, expected* swap-rejection: it fires fine. The real bug was that
it was the **only** diagnostic emitted, because `defmodule` body elaboration
bailed out of the whole module on the first erroring form.

Fixed with two edits in `src/compiler`:

1. `elab_module.c` (`elab_defmodule`, Pass 2 body loop): on a per-form failure
   (`elab_form` returns NULL), record the error and KEEP GOING rather than
   `return NULL` immediately -- mirroring the top-level driver in
   `elaborate_program` (`elab_toplevel.c:1204`, "keep going to surface more
   diagnostics"). After the loop a module that had any failure still returns
   NULL, so the caller still knows elaboration failed; only successful forms are
   kept in `body[]`.
2. `elab_typeclasses.c` (`elab_method_call`, ~line 4375): guard the field-name
   `strcmp` against a NULL field name. Continuing past an earlier error can
   reach a `.method`/field lookup on an incompletely-elaborated struct (one
   whose owning module hit a failed associated-type / instance binding), whose
   field table has a NULL name -> `strcmp(NULL, ...)` SEGV. The fast path now
   skips NULL-named fields and falls through to the regular dispatch path (which
   emits a clean diagnostic). Mirrors the existing `!def` bail two lines up.

### Verification

- tourist `swap_reject_test.tur`: pre-fix `tur check` emitted 1 diagnostic;
  post-fix it emits all 19 (17 `TUR-E0001` + 2 variadic-rest `url-map!`
  rejections) -- exactly the fixture's documented contract ("MUST produce
  TUR-E0001 mismatches at the marked sites").
- Minimal repro: 3 bad defns inside a `(defmodule ...)` reported 1 error pre-fix,
  3 post-fix (top-level already reported all 3 -- that was the asymmetry).
- Full compiler suite green WITH `../turmeric-spices/` checked out:
  `summary: 1690 passed, 0 failed`.

This same fix also resolves the two `ecs-defsystem-writes-unauthorized` reports
(see their RESOLUTION notes): that error fixture's expected
`TUR-E0003 unbound symbol 'Vel-write-cap'` lives in a `defsystem` body that the
old bail-on-first-error path never reached once an imported `ecs/world` module
errored first; with error recovery the body is elaborated and the expected
diagnostic appears. The NULL-guard prevents the SEGV that recovery would
otherwise hit on the way there.

---

# Original report (hypothesis was incorrect -- kept for the record)

The text below was the initial signature-based guess (carrier collapse) made
before `../turmeric-spices/` was available to inspect. It is wrong: there is no
`Captures` -> `int` collapse; the `captures-count` error is the intended
swap-rejection, and the actual defect was the module-body diagnostic
suppression described in the RESOLUTION above.

## One-line summary (original, incorrect)

The tourist regex API exposes a `Captures` type. On tip-of-main,
`swap_reject_test.tur` was reported failing at `captures-count` with
`expected Captures, got int` -- which was *mis-read as* the value losing its
nominal `Captures` identity. In fact that diagnostic is the correct, expected
rejection; the bug was that no other diagnostic followed it.
