---
title: Auto-desugar the shift/reset surface onto cross-function resume
category: Planning
status: landed (slices A/B/C + integration, single- AND multi-shot receivers, inline-lambda AND named-fn receivers)
description: Cross-function resumable continuations already WORK via the effect surface (perform/handle/resume/(k v)); blockers 1 (unified resume surface) and 3 (capturing fn payloads) landed, and the reset->handle + shift->perform mechanism is verified end to end. This plan is the remaining piece: automatically desugar a cross-function resuming `shift`/`reset` onto that machinery so users write the ordinary shift/reset surface. Split from cps-backend-n6-resuming-shift-plan.md; full analysis in docs/reported/cross-function-resume-design.md.
---

# Auto-desugar: shift/reset cross-function resume

## The story, in one sentence

A user writes a resuming `shift` whose `reset` is in a *caller* (cross-function),
and it Just Works -- lowering silently onto the effect machine that already
handles cross-function resumable continuations -- instead of raising `TUR-E0016`.

```turmeric
(defn ck [k : cont] : int (+ (k 1) (k 2)))
(defn inner [] : int (shift ck 0))            ; shift in a callee, resumes k
(defn outer [] : int (reset (+ 10 (inner))))  ; reset in the caller
;; today: TUR-E0016.  goal: outer = (10+1) + (10+2) = 23, direct == cps == turi.
```

## What is already done (do not redo)

- **Blocker 1 -- unified resume surface.** `(k v)` works on an effect handler
  continuation (`is_continuation` binding -> `EX_RESUME` via `elab_make_resume`).
  Commit `da478f3`; fixture `handler-cont-kv-sugar`.
- **Blocker 3 -- capturing fn payloads.** An effect can carry a *capturing*
  closure receiver as a **boxed** fn payload (one-word `void *` to a heap
  `{thunk, env}` box); `defeffect` marks a `TY_FN` param `boxed`, `elab_perform`
  boxes a non-capturing fn arg via `EX_FN_TO_FAT`. Commit `4c5d1de`; fixtures
  `effect-fn-payload`, `effect-fn-payload-capturing`.
- **Mechanism verified.** A hand-written `__Shift`-style desugar runs
  `direct == turi == 15`; the capturing single-resume shape is pinned by fixture
  `cross-function-resume-via-effect` (`= 11`). Commit `dbb45a3`.

So this plan adds **no new runtime capability** -- only the compiler-generated
desugar of the `shift`/`reset` *surface* onto the working effect encoding.

## Target encoding (what the desugar produces)

```
;; once per program (synthetic, hidden):
(defeffect __Shift [recv : (fn [int] int)] : int)

;; a cross-function resuming shift:
(shift RECV BODY)      ->   (perform (__Shift RECV'))       ; RECV' resumes via effect resume

;; a reset that may (transitively) catch one:
(reset BODY)           ->   <existing reset lowering> (handle BODY
                              (__Shift [recv] k) (recv k))
```

`(recv k)` applies the carried receiver to the delimited continuation `k` (an
`is_continuation` binding, so its `(k v)` inside `RECV'` resumes -- blocker 1). A
lexical abortive/reified shift is `EX_SHIFT` / `EX_CLONEABLE_SHIFT`, not a
`perform`, so it bypasses the handler and reaches the outer reset unchanged --
only a cross-function resuming shift performs `__Shift`.

## What landed

Slices A, B, C and the integration fixture are implemented; cross-function
resume works on the plain `shift`/`reset` surface for single- AND multi-shot
receivers (including the opening example = 23), `direct == cps == turi`:

- **Slice A** -- `CONT_EFFECT` continuation flavor (`effect-cont`), `(k v)` ->
  `EX_RESUME` (`types.h`, `elab_call.c`). Fixture `effect-cont-kv-sugar`.
- **Slice B** -- a resuming `shift` with an inline-lambda `cont` receiver and no
  lexical reset desugars to `(perform (__Shift RECV))`; the receiver's `cont`
  param is reflavored to `multishot-effect-cont` *before* elaboration (in
  `elab_shift`) so it is elaborated once (no dead cloneable copy). `__Shift` is
  registered lazily (`elab_effects.c`).
- **Slice C** -- reset nodes are recorded at elaboration time; a gated
  post-elaboration pass (`elab_wrap_resets_for_crossfn_resume`) wraps each in a
  `^multishot` `__Shift` handler only when `uses_crossfn_resume` is set --
  non-using programs are byte-for-byte unchanged (full suite green). A resuming
  shift with no reset anywhere is a compile error (TUR-E0016).
- **Multi-shot** -- the receiver's `k` is `multishot-effect-cont` (CONT_EFFECT +
  CK_MULTISHOT: `(k v)` snapshots before each resume) and the synthesized handler
  is `^multishot`, so a receiver may resume `k` any number of times
  (`(+ (k 1) (k 2))` = 23; a triple-resume = 306). `expr_has_multishot_handler`
  (`emit_core.c`) gained `EX_RESET`/`EX_CLONEABLE_RESET` cases so the cloneable-
  cont preamble is emitted for the synthesized handler inside a reset body.
  Single-resume behaves identically (one snapshot, one resume). The one-shot
  `effect-cont` flavor stays available (fixture `effect-cont-kv-sugar`);
  `multishot-effect-cont` is pinned by `multishot-effect-cont-kv-sugar`.
- **Named-fn receivers** -- a resuming receiver written as a top-level `defn`
  works too. A plain-`cont` named receiver is re-elaborated from its retained
  source form (`defn_form`) into a `multishot-effect-cont` specialization
  `NAME$xfn` (via `elab_defn` + `elab_register_file_def`, reusing the bare-fat
  monomorphization path); the shift is redirected to that clone. A named receiver
  the user already annotated `multishot-effect-cont` is accepted directly (flavor
  read from the receiver's own param binding). The same `cont`-param fn can be
  used BOTH lexically (keeps the cloneable path) AND cross-function (gets the
  effect specialization) -- the two copies coexist. So the plan's opening example
  with a named `ck` now Just Works, verbatim. Fixture
  `shift-crossfn-resume-named-fn`.
- **Integration** -- fixture `shift-crossfn-resume-works` (non-capturing +
  capturing single-resume, plus double- and triple-resume lambda receivers) and
  `shift-crossfn-resume-named-fn` (named receivers, incl. lexical+cross-function
  coexistence). `errors/shift-crossfn-resume` now pins the genuinely-unhandled
  case: a resuming shift with no reset anywhere is a TUR-E0016 compile error.

**Limitation (a single reset cannot serve both roles).** A `reset` that reifies a
*lexical* resuming shift (an `EX_CLONEABLE_RESET`) cannot *also* catch a
*cross-function* `__Shift`: the reified path walks the reset body under the narrow
`build_cloneable` grammar, which a `(handle ...)` wrapper falls outside of. The
two lowerings are fundamentally incompatible in one delimiter, so the
whole-program pass wraps only plain `EX_RESET` nodes. This is well-behaved in
every case:

- **Distinct resets nest cleanly** -- a reified reset inside a plain reset (or
  vice-versa) works; the plain one catches the cross-function `__Shift`, the
  reified one is left intact (fixture `shift-crossfn-resume-nested-reset`).
- **Forcing both roles onto one reset is rejected at compile time.** Most such
  shapes hit `build_cloneable`'s `TUR-E0710` directly (the cross-function call
  sits in the reified context, which the grammar refuses). The one shape that
  slips past E0710 -- a cross-function `__Shift` performed from inside the lexical
  shift's *receiver*, with a trivial reified context -- is caught by the
  handler-installing-capacity check: if the program performs `__Shift` but has no
  plain (wrappable) reset anywhere, it is a `TUR-E0016` compile error rather than
  a runtime "Unhandled effect: __Shift" (fixture
  `errors/shift-crossfn-resume-cloneable-reset-only`).

So no program silently misbehaves: the incompatible combination is either
expressed as two distinct resets (works) or rejected up front.

## Slices (ordered; each independently testable)

### Slice A -- the receiver-convention crux: `CONT_EFFECT` flavor  [do first]

The user's receiver is `(fn [k : cont] (k v))`, whose `(k v)` lowers to
**cloneable** resume (`ContFlavor CONT_CLONEABLE`). Cross-function needs **effect**
resume. Resolve by adding a continuation flavor whose `(k v)` lowers to
`EX_RESUME`, consistent with the existing `CONT_CLONEABLE` / `CONT_SERIAL` /
`CONT_ESCAPE` flavors.

- Add `CONT_EFFECT` to `ContFlavor` (`types.h`).
- In the `(k v)` sugar (`elab_call.c`, CC4, `TY_CONT` branch): for `CONT_EFFECT`,
  produce `EX_RESUME` (reuse `elab_make_resume`) instead of a cloneable-resume
  builtin.
- Spell it as a type the desugar can attach to the receiver's `k` param (e.g.
  reuse the flavored-cont annotation path in `elab_fns.c` that already handles
  `serial-cont` / `escape-cont`).

**Done when:** a receiver `(fn [k : <effect-cont>] (k v))` resumes an effect
continuation via `(k v)` (a hand-written `handle`/`perform`, no `shift` yet),
`direct == cps == turi`, and existing cloneable/serial/escape `(k v)` are
unchanged (full suite green).

*Rejected alternatives (see design doc): a Form-level `(k v)`->`(resume k v)`
rewrite (fiddly: identify continuation applications under shadowing); a runtime
cloneable-cont bridge; runtime-polymorphic `(k v)` dispatch. `CONT_EFFECT` is the
cleanest and matches the existing flavor design.*

### Slice B -- synthesize `__Shift` and desugar `shift` -> `perform`

At the site that raises `TUR-E0016` today (a resuming shift with no lexical
reset, `elab_effects.c` `elab_cont_shift_core`):

- Lazily declare a program-level synthetic effect `__Shift` (once) whose param is
  the receiver `(fn [<effect-cont-carrier>] ResetT)`, carried as a boxed fn
  payload (blocker 3).
- Elaborate the receiver so its continuation param is the `CONT_EFFECT` flavor
  (slice A), so `(k v)` inside it resumes the effect continuation.
- Emit `(perform (__Shift receiver))` instead of the `TUR-E0016` error. Set a
  program flag "uses cross-function resume" (drives slice C).

**Done when:** a cross-function resuming shift elaborates to a perform (dump/
inspect), and errors only if the effect payload is unrepresentable. Not yet
runnable end to end without slice C.

### Slice C -- gated `reset` -> `__Shift`-handler wrapping (whole-program)

Every `reset` must install a `__Shift` handler around its body so a callee's
`(perform __Shift)` is caught. Wrapping *every* reset changes reset codegen
corpus-wide -- the reset-alias experiment showed a blanket change breaks the
shift0 / nested / substrate fixtures. So gate it:

- A **post-elaboration** whole-program pass (a same-elaboration global flag will
  not do -- a reset can be elaborated before the callee's shift sets the flag).
- Only when the program flag from slice B is set, rewrite each `EX_RESET` /
  `EX_CLONEABLE_RESET` body `B` to `(handle B (__Shift [recv] k) (recv k))`,
  leaving the reset's own (abortive/reified) lowering intact around it.
- Programs with no cross-function resuming shift are **byte-for-byte unchanged**
  (flag off -> pass is a no-op).

**Done when:** the opening example compiles and runs `outer = 23`,
`direct == cps == turi`; and the full suite is green (the gate keeps every
non-using program's reset codegen identical -- verify no fixture-snapshot churn).

### Integration -- end-to-end fixture on the real surface

Add `shift-crossfn-resume-works` (promote the current `errors/shift-crossfn-resume`
negative fixture): the opening example plus a capturing-receiver variant, all on
the plain `shift`/`reset` surface, `direct == cps == turi`. Remove/relax the
`TUR-E0016` message for the now-supported case (keep it for genuinely unhandled
shifts -- e.g. a resuming shift with no reset anywhere).

## Safety and gotchas (hard-won)

- **The gate is load-bearing.** Without it, reset codegen churns and the fragile
  shift0/nested/substrate shapes break (measured: a blanket `reset ->
  cloneable-reset` alias broke 8 fixtures). Keep the common case a no-op.
- **Effect continuations are one-shot by default** (`CK_UNIQUE`). A resuming
  shift that invokes `k` more than once needs `^multishot` semantics on the
  synthesized handler continuation; a single-resume shift is the clean first
  target. (Observed: a multi-shot probe returned a wrong value with default
  one-shot resume.)
- **`shift0`** is one-shot and does not re-install the prompt; decide whether it
  participates in this desugar or stays lexical-only (recommend: lexical-only
  first).
- **Don't run a suite while rebuilding** -- a mid-run `tur` rebuild produced a
  spurious 115-failure result this session. Let each build finish first.
- Keep `direct == cps == turi` at every slice; the effect machine already agrees
  across paths, so divergence signals a desugar bug.

## Out of scope

- Cross-function resume via the CT-IR DK-subk architecture (a `DK`-flavored cont
  threaded into the receiver) -- an alternative to the effect desugar, not needed
  if this lands.
- Multi-shot cross-function resume beyond wiring `^multishot` (its own semantics
  question).
- Folding `cloneable-*` / `serial-*` into `shift`/`reset` (separate plan).

## Where this came from

Split from
[cps-backend-n6-resuming-shift-plan.md](cps-backend-n6-resuming-shift-plan.md)
(item c). Full blocker analysis, probes, and the four receiver-convention bridge
options: [cross-function-resume-design.md](../../reported/cross-function-resume-design.md).
