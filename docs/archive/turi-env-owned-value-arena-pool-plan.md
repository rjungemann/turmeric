# Plan: env-owned value-arena pool for the turi interpreter

**Status:** Phase 1 LANDED; Phase 2 deferred (optional). **Area:** `src/turi/` (tree-walking interpreter).

## Phase 1 -- landed

Implemented as designed below:

- `TuriEnv` gained an env-owned `Arena value_arena` (`env.h`), created in
  `turi_env_new` and reclaimed in `turi_env_free`, plus a process-global
  fallback pool the env adopts on free for the env-less error/rejection
  constructors. Allocators: `turi_val_alloc` / `turi_val_calloc` /
  `turi_val_strdup` (env) and `turi_val_strdup_global` (`value.c`/`value.h`).
- Escaping payloads routed through the pool: error/rejection strings; every
  retained `TuriClosure`; `TuriStruct` + fields (env threaded through
  `make_struct_val_def` / `make_struct_val` / `turi_default_of` /
  `turi_make_struct`); captured `EvalFrame` + `EvalBinding` (env threaded
  through `eval_frame_new` / `frame_bind` / `clone_frame_bindings` /
  `clone_ws_slice`); `TyvarBind`; the catch/panic Result boxes + payload;
  first-class handler values; ADT/set/cons-list payloads; the rc control block;
  `ptr-of` boxes; and the delimited-control / generator / STM-tvar / async-fiber
  structs (`TuriCont`, `TuriWsCont`, `TuriEffectCont`, `TuriGen`, `TuriTVar`,
  `TuriFiber`, `HandleExpr`/`HandleCase`). Guard-path `free()`s on now-pooled
  pointers were dropped; driver-freed transients (scratch arg/field
  accumulators, continuation/effect-handle stacks, defer items) were left on
  `malloc`/`free`.
- New leak-on regression gate: `tests/turi/env-teardown.c` +
  `tur_env_teardown` ctest (forces `ASAN_OPTIONS=detect_leaks=1`) +
  `tests/run-turi-env-teardown.sh`. Loops new/eval/free over a script that
  builds closures, structs, ADTs, and error values and asserts zero leaks.
- Validation: `tests/run.sh` 1788/0; `tests/run-turi.sh` matches baseline
  (no new failures); the teardown gate is leak-clean.

Known remaining residue (NOT covered by Phase 1, follow-up): the inline-C
emulation buffers in `ic_exec_*` (env-less helpers, only reachable via the
TI7 inline-C carve-out) and the mmap/malloc coroutine *stacks* backing fibers
and generators (the structs are pooled; their large stacks are not, and mmap
is untracked by LSan). See `docs/reported/turi-value-pool-residual-sites.md`.

**Area:** `src/turi/` (tree-walking interpreter).
**Goal:** make `turi_env_free` reclaim *all* memory an evaluation produces, so an
embedding C host can deallocate an interpreter session in one shot instead of
relying on process exit.

## Background -- the current memory model

turi is intentionally "never-free" at the value level: it allocates `TuriValue`
payloads with raw `malloc`/`calloc`/`strdup` and leans on process exit to
reclaim them. The harnesses that drive it default to
`ASAN_OPTIONS=detect_leaks=0` for this reason.

What `TuriEnv` (`src/turi/env.h:104`) already *owns* and what
`turi_env_free` (`src/turi/env.c`) already reclaims in bulk:

- `eval_arenas` -- a linked list of per-call `Arena`s (`ArenaNode`,
  `env.h:67-70`) holding elaborated ASTs / `elaborate_program` output and most
  per-call working memory. Freed node-by-node with `arena_free`.
- `sym_arena` + `SymbolTable`, the `globals` list + `globals_ht` slots, the
  async scheduler (`turi_sched_free`), dlopen'd spice images, `src_acc`.

What is **not** owned by the env and therefore leaks across `turi_env_free`
(the "ambient" residue) -- all raw heap, no central registry:

- error / rejection strings: `strdup` in `src/turi/value.c:11,20,24`.
- `TuriClosure`: ~10 `malloc`/`calloc` sites in `eval.c` (132, 4155, 5733,
  6414, 6451, 6460, 7086, 7589, 8461, ...).
- `TuriStruct` + its `fields` array: `eval.c:504,508,536`; ADT/record field
  arrays, boxes, cons cells, accumulator arrays (797, 814, 1621, 2336, 3207,
  4363, 5126, 5150, 5212, ...).

Note `eval.c` already issues ~78 `free` calls: genuinely transient buffers
(finished continuation/generator stacks, scratch arg arrays) are freed inline
today. Those are **not** the leak; the leak is the escaping/untracked payloads
above.

## Goals

1. After `turi_env_free(env)`, the process holds **zero** turi-attributable live
   allocations (verifiable under LeakSanitizer with `detect_leaks=1`).
2. Enable the clean embedding pattern: a host creates a `TuriEnv` per unit of
   work, evaluates, reads results out, then `turi_env_free`s -- reclaiming
   everything, repeatable in a long-lived process with no growth across units.
3. No change to interpreter semantics or to the value representation visible to
   host code (`TuriValue` stays a tagged union passed by value).

## Non-goals

- Bounding peak memory **within a single long-lived env** (a notebook kernel
  sharing one env across thousands of cells). Phase 1 makes teardown clean but
  an immortal env still grows; see Phase 2 for the optional bound.
- Reference counting or a tracing GC. That is a larger ownership-model change,
  out of scope here.
- Touching the compiled/codegen path, which is already leak-clean and
  leak-checked.

## Design

### Phase 1 -- route value payloads through an env-owned pool

Add a dedicated arena pool on `TuriEnv` for value-level allocations, distinct
from `eval_arenas` (which holds AST/elaboration memory with its own lifetime):

```c
/* env.h, inside TuriEnv */
ArenaNode *value_arenas;   /* pool for TuriValue heap payloads; freed in
                            * turi_env_free, like eval_arenas */
```

Provide a small allocation API in `value.c` that all payload sites call instead
of raw libc:

```c
/* value.h */
void  *turi_val_alloc(TuriEnv *env, size_t n);          /* arena bump */
char  *turi_val_strdup(TuriEnv *env, const char *s);    /* arena copy */
TuriClosure *turi_val_closure(TuriEnv *env);            /* zeroed closure */
TuriStruct  *turi_val_struct(TuriEnv *env, uint32_t nfields);
```

Each grabs from the tail `value_arenas` node, chaining a fresh arena when the
current one is exhausted (mirror the existing `eval_arenas` growth logic). The
pool is created in `turi_env_new` and torn down in `turi_env_free`:

```c
/* turi_env_free, alongside the existing eval_arenas loop */
ArenaNode *vn = env->value_arenas;
while (vn) { ArenaNode *next = vn->next; arena_free(&vn->arena); free(vn); vn = next; }
env->value_arenas = NULL;
```

### Migration: which allocations move, which stay

| Allocation | Today | After |
|---|---|---|
| error/rejection strings (`value.c:11,20,24`) | `strdup` | `turi_val_strdup(env, ...)` |
| `TuriClosure` (eval.c) | `malloc`/`calloc` | `turi_val_closure(env)` |
| `TuriStruct` + `fields` (eval.c) | `malloc` | `turi_val_struct(env, n)` |
| ADT/record/box/cons/accumulator payloads | `malloc` | `turi_val_alloc(env, n)` |
| continuation/generator stacks (mmap/large) | `malloc`/`mmap` + inline `free` | **unchanged** |
| scratch arg arrays freed inline before return | `malloc` + inline `free` | **unchanged** |

The rule: things that **escape** (could end up in a global, a closure capture, a
return value, or the host's hands) move to the pool. Things that are provably
**transient within one C call** and already explicitly freed stay as
`malloc`/`free` -- arenas cannot free individuals, so keeping bounded transients
out of the pool preserves today's steady-state behavior inside long evals.

### Interaction with the existing inline `free`s

Any inline `free(p)` whose `p` now comes from the pool must be deleted (freeing
an arena pointer is a bug). Audit the ~78 `free` sites in `eval.c`: a `free`
that targets a migrated payload type is removed; a `free` of an unmigrated
transient stays. This audit is the bulk of the review surface and the main risk
(see Risks).

`turi_val_alloc` must require a non-NULL `env`. A few payload sites today have no
`env` in scope (e.g. `turi_error(const char *)` in `value.c` is env-less by
signature). Thread `env` to them, or, where that is too invasive, keep a tiny
fallback bump-pool reachable from a single process-global the env adopts on
`turi_env_free`. Prefer threading `env`; it keeps ownership honest.

### Phase 2 (optional) -- bound a single long-lived env

Phase 1 makes teardown clean but does not shrink a never-destroyed env. The
optional two-region (scratch/permanent + escape-promotion) follow-up that bounds
a notebook-kernel-style host was split into its own plan:
`docs/upcoming/turi-value-pool-scratch-promotion-plan.md`. It is deferred
(only matters for the immortal-env host pattern); Phase 1 stands alone and
delivers goals (1) and (2) without it.

## API / behavior changes

- New: `turi_val_*` allocators (internal to `src/turi/`).
- Changed: `turi_env_free` additionally frees `value_arenas` (and, Phase 2, both
  regions). No signature change; existing embedders get clean teardown for free.
- Unchanged: `TuriValue`, `turi_env_new`, `turi_eval_*`, value semantics, and
  the host-visible lifetime contract ("values valid until `turi_env_free`",
  `eval.h:19`) -- now literally true rather than true-modulo-leaks.

## Testing

- New harness `tests/run-turi-env-teardown.sh`: in one process, loop N times
  { `turi_env_new`; evaluate a script that builds closures, structs, ADTs, and
  raises caught errors; read a result; `turi_env_free`; }. Run under
  `ASAN_OPTIONS=detect_leaks=1` and assert **zero** leaks -- the inverse of the
  current `detect_leaks=0` default, now achievable.
- Flip the turi ctest targets (`tur_turi_*`, `tur_flags_*`) to
  `detect_leaks=1` once Phase 1 lands, or add a dedicated leak-on variant so the
  property does not regress. Keep the legacy `detect_leaks=0` default only for
  paths that intentionally exit without `turi_env_free` (one-shot CLI eval).
- Re-run the full suite (`bash tests/run.sh`, 10-min timeout) -- value-path
  changes touch every interpreter fixture.

## Risks

- **Over-eager `free` removal / double-free.** The inline-`free` audit is the
  sharp edge: delete a `free` of a still-malloc'd transient and you reintroduce
  a leak; keep a `free` of a now-arena pointer and you crash. Mitigate by
  migrating one payload kind at a time, each with a leak-on test run.
- **Per-call growth regression (Phase 1 only).** Long-running single evals that
  produced many short-lived values used to free them inline; pooling defers
  reclamation to `turi_env_free`. Acceptable for the per-unit-env pattern;
  Phase 2 addresses the immortal-env case.
- **Env threading churn.** Some payload sites lack `env`; threading it reaches
  into several call chains. Bounded but wide.
- **Arena alignment/size for `TuriStruct` flexible field arrays** -- ensure
  `turi_val_struct` returns suitably aligned, contiguous storage.

## Alternatives considered

- **Per-value refcounting** -- precise and bounds long-lived envs, but turi
  aliases values freely (env slots, captures, returns, continuations); retro-
  fitting correct inc/dec across every site is far larger and bug-prone.
- **Tracing GC** -- best peak-memory story, largest effort; needs a precise root
  set over env globals, the work-stack, handler/defer stacks, and fibers.
- **Status quo + `detect_leaks=0`** -- zero work, but leaves teardown leaky and
  blocks any embedder that wants a clean long-lived host process.

## Recommendation

Land **Phase 1** (env-owned `value_arenas` + `turi_val_*` + the `free` audit):
it is self-contained, makes `turi_env_free` leak-clean, and unlocks the clean
per-unit-env embedding pattern with a leak-on regression test. Treat **Phase 2**
(scratch/permanent regions + escape promotion) as a follow-up only if a
single-immortal-env host (e.g. a notebook kernel) becomes a real requirement.
