---
status: RESOLVED 2026-08-05 (fix directions 1 and 2, plus the trace; direction 3 remains open by design)
severity: was medium
discovered: 2026-08-05
area: interpreter (work-stack capturability analysis, src/turi/eval.c)
---

# `ws_capturable` still treats forms as black boxes that the driver now drives

> **RESOLVED 2026-08-05.** Directions 1 and 2 landed together with the
> `TURI_TRACE_FIBER_FALLBACK` trace -- see [Resolution](#resolution-2026-08-05)
> at the bottom. Direction 3 (the boundary-carrying forms) remains open by
> design, unchanged from the analysis below. Executing this report also
> uncovered a **compiled-path miscompile** in the very shape the interpreter
> can now run:
> [cps-multishot-nontail-resume-inner-handle-drops-clause-rest](cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md).

## Summary

`ws_capturable` decides which of turi's two effect engines runs a `handle`: the
work-stack driver (a captured continuation is a heap slice cloned per resume --
**multi-shot**) or the ucontext fiber fallback (the continuation *is* the fiber
-- **single-shot**, and a second resume is now a clean error rather than an
`abort()`, see
[turi-multishot-resume-in-while-aborts](../archive/turi-multishot-resume-in-while-aborts.md)).

It is a conservative static analysis, and correctly so: a form the driver
cannot descend is one whose interior `perform` would never see the enclosing
`DK_PROMPT`. But **the driver has grown descents that the analysis was never
told about.** Three of its arms name forms the driver descends today and still
demand they be perform-free, so a `handle` containing one falls to the
single-shot fiber for no live reason.

This is the same defect shape as the `while` one, found by the same comparison
and split out rather than folded in: that fix was about a form with no arm at
all, these are arms that are stale.

## The three stale arms

`ws_capturable`, `src/turi/eval.c`:

```c
case EX_SET:        return !ws_has_perform(e->as.set_.value);
case EX_RETURN:     return !ws_has_perform(e->as.return_.value);
case EX_GET_FIELD:  return !ws_has_perform(e->as.get_field_.struct_expr);
```

All three operands are driven now:

| Form | Driven by | Landed as |
| --- | --- | --- |
| `EX_SET` value | `DK_UNARY` (via `unary_operand`) | SR N3 |
| `EX_RETURN` value | `DK_UNARY` (via `unary_operand`) | SR N3 |
| `EX_GET_FIELD` receiver | `DK_GET_FIELD` | SR N2 |

`unary_operand` also covers `EX_CAST` / `EX_ASCRIBE` / `EX_REINTERPRET` /
`EX_REF` / `EX_BORROW_IMMUT` / `EX_RC_FROM_REF` / `EX_EXISTS_PACK` /
`EX_UNION_INJECT`, none of which `ws_capturable` models at all -- they fall to
its `default: return !ws_has_perform(e)`, so the same relaxation applies to
them by extension.

## Repro

```turmeric
(defeffect Ask [] : int)
(defn main [] : int
  (println (handle
             (let [^mut sum 0]
               (set! sum (+ sum (perform (Ask))))     ;; <- a set! whose VALUE performs
               sum)
             (Ask [] ^multishot k) (+ (resume k 1) (resume k 10))))
  0)
```

```
$ tur --interpret repro.tur
tur: eval: resume: this continuation has already been resumed and its body has finished. ...
```

No loop is involved -- this is not the `while` defect. The compiled path
rejects the same program too, for its own unrelated reason (a `perform` inside
a `set!` value is outside the CPS backend's admissible subset), so there is no
working reference implementation to diff against; the expected answer is
derived below.

## Verified fix

Replacing the three arms with a recursive descent:

```c
case EX_SET:        return ws_capturable(env, frame, e->as.set_.value, depth);
case EX_RETURN:     return ws_capturable(env, frame, e->as.return_.value, depth);
case EX_GET_FIELD:  return ws_capturable(env, frame, e->as.get_field_.struct_expr, depth);
```

was applied and measured on 2026-08-05, then reverted pending this report:

- The repro prints **11**. That is the right answer for a properly cloned
  multi-shot continuation: each resume gets its own copy of the `let` frame, so
  each sees `sum = 0` and computes `0+1` and `0+10` independently. A shared
  frame would give 1 + 11 = 12.
- A capture-inside-a-loop multishot case (`perform` inside a `while` in the
  handle body, `(+ (resume k 1) (resume k 10))`) prints **44**, matching the
  compiled path exactly. That one currently falls back for the same reason --
  its loop body contains `(set! sum (+ sum (perform ...)))`.
- `bash tests/run-turi.sh`: 1756 passed, 0 failed, 705 skipped.
- `ctest -R "turi|eval|repl"`: 26/26.

So this is a three-line change with suite evidence behind it, not a
speculative one. It was not landed as part of the `while` fix because that
change was already load-bearing and this is a separate root cause with its own
repro; landing them together would have made a bisect ambiguous.

The one thing left to check before landing is whether `ws_has_perform` should
follow: it still answers `true` for these operands via its own arms, which is
only used at *black-box* positions, so it is not obviously wrong -- but the two
functions should agree about what the driver descends, and after this change
they would not.

## The rest of the fallback surface, classified

Not all of it is stale. Sorting the remaining rejections by what it would
actually take:

### Fixable, needs new driver work (bounded)

| Rejection | Why it rejects | What it needs |
| --- | --- | --- |
| `EX_MATCH` scrutinee + guards | `eval_match_resolve` evaluates them with `eval_expr` | a `DK_MATCH_SCRUT` phase, then resolve the arm when the value returns -- the same two-phase shape `DK_WHILE` uses |
| `EX_PERFORM` args | the driver's `EX_PERFORM` arm evaluates each arg with `eval_expr` into a stack array | drive the args like `DK_CALL_ARG` does, then dispatch. Note the array is read by the handler during dispatch, so its lifetime needs care |
| `EX_RESUME`'s `k` | the driver's `EX_RESUME` arm evaluates `k` with `eval_expr` (the *value* is already driven, C3) | a phase before `DK_RESUME`, mirroring what C3 did for the value |

A `perform` in any of these positions is unusual, which is presumably why they
were left; they are listed so the surface is enumerated rather than implied.

### Fixable, but blocked on `clone_ws_slice`

`EX_RESET`, `EX_CATCH_UNWIND`, `EX_ATOMICALLY`, `EX_STM`, `EX_ESCAPE`,
`EX_SERIAL_RESET` / `EX_CLONEABLE_RESET`. **The driver already descends every
one of these** (`DK_RESET`, `DK_CATCH_UNWIND`, `DK_ATOMICALLY`, `DK_STM_SEQ`,
`DK_ESCAPE`, `DK_CONT_FOLD`), so at first glance they look like the same stale
relaxation as the three arms above. They are not, and the difference is worth
stating because it is the trap here:

each of those frames carries a **heap boundary in `.aux`** -- a
`TuriResetBoundary`, `TuriCatchBoundary`, `TuriStmTx` -- that is linked onto a
global stack (`g_reset_stack`, `g_catch_stack`, `g_stm_tx`) and `free`d by its
own epilogue. `clone_ws_slice` duplicates `.aux` only for `DK_BUILTIN_ARG`,
`DK_CALL_ARG`, and `DK_MAKE_STRUCT`; every other kind shares the pointer. The
capture path says so out loud -- `default: break; /* aux is a defer mark /
boundary, not owned here */`.

So a continuation captured *across* one of these boundaries and resumed twice
would run two epilogues against one boundary: a double `free`, and a global
stack unlinked twice. Admitting them means teaching `clone_ws_slice` (and the
capture-time re-homing loop next to it) to duplicate and re-link a boundary per
clone, and deciding what a re-entered `atomically` or `catch-unwind` boundary
even means. That is a design question, not a switch arm.

**`catch-unwind` is the most likely of these to be wanted** -- it is the shape
in `tests/fixtures/errors/turi-multishot-resume-past-fiber-body`, and "run this
effectful thing, catching panics" is ordinary code.

### Not fixable by this analysis

| Rejection | Why |
| --- | --- |
| a performing closure passed to a **native** callee | the callback runs under a C frame inside the native function; the driver is not on that stack at all. Genuinely needs the callee to become driver-aware, or to stop being native |
| `EX_INLINE_C` | no interpreter execution to descend |
| `depth <= 0` (budget 64) and `g_wscap_n >= WSCAP_MAX_FNS` (128) | analysis budgets, not language facts. A `perform` more than 64 distinct non-recursive callees deep from its handler falls back. Raising the numbers is cheap; the recursion guard already special-cases the common recursive case |

## Why this matters beyond tidiness

Every one of these silently swaps a multi-shot continuation for a single-shot
one. Before the `while` fix that meant `abort()`; it now means a clean error,
which is a much better failure -- but it is still a program that runs compiled
and refuses interpreted, and the reason is invisible in the source. The
`set!`-value case is the sharpest example: `(set! acc (+ acc (resume k i)))` is
the natural way to write an accumulator, and nothing about it looks like it
should decide which effect engine the enclosing handler gets.

## Fix directions

1. **Land the three-arm relaxation** (verified above), with a fixture asserting
   the `set!`-value repro's 11 and the capture-in-loop 44. Reconcile
   `ws_has_perform`'s arms for the same forms at the same time.
2. **Then the bounded driver work** -- match scrutinee, perform args, resume's
   `k` -- each independently landable with its own fixture.
3. **Then, if wanted, the boundary-carrying forms**, starting with
   `catch-unwind`, which needs `clone_ws_slice` to own boundaries first.

A cheap safeguard to consider alongside any of these: an opt-in trace
(`TURI_TRACE_FIBER_FALLBACK=1`) naming the form that forced a handle onto the
fiber. The analysis is a whole-subtree scan whose answer is invisible from
source, and every future gap in it will present as this same silent
downgrade.

## Resolution (2026-08-05)

Directions 1 and 2 landed, plus the trace. Direction 3 is untouched and its
analysis above (the boundary-carrying frames, `clone_ws_slice` ownership)
stands as the design note for whoever takes it.

### Direction 1, wider than drafted

The three stale arms now recurse, and so does the **whole `unary_operand`
family** -- `EX_CAST` / `EX_REINTERPRET` / `EX_ASCRIBE` / `EX_BORROW_IMMUT` /
`EX_RC_FROM_REF` / `EX_REF` / `EX_EXISTS_PACK` / `EX_UNION_INJECT` previously
fell to the black-box default despite `DK_UNARY` driving every one of them.
The `ws_has_perform` reconciliation resolved the report's open question:
that scan answers a different question ("is a perform in here at all", used at
genuinely black-box positions) and its existing arms were already precise;
what it needed was matching arms for the transparent unary forms so a
perform-free `(:: v T)` at a black-box position stops reading as "may
perform" via its default.

### Direction 2, all three

- **Match scrutinee** (`DK_MATCH_SCRUT`): `eval_match_resolve` split into
  resolve-with-value; a scrutinee that may perform is driven and the arm is
  selected on the value's return, including the F1 tail leak. Gated on
  `ws_has_perform(scrutinee)` so the common perform-free match keeps the
  synchronous path with no extra frame. Guards still run via `eval_expr` and
  still must be perform-free.
- **Perform args** (`DK_PERFORM_ARG`): args accumulate on the work-stack when
  any may perform, then hand back to the descending `EX_PERFORM` arm through a
  *driver-local* side channel (`pargs_for`/`pargs_heap` -- locals, not
  globals, so debugger-hook or native re-entry cannot clobber a handoff), and
  dispatch proceeds through the existing code untouched. The values are
  copied into the C-stack `pargs` array before dispatch because the fiber
  path swapcontexts away with the handler still reading them. The
  accumulator joined `clone_ws_slice`'s and the capture re-homing's
  duplication sets, like `DK_CALL_ARG`'s.
- **Resume's `k`** (`DK_RESUME_K`): driven, validated on return, then the
  frame is repurposed in place as the `DK_RESUME` that drives the value --
  the C3 machinery from there on.

Measured, interpreter vs. hand evaluation (and vs. compiled where the
compiled path is correct): set!-value repro **11**, accumulator-in-loop
**44**, match-scrutinee-through-a-call **211**, perform-arg under nested
handlers **22**, weighted **2020**. All pinned in
`tests/fixtures/turi-ws-driven-operands` (`requires.interp-only`: run.sh's
`tur run` path is the JIT -- a compiling pipeline -- not the tree-walker).

### The part nobody asked for

Comparing the two backends on the newly-runnable shapes found the compiled
path **miscompiling** one of them: a non-tail multishot resume whose
continuation re-enters an inner handle delivers the first resume's value as
the outer handle's value and drops the rest of the clause (2 where the answer
is 22; single resume fine, no-inner-handle fine). Filed with a boundary table
as
[cps-multishot-nontail-resume-inner-handle-drops-clause-rest](cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md).
The interpreter is ahead of the compiler on these shapes now, which reverses
the usual direction of that comparison.

### The trace

`TURI_TRACE_FIBER_FALLBACK=1` prints, at each fiber fallback, the handle's
span and the span + kind of the deepest form `ws_capturable` rejected (or a
note when the rejection was a depth/fn budget). Implemented as a recording
wrapper -- recursion unwinds bottom-up, so the first `false` seen is the
deepest offender. The single-shot resume error now names the trace variable,
and its list of non-descendable forms was corrected (match *scrutinee* left
it; match *guards*, boundaries, and native HOFs remain).
