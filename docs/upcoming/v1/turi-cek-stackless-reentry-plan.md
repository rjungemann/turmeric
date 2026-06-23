---
title: Turi stackless native re-entry (full CEK / driver-CPS) -- Plan
category: Planning
description: Eliminate the last source of unbounded C recursion in the tree-walking interpreter -- a native / inline-C higher-order function that re-applies a closure through turi_call on a live C frame -- by reifying that callback onto the driver work-stack as an explicit resume continuation, the turi analog of tur's heap DK chain. With no synchronous native re-entry left, the eval_depth increment in eval_apply, the depth guard, and the TURI_EVAL_FRAME_BYTES byte-estimate retire: interpreter recursion becomes provably heap-bounded. It also completes turi-interpreter-delimited-control-plan.md (which lands first), letting continuations be captured through native HOFs as well, so turi gets first-class delimited control the same way tur has it.
---

# Turi stackless native re-entry (full CEK / driver-CPS) -- Plan

## Status update -- 2026-06-23 (N3)

**N3 has landed (the unbounded black-box forms; the longjmp shift/Show
receivers are deferred -- see below).** Following N2's finding, the remaining
program-internal C-recursion that scales with program depth is the rest of the
driver's single-operand black-box `default:` forms -- the same shape as
get-field. Confirmed by probe: deep non-tail recursion threaded through a type
ascription (`(:: (f ...) :int)`), an explicit cast, a `(return ...)`, or a
`(set! x ...)` all tripped the `eval_depth` guard at ~hundreds of levels, while
the same recursion through a builtin arg or an ordinary call arg folds fine.

N3 models the whole single-operand family in the driver via one generic
`DK_UNARY` continuation:

- New `DK_UNARY` `DriveKind`: descend the form's single operand on the
  work-stack (non-tail), then apply the form's post-operand logic when the value
  returns.
- `unary_operand(e)` returns the operand sub-expression (NULL for a bare
  `(return)`); `eval_unary_post(env, frame, e, v)` applies the post logic. Both
  cover the **transform** forms (`EX_CAST`, `EX_REINTERPRET`, `EX_ASCRIBE`,
  `EX_RETURN`, `EX_SET`) and the compiler-inserted **transparent** shims
  (`EX_POLY_WRAP`, `EX_FN_TO_FAT`, `EX_POLY_TO_FAT`, `EX_BORROW_IMMUT`,
  `EX_RC_FROM_REF`, `EX_REF`, `EX_EXISTS_PACK`, `EX_UNION_INJECT`).
- The cast/reinterpret/ascribe/return/set post-logic was moved into
  `eval_unary_post` and `eval_expr_impl`'s five cases now call it, so the
  recursive and driver paths share one copy and cannot diverge.
- Safety mirrors N2: any unary form whose operand may perform is non-capturable
  (`ws_has_perform` is conservative for these kinds), so a `DK_UNARY` never
  lands in a captured continuation slice -- no clone/capture interaction.

Result: deep recursion through `::`/cast/return/set is now heap-bounded (was
"recursion limit exceeded"). New `tur_eval_tco` regressions: `asc-sum 80000`
(ascription) and `ret-sum 60000` (return). `bash tests/run.sh` (1780/0),
eval/sandbox/STM ctests, and the `run-turi` baseline failure set (23,
byte-identical) all unchanged.

**Deferred to N3b / N4:** the longjmp-based delimited-control receivers the plan
originally named for N3 -- the abortive shift (`eval_abortive_shift`), the
serial/cloneable shift receivers, the `TsFrame` continuation replay
(`ts_cont_resume`), call/cc-escape, and `Show` dispatch. These apply their
receiver/closure **once** per invocation (bounded per call, not scaling with
program recursion depth), and they are entangled with the longjmp reset-boundary
machinery -- converting them to the work-stack means reconciling that path with
DC's `DK_PROMPT` capture, a large change with little heap-bounding payoff. They
remain synchronous `turi_call` re-entries and so must still be eliminated before
N4 retires the guard, but they are not a source of unbounded recursion today.
The guard stays until that N4 audit is airtight.

## Status update -- 2026-06-23 (N2)

**N2 has landed.** The plan's original N2 target (the inline-C `^fat`/`TUR_APPLY*`
HOF apply, e.g. a native `option-map`) no longer exists in the interpreter:
`option-map` & co. migrated to pure-Turmeric bodies (Track A), and inline-C
function-pointer apply is an unsupported TI7 carve-out under `--interpret`. So
the *current* residual unbounded C-recursion is not native HOF re-entry but the
driver's black-box `default:` forms -- concretely **`EX_GET_FIELD`** (field
accessors like `.value`/`.is-some`/`.v`), which `eval_drive_ex` routed through
`eval_expr`, black-boxing the receiver's whole subtree. Because the canonical
HOF idiom unwraps its result through a field accessor
(`(.value (option-map (some n) callback))`), recursion threaded through that
accessor C-recursed and tripped the `eval_depth` guard at ~hundreds of levels.

N2 moves that recursion onto the heap by modeling `EX_GET_FIELD` in the driver:

- New `DK_GET_FIELD` `DriveKind`: descend the receiver on the work-stack
  (non-tail), then apply the field extraction when its value returns.
- Field-extraction logic (the int64-carrier ABI + TuriStruct cases) factored
  into a shared `get_field_extract(e, sv)` used by both `eval_expr_impl`'s
  recursive `EX_GET_FIELD` and the new `DK_GET_FIELD` continuation.
- `ws_capturable` is left conservative (a get-field whose receiver may perform
  still forces the fiber path), so a `DK_GET_FIELD` never coexists with a
  capturable `DK_PROMPT` slice -- no clone/capture interaction.

Result: the option-map self-recursion probe now runs **heap-bounded at 1e6**
with the guard never tripping (was: "recursion limit exceeded" at ~hundreds of
levels). New regression test in `tests/turi/eval-tco.{tur,sh}` (`fld-sum 99999`,
the `tur_eval_tco` ctest) -- verified to FAIL on the pre-N2 binary and pass now.
`bash tests/run.sh` (1780/0), eval/sandbox/STM ctests, and the `run-turi`
baseline failure set (23, byte-identical) are all unchanged. The `eval_depth`
guard stays (retires in N4); this phase used an ordinary driver continuation,
not the `DK_NATIVE_RESUME` protocol -- get-field is a pure receiver-then-extract
form, no native re-entry involved.

## Status update -- 2026-06-23

**N1 (protocol scaffold) has landed.** The work-stack resume protocol is in
place and the first re-entrant site is converted end to end:

- `DK_NATIVE_RESUME` `DriveKind` added (`src/turi/eval.c`), carrying a
  `NativeResume{resume, state}` -- the structural twin of tur's `DKK_FRAME`.
- The driver gained a `have_apply` request channel at the top of
  `eval_drive_ex`'s loop: a native pushes `DK_NATIVE_RESUME`, sets
  `apply_fn`/`apply_args`, and the driver applies the closure on the work-stack
  (folding a turi-body callback into a fresh `DK_CALL_RET`, leaf natives via the
  existing synchronous `eval_apply`), then calls `resume(...)` to yield the
  native's value. Single-shot for now (`done` always true); N2 extends it to
  loop natives that re-request the next application in the reused slot.
- `tvar/modify` is converted: `EX_TVAR_MODIFY` is now handled directly in the
  driver's descending switch (read old -> request `fn(old)` -> commit + yield
  old in `tvar_modify_resume`), so the user fn no longer runs on a re-entrant C
  frame. `eval_expr_impl` keeps its synchronous `EX_TVAR_MODIFY` for non-driver
  callers (the public `turi_call` contract is untouched).
- **No guard change** (per the phase plan). New regression fixture
  `tests/fixtures/stm-tvar-modify-turi/` exercises the foldable path under the
  interpreter, including a deeply-recursive callback that folds onto the heap
  work-stack. `bash tests/run.sh` (1779/0) and the eval/sandbox/STM ctests are
  green; `bash tests/run-turi.sh` baseline-failure set is unchanged (23, all
  pre-existing list/vec/unique/typed mismatches, none STM).

**Both prerequisite plans have landed and been archived:**

- [turi-cek-frame-reuse-tco-plan.md](../../archive/turi-cek-frame-reuse-tco-plan.md)
  (F1-F5) -- landed; this plan was always the follow-up that retires F5's
  guard.
- [turi-interpreter-delimited-control-plan.md](../../archive/turi-interpreter-delimited-control-plan.md)
  -- **landed 2026-06-14** on the driver work-stack (exactly the substrate this
  plan extends). The five `interp-continuation` fixtures are un-carved.

**SR is in progress: N1 landed (see above); N2-N5 remain.** Residual machinery
this plan still retires (unchanged by N1, which deliberately left the guard in
place):

- `env->eval_depth` increment + check still live in `eval_apply` and
  `eval_expr` (re-grep `eval_depth`; line numbers drift). The guard stays until
  N4's audit proves no program-internal synchronous native re-entry remains.
- `TURI_EVAL_FRAME_BYTES` and `turi_default_max_eval_depth` still live in
  `src/turi/env.c`.
- The work-stack request channel is a driver-local `have_apply` flag rather
  than a `TURI_TAG_APPLY_REQUEST` return tag -- this keeps `eval_apply`'s
  signature untouched and is sufficient for the special-form (`tvar/modify`)
  conversion; N2's inline-C `^fat`/`TUR_APPLY*` path may still motivate the
  return-tag form, since those natives yield from inside `eval_apply`'s leaf
  dispatch rather than from the descending switch.
- The line/symbol references elsewhere in this plan predate N1 and have
  drifted; the phase structure (N1-N5) and the diagnosis stand. Re-grep
  `eval_depth`/`turi_call`/`DK_NATIVE_RESUME` before starting N2.

With DC done, SR is now the **next** interpreter-recursion step rather than a
sequenced follow-up; N5 in particular still describes accurately the residual
"continuation captured *through* a native HOF" case DC carved out.

## Status and scope

After [turi-cek-frame-reuse-tco-plan.md](turi-cek-frame-reuse-tco-plan.md)
(F1-F5, landed) the interpreter has **exactly one** remaining source of
unbounded C-stack recursion:

> A native or inline-C **higher-order function** that re-applies a closure
> *synchronously, through its own live C frame*, via `turi_call` /
> `eval_apply` -> `eval_apply_driven` -> `eval_drive_ex`.

Everything else is already heap-bounded: deep non-tail recursion folds onto the
driver work-stack, tail recursion reuses one work-stack slot (`DK_CALL_RET`), and
neither touches the C stack per level. The depth guard that F5 had to put back on
`eval_apply` (`src/turi/eval.c:4451`, `env->eval_depth >= env->max_eval_depth`)
and the hand-tuned `TURI_EVAL_FRAME_BYTES` byte-per-frame estimate
(`src/turi/env.c:50`) exist **solely** to keep that residual native-re-entry
recursion from SIGSEGV-ing. (Grounded: an `option-map` self-recursion probe
SIGSEGVs at ~10k levels with no guard; F5 made it trip the limit gracefully
instead -- but "gracefully refuse" is not "support".)

This plan removes the recursion itself rather than guarding it. A native that
needs to apply a closure stops calling back on the C stack; it **suspends onto
the driver work-stack** with a resume continuation, the driver applies the
closure on its heap stack, then resumes the native with the result. This is the
direct interpreter analog of how `tur` (compiled) keeps its delimited context on
the heap `DK` chain (`src/runtime/cps_prompt.c`) instead of the C stack.

The payoff is twofold:

1. **The guard retires.** With no synchronous native re-entry left, runtime
   recursion depth no longer maps to C-stack depth at all. `eval_apply`'s
   `eval_depth++/--`, the depth-limit check, and `TURI_EVAL_FRAME_BYTES` all
   become dead: interpreter recursion is provably heap-bounded (turi becomes
   *stronger* than tur here, which still C-recurses for ordinary recursion and
   reifies only delimited-control regions).
2. **It is the substrate that *completes* delimited control.** A work-stack
   that is the sole locus of control is what
   [turi-interpreter-delimited-control-plan.md](turi-interpreter-delimited-control-plan.md)
   (DC) needs for continuations captured *through* a native HOF callback. DC
   ships **first** -- its five target fixtures capture through turi-code control
   (`perform`/`handle`/`shift`), never across a native HOF, so they are tractable
   on today's work-stack and deliver the high-severity crash fixes now. SR then
   *removes DC's residual restriction* ("no capture through a native HOF") by
   putting that callback on the work-stack too. The two share the resume
   protocol; this plan extends DC's continuation representation rather than
   preceding it. See the sequencing note in DC.

This is a large, invasive change -- a real CPS conversion of the re-entrant
native surface -- hence its own plan. The frame-reuse hybrid + guard is a correct
resting point if it stalls.

## How `tur` does it (the model)

The compiled multi-prompt machine (`cps-transform-plan`, `src/runtime/cps_prompt.c`)
represents the live continuation as a **heap linked list** of `DK` nodes, never
the C stack:

```c
typedef enum { DKK_DONE, DKK_FRAME, DKK_PROMPT, DKK_SHIFT, DKK_SHIFT0 } DKKind;
struct DK { DKKind kind; DKFrame fn; intptr_t env; int tag;
            DKBody body; intptr_t body_env; DK *next; };
```

A `DKK_FRAME` node *is* "given the value, here is the rest of the computation"
(`fn`, closed over `env`). `emit_cps.c` CPS-transforms only the functions that
use delimited control (its `uses_base_delimited` / `uses_callcc` /
`uses_serial_dk` predicates gate this), so the C stack is still used for ordinary
calls but is never *captured*.

The interpreter already has the structural analog: `eval_drive`'s `DriveCont`
work-stack (`src/turi/eval.c:3642` `DriveKind`, `:3668` `DriveCont`) is a heap
array of "given the value, continue" frames -- `DK_CALL_RET`, `DK_IF_BRANCH`,
`DK_DO_SEQ`, `DK_LET_BIND/BODY`, etc. The frame-reuse plan already proved tail
chains and non-tail folds live entirely on it. **The gap is that native
re-entry escapes the work-stack back onto the C stack** -- the interpreter has a
CEK driver for turi code, but native HOFs call `turi_call` synchronously and
re-enter `eval_drive_ex` on a fresh C frame. Closing that gap is the whole plan.

## The residual recursion, precisely

Two categories of synchronous re-entry exist today; both must move onto the
work-stack:

1. **Native / inline-C HOFs that apply a closure.** e.g. `option-map`
   (`stdlib/option.tur:135`, `^fat f`, body `r->value = TUR_APPLY1(f, ...)`):
   under `--interpret` the inline-C apply recognizer runs `f` via `eval_apply`
   on a C frame. The same shape covers `vec`/list iteration, fold/reduce, and
   comparator-taking ops. These are the unbounded ones (the recursion depth is a
   program property).

2. **In-evaluator `turi_call` sites.** Confirmed call sites in `eval.c`:
   `:1225` (continuation/shift form receiver), `:1381` (TsFrame context replay),
   `:1670` / `:1700` (shift receiver), `:6183` (STM `tvar-modify`), `:6748`
   (`Show` dispatch), plus the public `turi_call` (`:6569`) and the async fiber /
   catch-unwind thunks (`:193`, `:5611`, `:5657`). Each runs a closure to
   completion on the calling C frame.

Note `turi_call` is **public API** (`eval.h`) used by embedders. The plan keeps a
synchronous `turi_call` for *external* callers (it pumps the driver to
completion); a single embedder-initiated frame is bounded and fine. What must go
stackless is **program-internal** re-entry (category 1, and the internal users of
category 2), because only that scales with program recursion depth.

## Design: native callbacks as work-stack resume continuations

Defunctionalize each re-entrant native into an **enter / resume** pair and add a
driver continuation that schedules the closure application:

1. **New `DriveKind`: `DK_NATIVE_RESUME`.** Carries an opaque native-state
   pointer and a C resume function `TuriValue (*resume)(TuriEnv*, void *state,
   TuriValue applied_result, bool *done, TuriValue *out)`. It sits on the
   work-stack exactly where a `DK_CALL_RET` would, beneath the application it
   requested -- the structural twin of tur's `DKK_FRAME`.

2. **A native yields instead of calling back.** A re-entrant native, instead of
   `turi_call(env, f, args, n)`, returns a *request* to the driver:
   "apply closure `f` to `args`, then resume me (`state`, `resume`) with the
   result." The driver:
   a. pushes `DK_NATIVE_RESUME{state, resume}`,
   b. drives the application of `f` (folding / reusing on the work-stack like any
      call -- so the callback's own recursion is heap-bounded too),
   c. on completion, calls `resume(state, result, &done, &out)`. If the native
      wants to apply again (a loop -- `vec/for-each`, `fold`), `resume` updates
      `state` and asks for the next application (the slot is reused, no growth);
      when `done`, `out` is the native's value and the slot pops.

   This is a small coroutine protocol: the native's C frame no longer spans the
   callback. A `fold` over N elements becomes N work-stack iterations in one
   `DK_NATIVE_RESUME` slot, not N nested C frames.

3. **The driver gains a "native request" return channel.** `eval_apply_driven`'s
   leaf-dispatch path (today `cl->native(...)` returns a value;
   `src/turi/eval.c:~4340`) learns a third outcome besides value/error: a
   `TURI_TAG_APPLY_REQUEST`-tagged value (internal only, never escapes, like the
   retired `TURI_TAG_TCO`) that the *driver* -- not `eval_apply` -- consumes by
   installing the `DK_NATIVE_RESUME` frame. Natives invoked outside a driver
   (rare; only from a raw `turi_call` with no enclosing drive) fall back to the
   synchronous `turi_call` pump.

4. **Convert the re-entrant surface.** Port the inline-C apply path (category 1)
   and the internal category-2 users (`tvar-modify`, `Show`, the shift/context
   receivers) to the enter/resume form. The set is small and enumerable (grep
   `turi_call` / the inline-C `TUR_APPLY*` recognizer); each conversion is local.

5. **Continuations capture through natives too (tie-in).** DC lands first and
   represents a continuation as the work-stack slice from the capture point up to
   the matching `DK_PROMPT`. Once SR puts native callbacks on the work-stack
   (`DK_NATIVE_RESUME`), that slice can also span a suspended native HOF, so DC's
   clone-on-resume logic *extends* to capture-through-a-native -- the one case DC
   carves out until SR. No rework: SR adds a frame kind DC's capture already
   knows how to copy.

## Retiring the guard (the headline payoff)

Once **no** program-internal path re-enters evaluation on the C stack, the
maximum C-stack depth reached during `eval` is bounded by the *static* nesting of
a single form being descended (a deeply nested literal / builtin chain), which is
a parse-time constant, not a function of runtime recursion. Therefore:

- Remove `env->eval_depth++/--` and the limit check from `eval_apply`
  (`src/turi/eval.c:4451`) and from `eval_expr` (`:4453`).
- Delete `TURI_EVAL_FRAME_BYTES`, `turi_default_max_eval_depth`, and the
  `max_eval_depth` machinery in `src/turi/env.c`, or repurpose `max_eval_depth`
  as a *static AST-nesting* safety net (cheap, optional).
- Keep the sandbox **step-fuel** limit (`step_fuel`) -- that bounds *work*, not
  C stack, and is orthogonal.

Gate this retirement hard: it is only sound once an audit proves no synchronous
`turi_call`-from-native remains on a program-reachable path (a grep + a
"poison the C re-entry" assertion during the conversion phases). Until then the
guard stays.

## Implementation phases

Each phase is independently landable and regression-green; the F5 guard is the
safe resting point.

1. **N1 -- protocol scaffold.** Add `DK_NATIVE_RESUME`, the internal
   apply-request tag, and the driver plumbing (push/resume/pop). Convert **one**
   site (`tvar-modify`, `:6183` -- simplest, single application) end to end. No
   guard change. Validate STM fixtures.
2. **N2 -- inline-C HOF apply.** Route the inline-C `^fat`/`TUR_APPLY*` callback
   path through the protocol so `option-map` & co. no longer C-recurse. This is
   where the unbounded recursion moves onto the heap. Validate the `option-map`
   self-recursion probe now runs O(1)-heap-bounded at 1e6 with the guard still
   in place (it just never trips).
3. **N3 -- remaining internal re-entry.** Convert the shift/context receivers
   (`:1225`, `:1670`, `:1700`), the `TsFrame` replay (`:1381`), and `Show`
   (`:6748`). Keep the public `turi_call` synchronous (driver-pump for external
   callers; protocol for internal ones).
4. **N4 -- audit + retire the guard.** Grep/assert no program-internal
   synchronous native re-entry remains; remove the `eval_depth` increment +
   check and `TURI_EVAL_FRAME_BYTES`. Re-run the probe: deep HOF re-entry is now
   heap-bounded with **no** limit error and no crash.
5. **N5 -- extend delimited control through natives.** DC has already shipped
   (continuations as work-stack slices; clone-on-resume; heap lifetime;
   re-established handler/prompt stack) and un-carved the five
   `requires.tur-only` `interp-continuation` fixtures, restricted to capture
   through turi-code control. N5 lifts that restriction: with native callbacks
   now on the work-stack (`DK_NATIVE_RESUME`), DC's capture/clone path extends to
   span a suspended native HOF. Add a fixture that captures a continuation across
   a native HOF (e.g. `shift` inside the callback of a recursive `option-map`)
   and un-carve it.

## Risks and trade-offs

- **Exhaustiveness gates the payoff.** A single missed re-entrant native keeps
  C-recursing, so the guard cannot retire until N4's audit is airtight. Mitigate
  with a debug "C re-entry poison" assert that fires if `eval_apply_driven` is
  entered recursively from within a native on a program path.
- **`turi_call` is public API.** Embedders rely on its synchronous return.
  Preserve that contract via a driver-pump wrapper; only internal callers use the
  yield protocol. The embedder's single re-entry frame is bounded (not program
  recursion) -- acceptable, and matches tur's "C stack for ordinary calls".
- **Per-native conversion cost.** Each re-entrant native becomes a small state
  machine. The set is small and enumerable, but the inline-C apply recognizer is
  the fiddly one (it must thread loop/branch state through `resume`).
- **ucontext fiber path.** Async/effect fibers (`eval.c` `:691+`, `:193`)
  interact with resume; sequence N5 to reconcile the fiber representation with
  work-stack continuations (the delimited-control plan already plans this).
- **Scope.** The largest interpreter change since the trampoline. Slice behind
  the harness (N1-N5); revert to the F5 guard if N2/N3 stall.

## Validation

- **Heap-bounded HOF re-entry (the goal):** the `option-map` self-recursion probe
  (and a `fold`/`vec-for-each` recursive probe) run at 1e6 with O(1) C stack --
  after N2 with the guard never tripping, after N4 with the guard *gone* and no
  crash and no "recursion limit exceeded".
- **TCO unchanged:** the four tail shapes (if/let/do/match) stay O(1); non-tail
  `sum-to 500000` heap-bounded.
- **Effects / continuations:** all `effect-*`, `shift*`, `callcc*`,
  `cloneable-context-*`, `serial-context-*` green; after N5 the five
  `interp-continuation` carve-outs (`fh-multishot-value`, `multishot-*`,
  `effect-capture-k`, `effect-handler-capture-nested`) pass and are un-carved.
- **No regressions:** `bash tests/run-turi.sh` green (baseline failure set
  unchanged), `tools/check_turi_parity.py` 0-gaps, `ctest -R "eval|sandbox"`
  green, `bash tests/run.sh` unchanged (compiled path untouched).
- **Audit (N4):** grep confirms no program-internal synchronous
  `turi_call`-from-native remains; the C-re-entry poison assert never fires
  across the whole suite.

## See also

- [turi-cek-frame-reuse-tco-plan.md](turi-cek-frame-reuse-tco-plan.md) -- F1-F5
  (landed); F5 added the `eval_apply` depth guard this plan retires. The "Risks"
  there note frame reuse does **not** by itself retire the guard -- this is the
  follow-up that does.
- [turi-interpreter-delimited-control-plan.md](turi-interpreter-delimited-control-plan.md)
  -- multishot / escaping / nested continuations. **Lands first** (its hard
  dependency, the explicit-stack evaluator, is already met; SR is not required
  for its target fixtures). SR follows and removes DC's residual "no capture
  through a native HOF" restriction (N5) -- it extends DC's continuation
  representation, it does not precede it.
- [turi-eval-trampoline-plan.md](turi-eval-trampoline-plan.md) -- the
  explicit-stack driver (`eval_drive`) whose `DriveCont` stack is the work-stack
  used here.
- `src/runtime/cps_prompt.c` -- tur's compiled heap `DK` chain; the model this
  plan mirrors in the interpreter ("just as tur does").
- `src/turi/eval.c`: `eval_apply` depth guard (`:4451`), `DriveKind`/`DriveCont`
  (`:3642`/`:3668`), `eval_apply_driven`/`eval_drive_ex` leaf dispatch, the
  `turi_call` re-entry sites (`:1225`/`:1381`/`:1670`/`:1700`/`:6183`/`:6748`).
- `src/turi/env.c:50` -- `TURI_EVAL_FRAME_BYTES` (retired in N4).
