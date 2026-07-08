# Interpreter never reclaims Vec/Set/Map buffers (unbounded leak)

**Severity:** medium (correctness-adjacent; unbounded memory growth for any
long-lived interpreter host that uses collections). Not a miscompile -- results
are correct, memory is not.

## Summary

In the tree-walking interpreter (`tur interpret`, and every `turi_eval` /
`libturi` embedder once collections are exposed there), a `Vec` / `Set` / `Map`
buffer is created with raw `calloc`/`malloc` and is **never freed automatically**
-- not on scope exit, not at `turi_env_free`. It persists for the process
lifetime unless the program explicitly calls `vec-free` / `set-free` /
`map-free`. A loop that builds and drops collections grows memory without bound.

## Repro

```turmeric
;; tur interpret this file
(defn fill [v : (Vec int) k : int] : int
  (if (< k 200) (do (vec-push! v k) (fill v (+ k 1))) 0))
(defn loop [i : int] : int
  (if (< i 60000)
    (let [v : (Vec int) (vec-new)]   ; transient: dropped from scope each iter
      (fill v 0)
      (loop (+ i 1)))
    0))
(defn main [] : int (loop 0))
```

Peak `VmRSS` under `tur interpret`, varying the loop bound N:

| N (collections created + dropped) | peak VmRSS |
| --- | --- |
| 5,000  | 508 MB |
| 20,000 | 1,039 MB |
| 60,000 | 2,004 MB |
| 60,000 (scalar control, no vec) | 133 MB |

Peak grows ~linearly with N; the scalar control stays flat. (Note: `tur run`
compiles, so use `tur interpret` to exercise the tree-walker.)

## Root cause

- `native_vec_new` (`src/main.c:8825`) allocates with `calloc`, ignores `env`,
  and does not register the buffer anywhere. `native_set_*` / `native_map_*` sit
  on the HAMT runtime and are likewise untracked.
- The interpreter's rc-drop path frees nothing here: `EX_RC_DROP`
  (`src/turi/eval.c:7954`) and `turi_rc_drop_value` (`src/turi/eval.c:610`) act
  only on the `__rc` `TURI_STRUCT` wrapper. A collection handle is a bare
  `TURI_INT` carrier, so both are no-ops for it.
- `turi_env_free` (`src/turi/env.c:239`) frees arenas, the scheduler, and spice
  images -- there is no pass over collection buffers.
- No GC, no refcount for these carriers.

So the buffers are orphaned the moment their handle leaves scope, and stay
resident until the process exits.

## Relationship to other work

- Orthogonal to the value-pool scratch-promotion work
  (`docs/upcoming/turi-value-pool-carrier-relocation-plan.md`): these buffers are
  outside `value_scratch`, so a collection global neither grows scratch nor is
  reclaimed by promotion.
- Surfaces prominently once collections are exposed on the `libturi` path
  (`docs/upcoming/turi-interp-collections-libturi-plan.md`), which inherits this
  limitation unchanged.

## Fix directions

1. **Env-tracked buffers.** Record each collection buffer on the env at
   `native_*_new` time (a small intrusive list, like `turi_env_track_coro_stack`
   already does for coroutine stacks) and free them all at `turi_env_free`. Bounds
   the per-env total to live+dead-since-last-teardown; does not bound a single
   immortal env mid-run, but fixes the create/teardown leak and the test-harness
   leak gate.
2. **Drop-glue for collection carriers.** Teach the interpreter's `EX_RC_DROP` /
   drop path to recognize the `:heap` collection carriers and call the existing
   `vec-free` / `set-free` / `map-free` on scope exit, matching the compiled
   path's ownership discipline. Bounds steady-state memory for transient
   collections but needs the same care about shared/aliased handles the compiled
   RC path takes (a `Vec` handle is a shared mutable pointer).

Option 1 is the smaller, safer first step; option 2 is the real steady-state fix.
