# Prerequisite tasks to de-risk the three open turi-* reports

A planning pass over the three still-open turi interpreter reports, decomposing
each big lift into smaller, independently-landable groundwork that eases (and
de-risks) the eventual fix. Grounded with file:line pointers and current
measurements taken 2026-06-12 against `./build/tur`.

Open reports surveyed:

- [docs/reported/turi-inline-c-silent-miscompiles.md](../../reported/turi-inline-c-silent-miscompiles.md)
- [docs/reported/turi-map-set-hamt-interpreter-gap.md](../../reported/turi-map-set-hamt-interpreter-gap.md)
- [docs/reported/turi-harness-flip-reconciliation.md](../../reported/turi-harness-flip-reconciliation.md)

---

## Report 1 -- inline-C silent miscompiles (W4 matcher tightening)

**Goal:** make each `ic_exec_*` matcher in `try_exec_simple_inline_c`
(`src/turi/eval.c`) refuse-rather-than-guess, so a body it cannot evaluate
faithfully flips from silent-wrong (rc=0) to a clean "inline-C not supported"
error (then it carves cleanly).

### Prereq 1a -- matcher claim trace (LANDED 2026-06-12)

`TUR_IC_TRACE=1` now logs which matcher claimed each inline-C body and what it
returned (`ic_claim`, `src/turi/eval.c`). Off the trace path it is a single
cached-flag branch. This is the linchpin: it turns the "delicate, all-at-once"
tightening into a per-matcher loop you can verify (a tightening should flip a
mis-claim to *unclaimed*, never disturb a correct claim).

### Prereq 1b -- the report's "22 remain" is stale; it is **20** (data below)

Running the trace + an rc/stdout classification over the 24 listed fixtures:

- **4 already flipped to clean errors** (no longer silent miscompiles), so they
  should be dropped from the report's list: `instance-head-hole-pair` (rc=1,
  free-defn collision), `stdlib-test-runner-registry`, `weak-dangling`,
  `weak-upgrade-option` (all `inline-C not supported`, rc=1). These now *claim
  nothing* -- the matcher already declines them.
- **20 still silently miscompile** (rc=0, wrong stdout).

**Matcher -> fixture map for the 20 (claim trace):**

| Matcher (`ic_exec_*`) | Count | Fixtures |
| --- | --- | --- |
| `constructor` | 12 | `backtrack-bind/-depth/-depth-exceeded/-do-macro/-guard/-interleave/-nested/-once` (8), `arrow-instance-apply`, `arrow-instance-loop-nonrecursive`, `workstealing-metrics`, `workstealing-steal` |
| `snprintf` | 4 | `show-float`, `show-list`, `show-pair`, `exg5-rc-in-exists` |
| `accessor` | 2 | `panic-catch-unwind-caught`, (`arrow-instance-apply` also hits accessor) |
| `simple-return` | 2 | `closure-capture-byptr-struct-param`, `inline-c-cname-splice` |
| mixed (free+constructor+accessor) | 1 | `stdlib-slice-runtime` |

(The `backtrack-*` cluster also fires `linked-list-print` once each -- a
secondary claim on a printf helper -- but the wrong *answer* comes from the
`constructor` claims.)

### Prereq 1c -- prioritize by matcher, not by fixture

The map shows **two matchers cover 16 of 20**: tightening `ic_exec_constructor`
(12) and `ic_exec_snprintf_fmt` (4) is the highest-leverage first slice.
`ic_exec_constructor` mis-claims the `backtrack-*` monad bodies (multi-statement
control flow it cannot model) -- the cleanest single win is to reject a body
with >1 meaningful statement or control flow it does not interpret.

### Prereq 1d -- a "known-good inline-C" regression set

Before tightening, pin the inline-C fixtures the matchers currently handle
**correctly** so a tightening cannot regress them. From the allowlist these
include `inline-c-binop`, the `gen-*` family, and (post the conditional-snprintf
fix) `range-bound-show-ord`. Capture them as an explicit list the W4 PR re-runs;
the trace makes "still correctly claimed" easy to assert.

---

## Report 2 -- map/set/hamt under `--interpret` (Gap 2: C-callback eq/hash)

**Goal (Tier B):** content-keyed maps whose `MapKey` comparator is a *turi
closure* (not a C function pointer) must work, because the runtime HAMT takes
equality as a raw `bool(*)(int64_t,int64_t)` (`src/runtime/hamt.h:25`).

Tier A (scalar-keyed map/set/hamt) already landed. The remaining prereqs:

### Prereq 2a -- thread a `void* ctx` through the HAMT eq callback (ABI groundwork)

The single hardest dependency is that `tur_hamt_keyeq_fn` has no context
parameter, so a turi closure+`TuriEnv*` cannot ride along to the collision-time
`eq(k1,k2)` call. A mechanical, behavior-preserving refactor unblocks everything
downstream:

- Add a parallel context-carrying entry-point family, e.g.
  `tur_hamt_set_eq_ctx(Hamt*, hash, key, val, bool(*eq)(int64_t,int64_t,void*), void* ctx)`,
  alongside the existing `tur_hamt_*_eq[_o]` (`src/runtime/hamt.h:144-186`). The
  compiled path keeps calling the no-ctx form (or passes `ctx=NULL` and a thunk
  that ignores it), so **no codegen change and no fixture regen**.
- This is a runtime-C change only (`src/runtime/hamt.c`), independently testable
  with a C-level unit, and lands with zero interpreter wiring. Once it exists,
  the interpreter's `tur_hamt_set_eq_turi` can pass `ctx = {env, closure}` and a
  trampoline `eq` that calls `turi_call`.

### Prereq 2b -- close the non-int map *values* item (independent of Gap 2)

`Map int cstr` / `Map int float` mis-render because `map-get`'s generic int64
carrier is not reinterpreted by `(:: ... :V)`. This is filed at
[../../archive/history/turi-map-nonint-value-carrier-ascription.md](../../archive/history/turi-map-nonint-value-carrier-ascription.md)
and is **orthogonal to the comparator problem** -- it is a value-carrier
ascription fix in `EX_GET_FIELD`/the map-get native, not a HAMT-callback change.
Landing it widens scalar-key map coverage (non-int values) without touching
Gap 2, and shrinks the Tier B surface to purely the comparator.

### Prereq 2c -- an explicit, guaranteed clean error for content-keyed maps

Per CLAUDE.md, the "works by luck until a hash collision" native must not ship.
A safe interim: have the interpreter's map natives detect a *non-native*
(turi-closure) comparator and raise a clean, explicit "content-keyed map keys
are not yet supported under --interpret" error -- a documented carve, not a
crash. This lets `map.tur`/`set.tur` join the `cmd_eval` prelude **now** (closing
the harness-flip "missing native" bucket for scalar keys) while Tier B is still
open behind an honest guard.

---

## Report 3 -- allowlist->denylist harness flip

**Goal:** run every fixture under `--interpret` minus markers, green. Current
measured state (from the report): ~660 pass / ~910 fail / 92 skip. The report
already sketches a roadmap; these prereqs make each step independently landable.

### Prereq 3a -- native-registry parity diff (the report's own TODO)

The "12 unknown function + unbound variable {vec-of,set-of,hamt-of,map-count,
grid-new,...}" bucket is almost entirely "the typed-stdlib modules are not
preloaded" -- those names are stdlib variadic builders
(`elab_toplevel.c:471/505/509`), not compiler builtins. Produce a small
diff/test that lists compiler-auto-loaded stdlib exports
(`compile_to_c()`, `src/main.c:646`) minus interpreter-registered natives
(163 `turi_env_register_native` calls in `src/main.c`). The output *is* the
preload work list, and as a checked-in test it ratchets against future drift.

### Prereq 3b -- benchmark-stub overlap audit

`cmd_eval` injects no-op stubs (`vec-get`, `vec-set!`, `hamt-*`, `ok?`, `some?`,
...) for stdlib-free benchmark scripts. Preloading the real modules then trips
`defn: '<x>' is already defined by an auto-loaded stdlib module`. Enumerate
exactly which stubs collide with real stdlib defns (vs the genuinely
benchmark-only ones: `run-ring`/`run-nbody`/`io-*`/`random-access-bench`/...), so
the prelude unification deletes precisely the overlap. A concrete list is the
prereq; the deletion is mechanical once it exists.

### Prereq 3c -- opt-in full-prelude flag

Wire the typed-stdlib `(load ...)` prelude behind `TUR_TURI_FULL_PRELUDE=1`
(off by default, given the ~300ms/invocation cost noted in the report). This
lets the prelude be iterated and measured fixture-by-fixture without committing
the cost to every interpreter run, and turns "recover the typed-stdlib bucket"
from a flag-day into an incremental, reversible rollout. Use `(load ...)` (not
`turi_eval_file`) so per-file `file_id`s keep multiple defmodule-carrying
modules from colliding (same root cause as the landed TI8.b defmodule fix).

### Prereq 3d -- carve markers for the genuine-divergence buckets

The move/linearity-checker divergence (~20) and `if condition must be bool, got
int` (13) are interpreter/type-check divergences, not quick fixes. Add
`requires.tur-only` markers to those fixtures now so the flip can land green with
them explicitly excluded, separating the "carve" decision from the "fix"
decision and keeping the denylist honest.

---

## Suggested ordering (cheapest, highest-leverage first)

1. **1a (landed)** + **1b/1c** -- the trace is in; fold the corrected count and
   matcher map into the inline-C report, then tighten `ic_exec_constructor`
   first (12 fixtures).
2. **3a** -- the native-registry diff: a small checked-in test that quantifies
   the biggest recoverable harness-flip bucket and guards against drift.
3. **2b** -- non-int map values: orthogonal, self-contained, widens map coverage.
4. **2a** -- HAMT `ctx` ABI: mechanical runtime-C refactor, no codegen/fixture
   churn, unblocks Tier B.
5. **3b/3c/3d** -- prelude unification groundwork for the harness flip.
