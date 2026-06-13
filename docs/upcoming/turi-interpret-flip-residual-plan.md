# turi `--interpret` flip: residual-gap plan (W5 tail)

> **Status:** Draft Plan
> **Last Updated:** 2026-06-13
> **Type:** Interpreter / Test Infra
> **Scope:** post-v1 -- the *last* tranche of non-inline-C fixtures that fail
> under `tur --interpret`, blocking the allowlist->denylist flip (W5).
> **Parent:** [turi-interpreter-gap-closure-plan.md](turi-interpreter-gap-closure-plan.md)
> (this is the concrete decomposition of that plan's W5 "residual" set)
> **Grand-parent:** [turi-parity-post-v1-plan.md](turi-parity-post-v1-plan.md) Phase TI8.b

---

## Why this plan exists

The gap-closure plan drove the interpreter from ~910 failures to a small tail.
A fresh full-denylist probe (every `tests/fixtures/*` under `tur --interpret`,
minus `requires.{compiled,tur-only,dedicated-runner,spices,tsan}`, built from
`5d5a9c8`) measures:

```
pass = 866   fail = 345   skip = 117
  311  inline-C-bodied fixtures      (permanent TI7 carve-outs, auto-skipped)
   34  non-inline-C fixtures         <-- THIS PLAN
```

The 34 are the entire remaining obstacle to the flip. They are **not** the cheap
wins the earlier workstreams harvested -- they cluster into a few whole-subsystem
native-shim campaigns, a continuation/fiber set, and three genuine silent
miscompiles. Each bucket below carries a fix-vs-carve disposition (per the
gap-closure plan's decision rule) and grounded pointers so the work is mechanical.

The harness-flip reconciliation report
([archived](../archive/history/turi-harness-flip-reconciliation.md)) is the
historical catalogue of how the flip got here; this plan is the forward map for
finishing it.

---

## Bucket R1 -- resource/concurrency native-shim campaign (~16, *fix*)

> **Progress (slices 1-3 landed, 2026-06-13):** 11 fixtures closed via additive
> `cmd_eval` native registration (`src/main.c`), no codegen change. Harness
> 1159 -> **1169**, compiled suite 1615/0, parity 0-gaps; all on the allowlist.
>
> - **Slice 1** (5): `safe-box` (wired the existing `wk_register_safe_natives` +
>   `wk_register_typeclass_natives` into the `--interpret` path -- previously only
>   `wk_eval_fixture` registered them), `comonad-capturing-closure`
>   (`wk_register_comonad_natives`, Identity `{value}` / Pair `Tuple2` layout),
>   `mutex-linear` (faithful `pthread_mutex_t`), `future-split-free`
>   (layout-exact refcounted `WkFutureCell`), `bytes-linear` (serial.tur `Bytes`).
> - **Slice 2** (1): `taskgroup-linear` (`WkTaskGroup` replica;
>   `cancel`/`wait`/`cancelled?` over the group flag, skipping the per-fiber TLS
>   cancel). Surfaced and filed a latent compiled-path heap OOB:
>   [taskgroup-block-cancel-reason-layout-overflow.md](../reported/taskgroup-block-cancel-reason-layout-overflow.md).
> - **Slice 3** (4): `chan-linear` / `asyncchan-linear` (bounded mutex-guarded
>   ring buffer -- the single-threaded interpreter cannot block, and the fixtures
>   stay within capacity), `future-linear` (`promise-fulfill` / `future-done?`),
>   `schan-roundtrip` (schan.tur synchronous session channels reuse the ring
>   buffer + a one-int64 recv cell).
>
> - **Slice 4** (2): `childhandle-linear` (`process/spawn` forks+execvp's,
>   ChildHandle = pid; `process/wait` reaps it) and `tmpfile-linear-borrow`
>   (`fs/tmpfile` mkstemp's a `{path,fd}` pair; borrow-accessors + close/free) --
>   faithful OS syscalls. `fd->int` is pure-turi (an ascription).
> - **Slice 5** (1): `serial-primitive-roundtrip` -- `Serializable [int]/[bool]`
>   instance natives over the length-prefixed LE byte buffer, under the
>   `__inst_Serializable_*` binding names (`extern-c printf` already runs under
>   `--interpret`). The deserialize native uses unsigned shifts, dodging a latent
>   signed-shift UB in serial.tur:
>   [serial-deserialize-int-signed-shift-ub.md](../reported/serial-deserialize-int-signed-shift-ub.md).
>
> **R1 stdlib-native surface COMPLETE** (harness 1159 -> **1172**, +13; compiled
> 1615/0, parity 0-gaps). The one item that stays out of R1:
> - `session-close` -- **not R1.** `make-session`/`close` are `-Xsessions`
>   compiler/runtime builtins (`src/compiler/elab_sessions.c`), so this is
>   interpreter session-runtime support, tracked with the runtime-level gaps
>   rather than a stdlib native shim.


The fixture body is pure-turi but it imports a stdlib subsystem whose ops are
`#{Unsafe}` inline-C over a fixed C ABI, so the tree-walker declines them with
`inline-C not supported in interpreter mode`. This is the **same pattern already
solved** for seq/map/hamt/free/either/grid/sized-buf: re-implement the bridge
surface as interpreter natives over the identical struct layout, registered in a
`wk_register_<subsys>_natives(env)` and wired into `cmd_eval` next to the
existing `wk_register_seq_natives` / `wk_register_map_natives` calls
(`src/main.c:5106-5139`). Reference implementation to mirror:
`wk_register_seq_natives` (`src/main.c:6812`).

Sub-campaigns (sequence by fixture count / shared ABI):

| Sub-campaign | Fixtures | stdlib source |
| --- | --- | --- |
| **Linear handles** | `bytes-linear`, `childhandle-linear`, `mutex-linear`, `taskgroup-linear`, `session-close`, `tmpfile-linear-borrow` | `mutex.tur`, `taskgroup.tur`, `session.tur`, + the bytes/tmpfile/childhandle handle ops |
| **Channels** | `chan-linear`, `asyncchan-linear`, `schan-roundtrip`, `serial-primitive-roundtrip` | `chan.tur`, `schan.tur`, `serial.tur` |
| **Futures** | `future-linear`, `future-split-free` | `future.tur` |
| **Misc handle/typeclass** | `comonad-capturing-closure`, `safe-box`, `typed-slots/tuple2-eq-method` | `comonad.tur`, `safe.tur` (`wk_register_safe_natives` at `src/main.c:9117` already exists -- extend it), tuple2 eq method |

**Disposition: fix.** Each is a finite native set over a known layout; none needs
new language machinery. **Note the linear angle:** these fixtures exercise the
affine/linear checker, which the interpreter shares with the compiled path, so
the *check* already runs -- only the runtime ops are missing. Validate each
sub-campaign by re-probing + adding the now-passing fixtures to the allowlist;
the compiled path is untouched (additive natives only).

**Caveat (do not skip):** mirror the W1b lesson -- the native must read the
*exact* layout the compiled ABI produces (`{cap,len,...}` headers, fat-closure
carriers via `seq_as_closure`/`turi_call`), or it silently miscompiles. Probe a
value with a non-trivial payload, not an empty handle.

## Bucket R2 -- GC subsystem natives (4, *fix*) -- **DONE 2026-06-13**

`gc-dag`, `gc-no-false-positives`, `gc-perf`, `gc-stress` all passed once the
interpreter could run `gc!`/`gc-enable!`/`gc-disable!`. These were **not** a
missing-native gap: the rc/weak ops already have `eval.c` case arms
(`EX_RC_OF`/`EX_RC_CLONE`/`EX_RC_DROP`/`EX_RC_COUNT`/`EX_WEAK`/...), and the GC
builtins lower (`elab_memory.c`) to exact captureless inline-C one-liners
(`gc_force();` / `gc_enable();` / `gc_disable();`) -- they were the sole thing
hitting `EX_INLINE_C`'s clean carve. The fix matches those three code slices in
the `EX_INLINE_C` eval case (`src/turi/eval.c`) and calls the linked runtime
(`src/runtime/gc.c`) directly. `gc-perf`/`gc-stress` are small (100 / 20
iterations) and finish well under the 10s run timeout -- **no carve needed**.
Harness 1172 -> **1176**, compiled 1615/0, parity 0-gaps; all 4 on the allowlist.

## Bucket R3 -- HKT stdlib instance resolution (2) -- **DONE 2026-06-13**

Split fix-vs-carve after the repro:

- **`hkt-stdlib-backtrack-instances` -- FIXED.** backtrack.tur's `Backtrack` is a
  cons-stream (linked list of results, `{value,next}` cells), and every
  Functor/Applicative/Monad/Alternative instance + the `*-raw` workers are
  pure-turi wrappers over a handful of inline-C primitives. `wk_register_backtrack_natives`
  (`src/main.c`) shims those primitives (`bt-cons`/`mreturn`/`mzero`/`mplus`/
  `mbind`/`bt-length`/`bt-print`/`bt-apply-fat`); `mbind`/`bt-apply-fat` invoke
  the continuation via `turi_call`. On the allowlist.
- **`hkt-stdlib-logic-instances` -- CARVED `requires.tur-only`.** logic.tur is a
  full miniKanren: ~22 inline-C primitives (tagged term constructors/accessors,
  unification, the substitution store, walk, reify-walk, run-logic threading
  state through the Backtrack stream). Re-implementing that engine as interpreter
  natives is disproportionate for one fixture and is library internals, not a
  language-level gap. The fixture still runs (and PASSes) on the compiled path.

Harness 1176 -> **1177**, compiled 1615/0, parity 0-gaps.

## Bucket R4 -- effect / multishot continuation divergence (7) -- **DONE 2026-06-13**

Investigated; split 2 fix / 5 carve:

- **`safe-array-bounds` -- FIXED (was a false negative).** It already produced
  the correct stdout; the only output was a benign ASan `makecontext/swapcontext`
  warning on **stderr** (the probe mis-scored it). Allowlisted.
- **`async-with-handler` -- FIXED.** The elaborator stores a non-`fn` async body
  *raw* (`elab_concurrent.c`, no thunk wrap), so `EX_ASYNC` pre-evaluated
  `(with-handler ...)` to its int result and then errored "expected a function".
  The `EX_ASYNC` eval case (`src/turi/eval.c`) now settles the future with that
  value when the body completed to a non-closure -- a synchronously completed
  body *is* a resolved future under the single-threaded interpreter; a
  `(fn [] ...)` thunk still takes the fiber-spawn path. Allowlisted.
- **`effect-capture-k`, `effect-handler-capture-nested`, `fh-multishot-value`,
  `multishot-copy-capture`, `multishot-handler` -- CARVED `requires.tur-only`
  (`interp-continuation`).** All five are genuine deep delimited-control gaps:
  resuming a continuation more than once (`^multishot`, SIGABRT), after its
  `handle` returned (escaping; heap-use-after-free), or *through* nested handlers
  (loses outer frames; `unhandled effect: B`). They are crashes / clean errors,
  not silent miscompiles, and need a heap-owned, clonable continuation -- one
  feature that overlaps the [trampoline plan](v1/turi-eval-trampoline-plan.md).
  Full analysis + repros:
  [turi-interpreter-delimited-control-gaps.md](../reported/turi-interpreter-delimited-control-gaps.md).
  All five still PASS on the compiled path.

Harness 1177 -> **1179**, compiled 1615/0, parity 0-gaps.

## Bucket R5 -- silent miscompiles (3) -- **DONE 2026-06-13 (all fixed)**

Highest severity (rc=0, wrong stdout) -- all three **fixed** (not carved), so no
wrong-answer is hidden behind a marker:

- **`polymorphic-ok-err-value-struct-payload` + `typeclass-return-dispatch-
  result-wrapped`** -- `native_ok`/`native_err` (`src/main.c`) flattened the
  payload to a bare int64, losing the tag of a *heap* payload (a make-struct
  struct, a cstr), so `ok-val`/`.field`/`println` read garbage. Fix: `ok`/`err`
  now build a make-struct `Result` (tag-preserving) when the payload is a heap
  value; int/bool payloads keep the int64 box, so the carrier-ABI fixtures are
  unaffected. `result_field` reads both reps.
  [turi-value-struct-payload-interpreter-miscompile.md](../reported/turi-value-struct-payload-interpreter-miscompile.md)
  (RESOLVED).
- **`multishot-snapshot`** -- `tur_continuation_snapshot` was unhandled in
  `ts_try_cont_builtin` (`src/turi/eval.c`) so it returned 0 and the snapshot
  resumed to 0. It is an alias for `tur_cloneable_cont_clone` (a deep
  continuation copy) -- exactly as the compiled path `#define`s it
  (`emit_module.c:3083`). One-line fix: match the alias in the clone branch.
  [turi-multishot-continuation-snapshot-miscompile.md](../reported/turi-multishot-continuation-snapshot-miscompile.md)
  (RESOLVED). (Note: this was a *cloneable*-continuation alias gap, not the deep
  one-shot-fiber multishot work the R4 family needs.)

Harness 1179 -> **1182**, compiled 1615/0 (no Result-rep regression), parity
0-gaps. All three on the allowlist.

## Bucket R6 -- known carve-outs (~2, *carve*)

- `range-show` -- genuine dependency inline-C (`range.tur`'s `snprintf %s`
  formatting body); already documented as a carve in the reconciliation report.
- `reader-macros-rx-literal` -- drives `re.tur`'s regex engine (inline-C). Carve
  unless R1-style native shims for the regex VM are in scope (they are not, here).

Carve `requires.tur-only` with reason `inline-c`. (`elab-defmodule-after-load`
fails with `inline-C not supported` too but is a *loader* test -- triage whether
its inline-C is incidental before carving.)

---

## Sequencing

```
R5 (silent miscompiles -- highest severity, blocks flip)  ──┐
R1 (resource/concurrency natives, biggest count)           ─┤
R2 (gc natives)                                            ─┼─► re-probe ─► W5 flip
R3 (hkt instances: fix or carve)                           ─┤   (delete allowlist,
R4 (effect/multishot: investigate, fix or carve)           ─┤    default to denylist)
R6 (carve range-show / rx-literal)                         ──┘
```

Suggested PR slicing (each independently green, allowlist additions in-PR):

1. **R5** -- the value-struct-payload fix (one root cause, two fixtures) + the
   multishot-snapshot fix. Highest severity; do first.
2. **R1 linear-handles** sub-campaign, then **R1 channels**, then **R1 futures**
   -- one PR per sub-campaign (shared ABI keeps each focused).
3. **R2** gc natives (+ carve gc-perf/stress if they time out).
4. **R3 / R4** -- investigate, fix the tractable, carve the rest with reports.
5. **R6** -- carve sweep.
6. **W5 flip** -- delete `TURI_FIXTURES_DEFAULT` from `tests/run-turi.sh`, default
   to "run everything minus markers," flip `run-flags.sh`'s three `tur run`
   assertions (`:345`/`:355`/`:408`), confirm green. (Detailed in the gap-closure
   plan's W5.)

**Re-measure after every step.** Definition of done is unchanged from the parent:
`run-turi.sh` green at denylist default, `check_turi_parity.py` 0-gaps, full
`tests/run.sh` green, no silent miscompile hidden behind a marker-only carve.

---

## Validation / metrics (carry from parent)

- **probe pass/fail/skip** -- headline; target non-carved fail -> 0
  (866/345/117 at this plan's open; 34 non-inline-C fails to close).
- **`run-turi.sh` summary** -- must stay green every step.
- **`check_turi_parity.py`** -- must stay 0-gaps.
- **`tests/run.sh`** -- must stay green (additive natives should not touch codegen;
  if any fix shifts a snapshot, regenerate in the same PR per CLAUDE.md).

---

## See Also

- [turi-interpreter-gap-closure-plan.md](turi-interpreter-gap-closure-plan.md) --
  parent (W1-W5; this decomposes W5's residual).
- [docs/archive/history/turi-harness-flip-reconciliation.md](../archive/history/turi-harness-flip-reconciliation.md)
  -- historical catalogue of the flip.
- [v1/turi-eval-trampoline-plan.md](v1/turi-eval-trampoline-plan.md) -- relevant to
  the R4 continuation set.
- `src/main.c` `cmd_eval` (`:4801`), the `wk_register_*_natives` cluster
  (`:5106-5139`, defs from `:5616`), `wk_register_safe_natives` (`:9117`).
- `src/turi/eval.c` fiber path (`:691+`, `async_fiber_thunk` `:198`).
