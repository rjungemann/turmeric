## Turmeric Godot Binding -- AOT Cleanup Plan (Tier 1)

> **Status:** Draft Plan
> **Last Updated:** 2026-06-25
> **Type:** Integration / Game Engine -- post-v1 follow-up to
> [godot-language-binding-plan.md](./v1/godot-language-binding-plan.md)
> and the archived
> [godot-binding-aot-plan.md](../archive/godot-binding-aot-plan.md).

---

## Why this exists

A1-A6 of the AOT plan landed end-to-end: scripts AOT-compile, dlopen,
dispatch, and persist their inspector/signal metadata across reloads.
What surfaced during the bench session was three friction points small
enough that they don't merit their own multi-phase plans but big enough
that real adoption hits them on day one. Bundling them so they can be
picked up in a single short pass.

The microbenchmark also stopped well short of the plan's 5x success
criterion -- not because the implementation is wrong, but because the
bench body is too cheap to surface the win. Adding a heavier-body bench
fixture closes that audit gap.

---

## Phases

### Phase T1.A -- `defmodule` interp dispatch parity -- SKIPPED (verified non-issue)

**Status (2026-06-25):** investigated and skipped. The premise in the
original draft was wrong.

Re-reading `src/turi/eval.c:6458-6517`, the `EX_FN_DEF` case already
handles both shapes correctly:

- A `(defn hot ...)` listed in the enclosing defmodule's `(export ...)`
  list sets `exported=true`, leaves `qkey=NULL`, and falls through to
  `turi_env_set(env, fname, v)` -- bound under the bare name.
- A `(defn priv ...)` NOT in `:exports` gets `qkey = "modname/priv"`
  AND publishes a bare-name fallback when the bare slot is free
  (lines 6511-6513). So `lookup_method("priv")` still resolves.

Empirically confirmed during the bench session: the interpreter run of
`bench.tur` (wrapped in `(defmodule bench (export _ready hot) ...)`)
produced `acc=1000000`, meaning `hot()` did dispatch through interp's
bare-name lookup. The "silently breaks in interpreter mode" symptom
the draft described does not occur.

Leaving the section in place rather than deleting so future readers
don't re-investigate the same false trail. Real issues with cross-
module bare-name collisions (e.g. two modules both declaring `step`)
are tracked under the Tier 4 cross-script plan, not here.

### Phase T1.B -- AOT dispatch lookup cache (~1-2 days)

`aot_dispatch.cpp` runs `find_by_name` linear-scanning the exports
vector on every call. A per-`TurmericInstance` LRU keyed by `StringName`
saves the scan on the hot loop -- empirically ~30 ns/call out of the
~260 baseline, ~10% of total. Invalidate when the underlying
`aot_image_` pointer changes (i.e., on reload).

Implementation sketch:

```cpp
struct TurmericInstance {
    ...
    const aot::AotImage *cached_image = nullptr;
    struct CachedHit { StringName name; const aot::AotExport *ex; };
    std::array<CachedHit, 8> aot_cache;
    uint8_t aot_cache_head = 0;
};
```

Tiny ring; eight entries cover any plausible per-instance method
fan-out. On cache miss fall through to `find` / `find_by_name` as
today, then insert at `aot_cache[aot_cache_head++ & 7]`.

### Phase T1.C -- Heavier-body bench fixture (~1 day)

Add `examples/aot-bench-heavy/` mirroring `aot-bench/` with a body that
walks more than 1-2 AST nodes per call. Two candidates:

```turmeric
;; ~100 inline arithmetic ops -- exercises the per-call AST walk.
(defn hot [x : int] : int
  (+ x 1 2 3 4 5 6 7 8 9 10
     11 12 ... 100))

;; Recursive sum-to-N -- exercises the closure-call stack.
(defn hot [n : int] : int
  (if (= n 0) 0 (+ n (hot (- n 1)))))
```

The recursive form is the better test: it exercises the call frame
machinery, which is where the interp's per-frame cost compounds. Use
a moderate N (e.g. 50) to keep total wall-time reasonable while
forcing 50 nested calls per outer iteration.

Update `examples/aot-bench/README.md` to point at the heavy bench as
"the one that actually shows AOT speedup" and keep the light bench as
the smoke test.

---

## Risks

- **Bare-name aliasing collisions.** Two scripts in the same env
  declaring `(defmodule a (defn step ...))` and `(defmodule b (defn step ...))`
  would clash on the bare `step` name. In the turmeric-godot binding
  each script has its own `TuriEnv`, so this is a non-issue today; the
  risk applies to whoever embeds libturi with shared envs across
  scripts.
- **Cache invalidation timing.** If `TurmericScript::_reload` swaps
  `aot_image_` while a call is in flight (no realistic path today, but
  worth noting), a stale cache entry could survive into the next call.
  Mitigation: clear the per-instance cache from `_reload` *before* the
  new image loads.

---

## Success Criteria

- `examples/aot-bench-heavy/` shows AOT at least 3x faster than
  interpreter at the recursive workload. (5x at body weights where the
  Variant fixed cost ~250 ns becomes a small fraction of total.)
- `find_by_name` does not appear in a hot-loop profile of the bench.

---

## Out of Scope

- General AOT performance tuning beyond the dispatch cache. Variant
  marshalling overhead is structural to Godot's call protocol and is
  the long pole regardless of language; not chased here.
- Cross-script symbol resolution (Tier 4 territory).
- Typed exports / typed return values from AOT calls. Currently every
  return is `Variant`-marshalled per `ret_type`; deeper typing is the
  Tier 2 facade plan's job.
