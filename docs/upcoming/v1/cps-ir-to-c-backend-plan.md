---
title: CPS-IR-to-C backend -- emitting the ANF/CPS IR as C for colored functions -- Plan
category: Planning
description: The whole-program CPS pipeline colors may-capture functions (CPS1) and translates each colored function body into an ANF/CPS IR (CPS2/CPS3, src/passes/cps_ir.c), but that IR is dump-only -- no backend emits it as C. Every control operator that needs a *resumable* continuation across a call boundary (perform/resume, and non-abortive shift/reset) is therefore stuck on the fiber runtime or on the syntactically-local abortive DK path. This plan builds the missing CPS-IR-to-C backend: it emits a colored function's CTerm as C that threads a heap continuation (the DK chain) through the fat-closure ABI, so a captured sub-continuation reaches from a callee up to its delimiter and is resumable via dk_invoke. It is the unbuilt prerequisite that docs/upcoming/v1/compiled-first-class-continuations-plan.md (Phase F1) depends on.
---

# CPS-IR-to-C backend -- Plan

## Why this exists

The whole-program CPS pipeline is built and colored end-to-end except for
its last stage:

- **CPS1 -- coloring** (`src/passes/cps.c`) marks every may-capture
  function. The seed set includes the delimited operators *and* the effect
  operators (`EX_PERFORM` / `EX_HANDLE` / `EX_RESUME` / `EX_DISCONTINUE`),
  and colors transitively. Verified by `tests/fixtures/cps-effect-coloring/`.
- **CPS2/CPS3 -- ANF/CPS IR** (`src/passes/cps_ir.c`, `cps_ir.h`)
  translates each colored function body into a `CTerm`: continuations are
  reified as `CKont` and threaded through `CT_TAILCALL` (`f(args, k)`) and
  `CT_APPCONT` (`(k v)`), join points as `CT_LETCONT`, etc. Exposed via
  `--dump-cps`.
- **The gap.** That IR is **dump-only** -- nothing emits it as C. The
  header says so directly: "The IR is dump-only at CPS2 (exposed via
  --dump-cps); it is not yet wired into codegen. CPS3 consumes it."

Because no backend consumes the IR, every construct that needs a
continuation to survive *across a call boundary* has no lowering and falls
back to one of two weaker substrates:

- **`shift` / `reset`** lower onto the DK machine, but **abortively**: the
  emitted `__dk_abort_body` (`src/compiler/emit_cps.c`) discards the
  captured sub-continuation, and `emit_cps_reset` only walks a
  **syntactically-local** delimited body (`emit_first_shift`). A `shift` in
  a callee, delimited by a `reset` in the caller, is not visible to that
  machinery.
- **`perform` / `handle` / `resume`** run on `ucontext` fibers
  (`src/compiler/emit_effects.c`): each `handle` body is its own fiber and
  `resume` re-enters it. Correct, but a different substrate from the CPS
  machine, and the one the compiled-first-class-continuations plan wants to
  retire for coherence.

Concretely, `--dump-cps` on a cross-function effect program shows the
threading is *already computed* -- it just isn't emitted:

```
cps-fn calls-performer [] k:cont<int> entry
  letcont j2(t0) =
    let t1 = (+ 1 t0)
    (k t1)
  in
  tailcall does-perform(j2)   ; cps->cps
cps-end

cps-fn does-perform [] k:cont<int> internal
  <unsupported: form not in CPS2 subset>     ; <- `perform` has no IR node yet
cps-end
```

The continuation `j2` is exactly the sub-computation a `perform` inside
`does-perform` must capture and a handler must resume. The backend this
plan builds is what turns that IR into C.

The resumable-capture *primitives* already exist and are proven: the
cloneable-continuation codegen (`emit_cps_cloneable_reset`, `emit_cps.c`)
builds `dk_frame` chains and resumes them with `dk_invoke` (multi-shot).
What is missing is the **general** emitter that produces those chains for an
*arbitrary colored function's* `CTerm`, threading the continuation through
the call ABI rather than only within one syntactic delimiter.

## Scope and guiding constraint

**Fallback-guarded, incremental, zero-regression.** The backend emits
CPS-form C for a colored function **only when its entire `CTerm` translates
without any `CT_UNSUPPORTED` node**. Otherwise the function keeps its
current emission path (direct-style, with effects on fibers) untouched.
This is the same discipline `emit_cps_reset` already uses (return NULL ->
fall back), lifted to whole-function granularity. Consequences:

- Coverage grows monotonically as `CT_UNSUPPORTED` gaps close; nothing that
  compiles today stops compiling.
- The whole backend ships behind an experiment and is off by default, so
  the ~1990-fixture suite is unaffected until a fixture opts in.
- "Correct subset now, wider subset later" is an explicit goal, not a
  compromise.

## The representation decision (Phase C0)

Ratify the calling convention before emitting anything. The proposal, to be
confirmed in C0:

- **Continuation value = `DK *`** (the multi-prompt chain from
  `cps_prompt.h`, already emitted into every delimited program by
  `emit_cps_runtime_prelude`). It carries prompt markers, so `reset` /
  `handle` and cross-function capture all live on one representation, and
  `dk_invoke` gives multi-shot resume for free.
- **Colored-function C ABI.** A colored `f` with source params `a b`
  becomes `int64_t f(int64_t a, int64_t b, DK *k)` -- the return
  continuation `k` (the IR's `KK_RET`) is an appended parameter. `KK_RET`
  already models this: `k : cont<T>`.
- **`CT_TAILCALL f(args, kont)`** emits `return f(args..., kont)` (the
  callee consumes the threaded continuation). **`CT_APPCONT (k v)`** emits
  the delivery of `v` to `k` -- `dk_run`/`dk_invoke` depending on whether
  `k` is the chain being built or a captured sub.
- **Direct<->CPS boundary trampolines.** An uncolored caller entering a
  colored function supplies the initial continuation via `dk_run_root`
  (implicit root prompt, `CPS5.3`); a colored function calling an uncolored
  one emits `CT_LETCALL` as an ordinary direct-style call and resumes the
  chain with the result. The IR already classifies these edges
  (`cps->cps` / `cps->direct` in the dump).

C0 output: a short ABI note appended here (or a sibling doc) plus a
throwaway hand-written C sketch of one colored function under the ABI,
compiled and run, to de-risk the convention before the emitter is written.

### C0 result -- ratified ABI

The convention above is **confirmed**. Two throwaway, hand-written C sketches
under `tests/probes/cps-abi-c0/` transcribe colored functions into the ABI
node-for-node against `tur check --dump-cps`, compile against the real DK
runtime (`src/runtime/cps_prompt.c`), and run to the expected value. Both are
clean under `-Wall -Wextra` and under `-fsanitize=address,undefined` with
LeakSanitizer proven live (a deliberate control leak is reported; the sketches
are not). Build/run:

```sh
cd tests/probes/cps-abi-c0
cc -std=c11 -Wall -Wextra mixed.c      ../../../src/runtime/cps_prompt.c -o /tmp/mixed      && /tmp/mixed        # prints 41
cc -std=c11 -Wall -Wextra xfn_resume.c ../../../src/runtime/cps_prompt.c -o /tmp/xfn_resume && /tmp/xfn_resume   # prints 422
```

The ratified rules, keyed to the IR node kinds the emitter (`emit_cps_ir.c`,
Phase C1+) will lower:

| Concept | C ABI |
| --- | --- |
| Continuation value | `DK *` (the `cps_prompt.h` multi-prompt chain) |
| Colored `f a b` | `int64_t f(int64_t a, int64_t b, DK *k)` -- return continuation `k` (`KK_RET`) appended as the last parameter |
| `CT_TAILCALL f(args, kont)` (`cps->cps`) | `return f(args..., kont);` -- thread the continuation straight through, no trampoline |
| `CT_APPCONT (k v)`, `k = KK_RET` | `return dk_run(k, (intptr_t)v);` -- deliver `v` to the threaded return continuation |
| `CT_APPCONT (k v)`, `k` a captured sub | `dk_invoke(k, (intptr_t)v)` -- multi-shot resume of a captured `DK` |
| `CT_LETCONT j(x)=jbody in body` | a join point: `DK *j = dk_frame(<jbody-as-frame>, env, <enclosing k>);` prepended onto the enclosing continuation; the chain flows into the outer `k` after `jbody` |
| `CT_LETCALL x = g(args)` (`cps->direct`) | ordinary direct-style call; `g` uncolored, no continuation threaded in |
| direct->cps entry | trampoline: seed a `dk_done()`-terminated root continuation, call the colored function passing it as `k` (`dk_run_root` when an undelimited capture must reach program entry) |
| `CT_RESET` | `dk_prompt(tag, next)` marker pushed into the chain being built |
| `CT_SHIFT` | `dk_shift(tag, body, env, next)`; the body receives the captured sub-continuation and may `dk_invoke` it (multi-shot) rather than discard it |

**What the sketches prove.** `mixed.c` (the `cps-mixed-coloring` fixture) exercises
all three direct<->CPS edges (`cps->cps` tail calls, a `cps->direct` call to the
uncolored `twice`, and the `direct->cps` entry trampoline), the two `letcont` join
points, and a syntactically-local `reset`/`shift`, computing `run(20) = 41`.
`xfn_resume.c` proves the load-bearing claim the whole backend exists for and that
the syntactically-local `emit_cps_reset` cannot do: a `shift` in a **callee**,
delimited by a `reset` in its **caller**, capturing a sub-continuation that spans
**both** functions' frames and **resuming it twice** (multi-shot) -- the caller
threads a `k` already carrying its own post-call frame and the prompt (exactly
`CT_TAILCALL`), the callee prepends its frame and shifts, and `dk_invoke` composes
the arithmetic across the boundary to `422`. This is the capability Phases C3/C4
generalize from hand-written to emitted C.

**Open follow-ups deferred to the emitter (not blockers for C0):** (1) `letcont`
join points whose `jbody` is not a single pure frame (contains its own calls or
branches) need lowering to a helper function rather than a bare `dk_frame` -- the
sketches only needed the frame form; (2) DK-node ownership/free discipline in
emitted code (the sketches leak a couple of join frames harmlessly at process exit,
which LSan tolerates as reachable-at-exit; a real emitter wants an arena or explicit
frees). Neither disturbs the calling convention ratified here.

## Phase C1 -- emit the control/data core to C

Emit the non-delimited `CTerm` kinds for a colored function whose body is
free of `reset` / `shift` / effects: `CT_APPCONT`, `CT_LETVAL`,
`CT_LETPRIM`, `CT_LETCALL`, `CT_TAILCALL`, `CT_LETCONT`, `CT_IF`. This is
the CPS analogue of straight-line + branch + join + call code.

- New file `src/compiler/emit_cps_ir.c` (+ header): `emit_cps_ir_fn(ctx,
  fd, cterm)` walks the `CTerm` and writes the function body; join points
  (`CT_LETCONT`) become either local labels/gotos or static helper
  functions taking the join parameter.
- Guard: if translating `fd` yields any `CT_UNSUPPORTED`, `emit_cps_ir_fn`
  returns false and the caller uses the existing path.
- **Round-trip fixture.** A colored pure/recursive function (colored
  because it transitively reaches a control op elsewhere, but whose own
  body is core-only) must compute the identical value through the CPS
  backend as through direct-style. Assert equality, not just "compiles".

### C1 result -- shipped

Built in `src/compiler/emit_cps_ir.c` (+ `emit_cps_ir.h`), gated behind
`--enable=cps-backend` (experiment row registered in
`src/runtime/experiments.c`, `g_opt_cps_backend`, prototype, expires 0.29.0).

- **Emittable-set classification.** `ensure_S` colors the program (the emit
  pipeline has not colored by emit time) and selects the colored functions the
  backend emits: those whose entire `CTerm` is in the C1 core subset (the seven
  non-delimited kinds, scalar int/bool types, supported builtin shapes) **and**
  which never need a join reified onto the heap chain -- i.e. no non-tail
  cps->cps call. That last clause is a monotone fixpoint (dropping a function
  only turns its callers' cps->cps edges into ordinary synchronous calls, which
  never forces a heap join), so it converges. Everything else keeps its
  direct-style emission, so coverage grows monotonically with zero regression.
- **Emission.** Each emittable `f` becomes `int64_t f__cps(params..., DK *k)`
  plus an `__attribute__((unused)) static int64_t f(params...)` entry wrapper
  that seeds `dk_done()` -- so uncolored/direct callers reach the CPS body by
  the plain name unchanged. `CT_TAILCALL` to another emittable fn threads `k`
  (`return g__cps(args, k)`); to a fallback/uncolored fn it calls synchronously
  and delivers the result to the continuation. `CT_APPCONT` to the return
  continuation is `return dk_run(k, v)`. Join points (`CT_LETCONT`) lower to a
  C local + forward label/goto: delivering to join `j` assigns the join
  parameter and `goto L<j>`. `main` and exported bindings are held on the
  direct-style path (their linkage is fixed).
- **Round-trip fixture** `tests/fixtures/cps-backend-core/`: one program
  exercising a cps->cps tail call between two emitted functions, a cps->direct
  call, a synchronous fallback bridge to a delimited `seed`, `CT_IF`, an inline
  join, and tail self-recursion. Built with `--enable=cps-backend`; its
  `expected.stdout` (`18`) is the direct-style value, so the CPS build asserts
  value equality. Full suite: 1988 passed, 0 failed (neutral when the gate is
  off).

De-risking of the general boundary trampolines (`dk_run_root`, final-value
unwrapping, all three edges in the `cps-mixed-coloring` shape with cps->cps
join reification) is Phase C2; C1 deliberately excludes the non-tail cps->cps
join (it needs a real `dk_frame`) via the fallback guard.

## Phase C2 -- direct<->CPS boundary bridging in emitted C

Make the two boundary edges real in C, matching the IR's existing
classification:

- **cps->direct** (`CT_LETCALL`): a colored function calls an uncolored
  one. Emit an ordinary call, bind the result, continue the chain. No
  continuation is threaded into the uncolored callee.
- **direct->cps entry**: an uncolored function (or `main`) calls a colored
  one. Emit an entry trampoline that seeds the initial continuation
  (`dk_run_root` with a `dk_done`-terminated chain) and unwraps the final
  value.
- **cps->cps** (`CT_TAILCALL`): both colored -- thread `k` straight
  through, no trampoline.

Fixture: `cps-mixed-coloring`'s runtime result (currently produced "via the
existing fiber path" per its own comment) must be reproduced by the CPS
backend, exercising all three edges in one program.

### C2 result -- shipped

All three edges are real in the emitted C and self-documenting (each emitted
call carries a `/* cps->cps */` or `/* cps->direct */` marker matching the IR
classification):

- **cps->direct** (`CT_LETCALL`, and a `CT_TAILCALL` to an uncolored or
  fallback callee): an ordinary synchronous call binds the result, then the
  chain continues / the value is delivered to the continuation. No continuation
  is threaded into the callee.
- **direct->cps entry**: the entry trampoline emitted alongside every emittable
  `f__cps` was upgraded from a bare `dk_done()` seed to a principled,
  leak-clean root: it installs the CPS5.3 implicit root prompt
  (`dk_prompt(DK_ROOT_TAG, dk_done())`) -- the structural equivalent of
  `dk_run_root`, so an undelimited capture inside the body reaches program
  entry -- runs the CPS body, `dk_free`s the seed chain, and returns (unwraps)
  the delivered value. Uncolored callers (including `main`) reach the colored
  function by its plain name unchanged.
- **cps->cps** (`CT_TAILCALL` to another emitted fn): `k` is threaded straight
  through (`return g__cps(args, k)`), no trampoline.

Fixture `tests/fixtures/cps-backend-mixed/`: the `cps-mixed-coloring` program
built under `--enable=cps-backend`, reproducing the historical fiber-path
result (`41`). `run` is CPS-emitted and bridges synchronously (cps->direct) to
the delimited-and-therefore-fallback `shift-then-twice`; `main` is the
direct->cps entry. (The genuine cps->cps tail edge between two *emitted*
functions is covered by `cps-backend-core` from C1 -- in `cps-mixed` the
delimited `shift-then-twice` breaks the colored chain into a fallback, so no
cps->cps edge survives there.) Full suite: 1989 passed, 0 failed.

Note: a *non-tail* cps->cps call (a colored call whose result is consumed,
needing the join reified onto the heap chain as a `dk_frame`) remains outside
the emitted subset -- such a caller still falls back -- consistent with the C1
scope. Closing it is future work layered on the C3 machinery.

## Phase C3 -- resumable `reset` / `shift` on the general backend

With whole-function CPS emission in place, `reset` / `shift` stop being
syntactically-local:

- `CT_RESET` -> push a `dk_prompt(tag, ...)` marker into the chain being
  built (the delimiter can now sit in a *different* function from the
  shift).
- `CT_SHIFT` -> `dk_shift(tag, body, env, ...)` capturing the real
  sub-continuation `subk`; the shift body receives `subk` and may
  **resume** it with `dk_invoke` (multi-shot) instead of discarding it.
  This supersedes the abortive `__dk_abort_body` for colored functions
  (the abortive path stays for the uncolored/legacy fallback).
- Cross-function delimited control: a `shift` in a callee reaches the
  `reset` in its caller because the caller threaded its continuation as
  `k`. This is the capability that unlocks Phase C4.

Fixtures: extend `escape-nested-reset`, `serial-reset-basic`, and add a
cross-function `reset`/`shift` case that the current syntactically-local
`emit_cps_reset` rejects (verify it falls back today, and lowers under the
backend).

### C3 result -- shipped (cross-function abortive subset)

`CT_RESET` and `CT_SHIFT` now lower through the backend, so a `shift` in a
callee reaches a `reset` in its caller across the threaded continuation.

- **Receiver fix (prerequisite).** `cps_ir` previously discarded the shift
  receiver `k_fn`, making the IR lossy (see
  `docs/archive/cps-ir-shift-receiver-dropped.md`). It now translates
  `(shift k_fn body)` as the CPS of `(k_fn body)` delivered to the prompt, so
  the delivered value is the correct `receiver(body-value)`; a non-callable
  receiver becomes `CT_UNSUPPORTED` (fallback, never a miscompile).
- **`CT_RESET`** lifts its continuation `\x. body` to a file-scope `DKFrame`
  helper (`<fn>_kN(env, x)`), installs `dk_prompt(1, dk_frame(helper, k,
  dk_done()))`, and tail-emits the delimited body threading that prompt chain
  as the current continuation (`KK_PROMPT` deliveries run it; `KK_PROMPT` tail
  calls thread it). The reset stays in tail position (stackless).
- **`CT_SHIFT`** lifts its body to a `DKBody` helper (`<fn>_sN(env, subk)`) that
  computes the receiver-applied value and returns it, and emits
  `dk_run(dk_shift(1, helper, 0, <cur_k>), 0)` -- capturing the real
  sub-continuation up to the nearest enclosing prompt (which may sit in a
  different function). The abortive body discards `subk`; the multi-shot
  `dk_invoke` resume path is wired by C4 (`resume`).
- **Subset (fallback-guarded).** Zero-capture only: a reset continuation or
  shift body that would have to capture an enclosing local is rejected at
  classification (`has_capture`) and falls back, as is a non-identity/indirect
  receiver, a non-straight-line shift body, or a non-self-contained reset
  continuation (`reset_body_ok`). Per-reset/shift DK nodes are leaked (DK is
  opaque; `docs/reported/cps-delimited-dk-node-leak.md`), matching the abortive
  path.
- **Fixture** `tests/fixtures/cps-backend-xfn-reset/` (`--enable=cps-backend`,
  `requires.no-leak-check`): the cross-function `reset`/`shift` case
  `emit_cps_reset` rejects -- `inner` shifts, `outer` resets around the call --
  reproducing the direct-style `6`. `escape-nested-reset`, `serial-reset-basic`,
  and `shift-result-typing` remain green (they use `escape` / serial /
  cloneable variants, which stay on their existing paths). Full suite: 1990
  passed, 0 failed.

## Phase C4 -- `perform` / `handle` / `resume` as IR nodes

The payoff phase, and the exact hinge the compiled-first-class-continuations
plan (F1) is blocked on:

- Add `CT_PERFORM` / `CT_HANDLE` / `CT_RESUME` / `CT_DISCONTINUE` to the IR
  (`cps_ir.h`) **or** lower them in `cps_ir.c` to `CT_SHIFT` / `CT_RESET`
  against a per-effect prompt tag. Decide based on how much effect-specific
  metadata (effect name dispatch, multi-case handlers) the shift/reset
  shape can carry cleanly; a dedicated node is likely clearer for
  multi-case `handle`.
- `handle` -> a prompt keyed by the effect (one tag per handler, or the
  effect name interned as tag); handler cases become continuations of that
  prompt.
- `perform` -> a shift-shaped capture against the effect's prompt; the
  sub-continuation from the perform site up to the enclosing `handle` is
  reified as a `DK` chain and handed to the matching case as `k`.
- `resume k v` -> `dk_invoke(k, v)`; `discontinue k v` -> a DK-side
  abortive invoke (audit the `panic-return-signal` transport for the
  cleanest hook, per the parent plan).
- `(cont? k)` -> read the DK continuation's consumed state; the
  stack-allocated `TurContK` token disappears on this path.

At the end of C4, a colored effect program lowers with `perform` capturing
a real heap continuation and `resume` invoking it -- the fiber runtime is
no longer on the critical path for colored effect code (it stays as the
uncolored/`async` fallback).

### C4 result -- shipped (shallow single-effect subset)

`perform` / `handle` / `resume` now lower onto heap continuations via a new
runtime **handler-prompt** rather than the fiber runtime.

- **Runtime.** `cps_prompt.{h,c}` (and the emitted prelude in `emit_cps.c`) gain
  a `DKK_HANDLER` node carrying a `DKHandler` case, `dk_handler(tag, fn, env,
  next)`, and `dk_perform(tag, arg, k)`: find the nearest enclosing handler for
  `tag`, reify the sub-continuation from the perform site up to it (re-installing
  the handler on the captured copy for deep-handler semantics), run the case with
  `(arg, subk)`, and deliver its result to the handler's outer continuation.
- **IR.** Dedicated nodes `CT_HANDLE` / `CT_PERFORM` / `CT_RESUME` (`cps_ir.h`,
  translated in `cps_ir.c`; the effect is identified by its interned name Symbol,
  which `emit_cps_ir.c` maps to a per-program prompt tag). A dedicated node was
  chosen over lowering to shift/reset because the handler case carries
  effect-specific param/`k` binding.
- **Emission.** `handle` mirrors `reset`: it lifts the handle continuation to a
  `DKFrame`, installs `dk_handler` (case as a `DKHandler`), and runs the handled
  body threading the handler prompt (cross-function -- the `perform` may sit in
  a callee). `perform` mirrors the reified capture: a tail perform is
  `dk_perform(tag, arg, cur_k)`; a perform with post-work lifts that work as a
  pure value-transform frame onto the chain. `resume k v` is `dk_invoke((DK*)k,
  v)` -- **multi-shot for free** via the DK copy (verified: a `^multishot`
  handler resuming twice sums correctly). The handle continuation runs exactly
  once (no double-run).
- **Subset (fallback-guarded).** Single handler case, effect arity <= 1,
  zero-capture straight-line perform continuations and handler-case bodies.
  Anything else -- multi-case handlers, `discontinue`, `(cont? k)`, env-capturing
  or tail-call continuations -- falls back to the fiber path (correct, just not
  accelerated). Per-perform/handle DK nodes are leaked (opaque `DK`; the open
  leak report covers it), so effect fixtures carry `requires.no-leak-check`.
- **Not yet stackless.** A deeply-recursive perform/resume loop (the
  `effect-rec` sign-off probe) nests `dk_perform` -> handler -> `dk_invoke` in C
  and would overflow under `ulimit -s 256`; that needs a trampoline driver (yield
  to a top-level loop instead of nesting) and is deferred. `effect-rec` uses an
  env-capturing accumulator continuation, so it falls back to fibers today and
  the probe is unaffected. The C6 sign-off will need the trampoline before
  routing deep effect recursion through this backend.
- **Fixture** `tests/fixtures/cps-backend-effect/` (`--enable=cps-backend`,
  `requires.no-leak-check`): a cross-function `perform`/`handle`/`resume` --
  `use-ask` performs, `run` handles and resumes -- reproducing the direct-style
  `420`, with the handle continuation (`* 10`) running once. All existing effect
  fixtures produce identical values under the backend (CPS-emitted or fallback).
  Full suite: 1991 passed, 0 failed.

## Phase C5 -- close the `CT_UNSUPPORTED` gaps that block real code

`--dump-cps` today shows broad `<unsupported: form not in CPS2 subset>`
coverage (match, several struct/ADT forms, assorted stdlib bodies). The
backend only needs the forms that actually appear **in colored functions**,
so drive this phase by real programs, not by the whole grammar:

- Enumerate the `CT_UNSUPPORTED` reasons hit by the effect + delimited
  fixture corpus under `--enable`, rank by frequency, and close them in
  order (translation in `cps_ir.c` + emission in `emit_cps_ir.c`).
- Each closed form gets a round-trip fixture (direct vs CPS value equality).
- Anything still unsupported keeps the whole-function fallback, and
  `--dump-cps` remains the diagnostic that shows what is left.

### C5 result -- shipped (top tractable gap closed; remainder measured)

Drove this by measurement: tallying `<unsupported: ...>` (temporarily annotated
with the source `ExprKind`) across the 120-fixture effect + delimited corpus
ranked the blocking forms in colored functions:

| form | count (before) | disposition |
| --- | --- | --- |
| `EX_ASCRIBE` `(:: e T)` | 717 | **closed** -- erased at codegen; `cps_ir.c` now peels it |
| `EX_GET_FIELD` (struct/ADT read) | 1077 | deferred -- non-scalar value support |
| `EX_MAKE_STRUCT` | 358 | deferred -- non-scalar |
| `EX_DEFAULT_OF` | 357 | deferred -- non-scalar |
| `EX_CSTR_LIT` (strings) | 42 | deferred -- non-scalar (pointer) values |
| misc (`EX_PANIC`, `EX_WHILE`, ...) | <5 each | deferred -- low frequency |

- **Closed: type ascription.** `ascribe_peel` in `cps_ir.c` unwraps `EX_ASCRIBE`
  everywhere the translator inspects a form (`cps_tail`, `cps_bind`,
  `is_atomic`, `atom_of`), so a colored function whose body carries an
  ascription on a scalar sub-expression now lowers instead of falling back
  (717 -> 0 in the corpus).
- **Round-trip fixture** `tests/fixtures/cps-backend-ascribe/`: a colored effect
  function (`use-ask` performs `Ask`) whose body carries `(:: 1 :int)` now
  CPS-emits, reproducing the direct-style `420`.
- **Deferred (kept behind the permanent fallback).** The remaining top blockers
  are ADT/struct forms (`EX_GET_FIELD` / `EX_MAKE_STRUCT` / `EX_DEFAULT_OF`,
  ~1800 combined) and strings (`EX_CSTR_LIT`), all requiring **non-scalar value
  support** (the emitter currently threads everything as `int64_t`). They appear
  overwhelmingly in stdlib option/result/map bodies that get colored transitively
  but fall back harmlessly (direct-style is correct); the user effect/delimited
  code on the critical path is already scalar and already lowers. Per this
  phase's own scope ("the whole-function fallback covers the rest indefinitely")
  these stay deferred until a program actually needs them.
- **Related finding filed.** A control op nested *only* inside an ascription is
  invisible to the coloring pass (`cps.c` lacks an `EX_ASCRIBE` case), so such a
  function is never colored and the backend never sees it -- a coverage gap, not
  a miscompile (direct-style still handles it). Not fixed here because coloring
  is always-on and feeds decisions beyond this backend; see
  `docs/reported/cps-coloring-ascription-hides-control-op.md`.

Full suite: 1992 passed, 0 failed.

## Phase C6 -- experiment gate, sign-off, docs

- **Gate.** Ship behind `--enable=cps-backend`, prototype lifecycle,
  `opt_global -> g_opt_cps_backend`, `expires_at` two releases after the
  first colored subset lands. Register the row in
  `src/runtime/experiments.c` with all seven fields and call
  `experiment_warn_if_used("cps-backend")` from the emit entry.
  (`--enable=cps-effects` from the parent plan then becomes "route colored
  effect programs through this backend"; it can depend on `cps-backend` or
  be folded into it -- decide at C4.)
- **Neutrality.** Off by default -> the suite is untouched; when a fixture
  opts in, direct-vs-CPS value equality is the correctness gate. Measure
  hot-path neutrality on any fixture that opts in, same bar the catch-unwind
  graduation held.
- **Sign-off probe.** Reuse `tests/probes/stackless-signoff/effect-rec.tur`
  compiled under `--enable=cps-backend`: same `1000000` result, no SIGSEGV,
  under `ulimit -s 256`, now on heap continuations rather than fibers.
- **Docs.** A `docs/guides/` note on the ABI and the direct<->CPS boundary,
  and update the compiled-first-class-continuations plan's Status to point
  F1's substantive lowering at this backend.

## Depends on / reuses

- **CPS/ANF IR** (`src/passes/cps_ir.c`, `cps_ir.h`) -- the input; already
  computes continuation threading, join points, and boundary classification.
- **Coloring** (`src/passes/cps.c`) -- selects which functions this backend
  emits; already seeds on effect + delimited ops.
- **DK multi-prompt machine** (`src/runtime/cps_prompt.{h,c}`) -- emitted
  into generated C by `emit_cps_runtime_prelude`; supplies `dk_prompt` /
  `dk_shift` / `dk_invoke` / `dk_run` / `dk_run_root`.
- **Cloneable-continuation codegen** (`emit_cps.c`) -- the proven pattern
  for building `dk_frame` chains and resuming with `dk_invoke`; the general
  emitter generalizes it past one syntactic delimiter.
- **`panic-return-signal` transport** (graduated) -- the hook `discontinue`
  rides.

## Out of scope

- **The `async` scheduler.** Fibers stay the default `async` runtime; moving
  `await` onto CPS is Phase F3 of the parent plan (`--enable=cps-async`),
  downstream of this backend, not part of it.
- **Removing direct-style emit.** Uncolored functions keep their current,
  faster direct-style lowering permanently. This backend is only ever for
  colored functions.
- **Full grammar coverage.** C5 closes the forms real colored code needs,
  not the entire language; the whole-function fallback covers the rest
  indefinitely.
- **Source-surface changes.** No changes to `defeffect` / `handle` /
  `perform` / `shift` / `reset` syntax or typing; this is codegen only.
- **Retiring the standalone unit tests.** `tests/cps_prompt_unit.c` /
  `tests/cps_rt_unit.c` keep exercising the runtime in isolation.
