# Interpreter Incremental Reclamation -- Bounding a Long-Lived turi Env (TR0--TR5)

> **Status:** Not started as an umbrella, but it sits on substantial landed
> groundwork. Env teardown is already leak-clean (the env-owned value pool,
> Phase 1, shipped). What is *not* solved is **incremental** reclamation: a
> long-lived interpreter env (REPL session, notebook kernel, embedded service)
> grows `value_scratch`, `eval_arenas`, `src_acc`, and dropped collection buffers
> **without bound** for the life of the env -- only a full env rebuild
> (`:reset` / `(reload)`) reclaims them. This plan ties the existing partial
> mechanisms together into a story that actually bounds a never-destroyed env.
>
> **This is the case where "a GC of some sort" is genuinely required.** The
> obstruction is real GC-shaped work: closures capture their defining frames, and
> `letrec`/mutual recursion makes frame<->closure references *cyclic*, so no
> scope-exit free is locally safe. The only correct reclamations are whole-arena
> teardown (coarse) or a tracing/copying walk that handles cycles via forwarding
> -- the conservative Cheney-style "scratch promotion" copy already prototyped is
> exactly such a collector.
>
> **Prerequisites:** builds directly on `turi-value-pool-scratch-promotion-plan`
> (Phase A landed, opt-in/off) and its unfinished tail
> `turi-value-pool-carrier-relocation-plan` (not done). Both are in
> `docs/archive/history/`.
>
> **Gate:** none for the interpreter path (turi is a tool, not shipped program
> semantics). Enabling promotion-by-default in the REPL is a behavior change to
> stage carefully but needs no `EXPERIMENTS[]` row.
>
> **Last updated:** 2026-07-24

---

## Motivation

The tree-walker deliberately never frees individual frames, closures, bindings,
or struct payloads -- per-object free (`eval_frame_free`, `eval.c:446-451`) is a
no-op, because a returned/stored closure captures its defining `EvalFrame`
(`eval.c:118,424-427`) whose lifetime is not bounded by lexical scope. For the
suite's fork-per-fixture model and one-shot `tur build`, this is fine: everything
is reclaimed at `turi_env_free` and the process is short-lived. The problem is
the **long-lived env**:

- The REPL creates one env (`repl.c:986`) and threads it through every
  evaluation; `turi_env_free` is not called between inputs. Each `turi_eval`
  appends an `ArenaNode` to `eval_arenas` (`eval.c:9927-9931`, field comment:
  "never freed", `env.h:165`) and appends to `src_acc`, which is then re-parsed
  whole on the next line. Scratch promotion is **not** enabled in the REPL.
- Net effect: `value_scratch` + `eval_arenas` + `src_acc` + any collection
  buffer dropped from scope all grow monotonically for the whole session. This
  is the "leak" the gc-guide gestures at -- not memory lost to the OS, but
  unbounded growth until teardown.

Bounding this is what makes turi viable as a persistent kernel/service, and it
closes the gap between the guide's reassuring "process-lifetime is fine" and the
reality for an env that never exits.

---

## What already exists (do not rebuild)

- **Env-owned value pool (Phase 1, done).** All escaping values live in
  `value_scratch`/`value_perm` (`value.c:16-52`); teardown reclaims everything in
  one pass (`env.c:267-336`). Teardown is leak-clean under LSan.
- **Scratch promotion (Phase A, landed, OFF by default).** When
  `turi_env_set_scratch_promotion(env,true)` is set, `turi_promote_escaping`
  (`eval.c:9809`) does a Cheney-style deep copy of the root set (eval result +
  globals) into `value_perm`, then `arena_reset(value_scratch)` (`eval.c:9836`)
  at each top-level eval boundary. Conservative: it declines to rewind whenever
  an escaping value reaches a shape it can't safely relocate (`eval.c:9811,9823`).
- **Carrier relocation (the hard tail, NOT done).** Relocating carrier-encoded
  bare-`int` pointer values (cons/vec/set/ADT carriers) and live
  continuation/generator/fiber C-state so promotion can rewind those too. Spec'd
  in `turi-value-pool-carrier-relocation-plan`.
- **Collections tracked + teardown-freed (done).** Vec/Set/Map buffers are
  registered (`env.c:474`) and swept at teardown (`env.c:320-327`), but only at
  teardown -- a collection dropped from scope is not reclaimed mid-run.

The reclamation model is therefore *already a copying collector*; the work is to
enable it, complete its coverage, and extend it past the value pool.

---

## Phases

### TR0 -- Enable + measure promotion in the REPL

Turn on `turi_env_set_scratch_promotion(env,true)` in `repl.c` and add a
steady-state memory assertion to the existing long-lived harness
(`tests/turi/env-longlived.c` / `tur_env_longlived`). Measure real REPL sessions:
how often does the conservative walk decline to rewind (the "quiescent" gate,
`eval.c:9811`)? This quantifies how much of the problem Phase A alone solves and
prioritizes TR1.

### TR1 -- Complete carrier relocation (revive the hard-tail plan)

Execute `turi-value-pool-carrier-relocation-plan`: teach the promotion walk to
relocate carrier-encoded bare-int pointers (cons/vec/set/ADT) and live
control-flow C-state, so `arena_reset(value_scratch)` becomes safe in the common
cases that currently force a decline. This is the substantive GC work -- the
forwarding-pointer walk that makes frame<->closure `letrec` cycles safe to copy.

### TR2 -- Reclaim `eval_arenas` + stop `src_acc` re-accumulation

Promotion resets `value_scratch` but never touches `eval_arenas` or `src_acc`
(`env.h:165`, `eval.c:9920-9924`), so AST/elaboration memory and source text
still grow per line. After promotion has copied out everything the eval result
reaches, the completed eval's AST/elaboration arena can be freed rather than
retained forever; and the REPL should stop concatenating + re-parsing the whole
prior source blob each input. This is orthogonal to the value pool and may be the
larger REPL win per line.

### TR3 -- Collection drop-glue (reclaim mid-run, not just at teardown)

Implement "fix direction 2" from `interp-collections-never-freed`: teach the
rc-drop path (`EX_RC_DROP` / `turi_rc_drop_value`) to recognize `:heap`
collection carriers (Vec data, Set/Map HAMT) and free them on scope exit, with
the aliasing care the compiled RC path takes (a Vec handle is a shared mutable
pointer -- do not free while aliased). Turns collection memory from
teardown-bounded (constant in N) into scope-bounded (near-zero steady state).

### TR4 -- Choose the long-term model: complete copying-promotion vs tracing sweep

With TR0--TR3 data in hand, decide the durable architecture:

- **(a) Copying-promotion** (extend what exists): each eval boundary is a minor
  GC; `value_perm` is the survivor space. Simple, incremental, already prototyped
  -- but `value_perm` itself is never compacted, so a very long session with many
  genuinely-immortal globals still grows (slower).
- **(b) Tracing mark-sweep over the pool**: register value-pool blocks, mark from
  roots (globals + live frames + control stack), sweep the unreachable. Handles
  cycles natively, compacts survivors over time, but is a larger build and must
  reuse the same walker discipline as the compiled-side cycle GC
  (`gc-cycle-collection-plan.md`) to stay maintainable.

Recommendation to evaluate: (a) for v1 (finish what's prototyped), with (b) as
the post-v1 direction if `value_perm` growth proves real. Note the pleasing
symmetry -- both the interpreter reclamation and the compiled-side cycle
collector are "tracing over a walker-described object graph," and should share
vocabulary even if not code.

### TR5 -- Tests + honest docs

- Extend `tur_env_longlived` to assert bounded steady state across thousands of
  evals with promotion + TR1--TR3 on, and a promotion-off control that still
  grows (proving the mechanism).
- Correct `docs/guides/gc-guide.md`: replace "the interpreter leaks its closures
  by design" framing with the accurate "region-allocated, reclaimed at teardown;
  incremental reclamation via scratch promotion (opt-in)" story, and note the
  long-lived-env caveat explicitly. (Tracked in
  `docs/reported/gc-guide-stale-and-misleading.md`.)

---

## Risks / open questions

- **The quiescent gate may decline often.** If real REPL use frequently has live
  continuations/generators at eval boundaries, Phase A rewinds rarely and TR1 is
  load-bearing, not optional. TR0 measures this first.
- **`value_perm` is unbounded under model (a).** Acceptable if immortal-global
  volume is small; TR4 revisits if not.
- **Aliasing in TR3.** A shared mutable Vec handle freed while still aliased is a
  use-after-free -- this needs the same escape care as the compiled RC path, not
  a naive scope-exit free. Conservative (leak-on-doubt) is the safe default,
  matching the rest of the interpreter.
- **Do not regress teardown leak-cleanliness.** Every phase must keep
  `tur_env_teardown` LSan-clean; incremental reclamation is added *under* that
  invariant, never at its expense.
