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

### Prereq 1b -- the report's "22 remain" is stale; it is **20** (data below) (FOLDED 2026-06-12)

The corrected count and the matcher->fixture map below have been folded into
[the report](../../reported/turi-inline-c-silent-miscompiles.md) (the stale
"22 remain" is marked superseded; the table now lives in the report body).
Re-verified against `./build/tur`: 5 of 25 are clean errors (rc=1), 20 still
silently miscompile (rc=0).

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

> **LANDED 2026-06-12.** Tier B is closed for the runnable case (pure-turi
> closure comparators). The 2a `ctx` ABI + a trampoline in the map natives
> (`map_turi_eq_tramp`, `src/main.c`) detect a `TURI_CLOSURE` comparator and
> route through `tur_hamt_*_eq_ctx`, calling the closure via `turi_call` on every
> collision. New allowlisted fixture `tib-map-turi-comparator` (custom
> equality-by-x under a forced hash-0 chain) passes under `--interpret`; harness
> 982 -> 983, 0 failed; compiled 1599/0. The only residual is *inline-C*
> comparators (struct-key fixtures that deref a boxed pointer) -- inherently
> interpreter-bound, failing cleanly per 2c, not a Tier B gap.

Tier A (scalar-keyed map/set/hamt) already landed. The remaining prereqs:

### Prereq 2a -- thread a `void* ctx` through the HAMT eq callback (ABI groundwork) (LANDED 2026-06-12)

**Landed.** `src/runtime/hamt.h`/`.c` now expose a `tur_hamt_*_eq_ctx` family
(`set`/`del`/`has`/`get`) taking `tur_hamt_keyeq_ctx_fn = bool(*)(int64_t,int64_t,void*)`
plus a `void *ctx`. A new thread-local `g_hamt_key_eq_ctx_fn`/`g_hamt_key_eq_ctx`
hook is consulted first in `keys_equal`; the plain and owned `_eq` families now
save/clear it for their scope (and the ctx family clears the no-ctx hook), so
the two comparator families are mutually exclusive per dynamic scope and nest
correctly. The compiled path is untouched -- it keeps calling the no-ctx form --
so **no codegen change and no fixture regen**. C-level unit:
`tests/test_hamt_eq_ctx.c` (gate `tests/run-hamt-eq-ctx.sh`, ctest
`tur_hamt_eq_ctx`) proves ctx is threaded to every collision compare,
content-keyed get/has/set/del work, `ctx == NULL` is a clean passthrough, and a
plain op nested inside a ctx comparator does not leak the outer hook. The
interpreter's `tur_hamt_set_eq_turi` can now pass `ctx = {env, closure}` and a
trampoline `eq` that calls `turi_call`.

Original groundwork notes:

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

### Prereq 2b -- close the non-int map *values* item (independent of Gap 2) (ALREADY RESOLVED -- #341)

**Already resolved** -- landed in commit #341 via *direction 2* (the
`EX_ASCRIBE` node), ahead of this doc's writing. The fix was upgraded from
"ambiguous" to root-correct: the compiled `::` is a *representation assertion*
that bit-reinterprets the carrier word, while numeric int<->float conversion is
the separate `EX_CAST` node. `src/turi/eval.c` `case EX_ASCRIBE` now
bit-reinterprets the int64 carrier for `:float`/`:float64` (int64->double via a
union) and adds a `:cstr` arm (int64->`char*`); `:float32` keeps the numeric
form to match the compiled float32 ascription. Verified 2026-06-12:
`tce3-map-cstr-val` passes under `--interpret` (output `alpha/gamma/3/0.5/delta/4`)
and is on the `run-turi.sh` allowlist (line 901); the interpreter harness is
**982 passed, 0 failed**. Full write-up + the *inherent* `EX_REINTERPRET`
limitation (`::` is value-preserving, not bit-preserving, under the interpreter):
[../../archive/history/turi-map-nonint-value-carrier-ascription.md](../../archive/history/turi-map-nonint-value-carrier-ascription.md).

Original framing: `Map int cstr` / `Map int float` mis-rendered because
`map-get`'s generic int64 carrier was not reinterpreted by `(:: ... :V)` -- a
value-carrier ascription fix, orthogonal to the comparator problem (Gap 2). With
it resolved, the Tier B surface is purely the comparator.

### Prereq 2c -- an explicit, guaranteed clean error for content-keyed maps (GUARANTEE ALREADY MET; message clarified 2026-06-12)

Per CLAUDE.md, the "works by luck until a hash collision" native must not ship.
Investigated 2026-06-12 -- **the clean-error guarantee already holds, and the
enabling goal is already done:**

- **`map.tur`/`set.tur` are already in the `cmd_eval` prelude** (verified by
  prereq 3a; only `contract`/`mutmap`/`json`/`schema` remain carved). The
  scalar-key "missing native" bucket is closed.
- **A content-keyed *user* comparator already produces a clean `rc=1` error, not
  a crash or silent miscompile.** Verified against `wkc3-struct-map-key`,
  `eqmap-struct-content`, `eqmap-struct-float-fields` under `--interpret`.
- **The literal mechanism in 2c (a guard inside the map natives) is moot.** As
  the umbrella report diagnoses, the failure surfaces *earlier*, at `mk-cmp`:
  its synthesized closure body returns a captured C function-pointer address via
  inline-C, which the tree-walker declines (`EX_INLINE_C`) **before**
  `map-assoc-eq-o` and friends are ever called. So a guard in the map natives
  would never run for this case.

What landed: the generic interpreter inline-C error
(`src/turi/eval.c` `case EX_INLINE_C`) is now **actionable** -- it points the
user at `tur build`/`tur run`, which implement these natively. No fixture
asserts on the message, so no regen. A map-*specific* wording ("content-keyed
map keys are not yet supported") was considered and **declined**: the only site
with function context is `try_exec_simple_inline_c`, and gating a map-specific
message there requires fragile name/body matching on synthesized comparator
closures that could misfire on unrelated capture-returning inline-C -- low value
(the error is already clean and now actionable) for real risk. The genuine
remaining work is **Tier B itself** (the turi-closure-aware HAMT path, now
unblocked by the 2a `ctx` ABI), not an interim guard.

---

## Report 3 -- allowlist->denylist harness flip

**Goal:** run every fixture under `--interpret` minus markers, green. Current
measured state (from the report): ~660 pass / ~910 fail / 92 skip. The report
already sketches a roadmap; these prereqs make each step independently landable.

### Prereq 3a -- native-registry parity diff (the report's own TODO) (LANDED 2026-06-12)

The "12 unknown function + unbound variable {vec-of,set-of,hamt-of,map-count,
grid-new,...}" bucket is almost entirely "the typed-stdlib modules are not
preloaded" -- those names are stdlib variadic builders, not compiler builtins.

**Landed** as `tools/check_turi_native_parity.py` (run from `tests/run.sh`
next to `check_turi_parity.py`). It parses both module lists out of
`src/main.c` -- the compiled auto-load `stdlib_files[]` and the
`--interpret`/`tur eval` `prelude[]` -- and computes the preload gap. The
interpreter prelude has since grown to cover almost the entire compiled set, so
the gap is now just **4 modules**, all intentionally carved with a rationale in
`docs/turi-preload-carve-out.txt`:

| Gap module | Why not preloaded | Names it would add |
| --- | --- | --- |
| `contract.tur` | `tur-contract-check` inline-C conflicts with `:pre/:post` lowering | (contract macros) |
| `mutmap.tur` | needs native overrides over `tur_hamt_*`; content-keyed path awaits Tier B (2a/2c) | `mutmap-new/-set!/-get/...` |
| `json.tur` | reader-backed, `-X`-gated -- preload only behind the same flag | `json/encode`, `json/decode`, ... |
| `schema.tur` | reader-backed, `-X`-gated, depends on `json.tur` | `schema/*`, `schema-decode*` |

The check ratchets both ways: an undocumented gap (compiled auto-load widened
without teaching the interpreter) fails, and a stale carve-out (module now in
the prelude but still listed) fails -- same discipline as `docs/turi-carve-out.txt`.
Run `python3 tools/check_turi_native_parity.py --worklist` for the full
name-level preload work list. The original "12 unknown" bucket is largely
**already closed**: `vec-of`/`set-of`/`hamt-of`/`map-count`/`grid-new` live in
`vec.tur`/`set.tur`/`map.tur`/`grid.tur`, all now in the prelude.

### Prereq 3b -- benchmark-stub overlap audit (AUDITED 2026-06-12)

`cmd_eval` injects no-op stubs (`vec-get`, `vec-set!`, `hamt-*`, `ok?`, `some?`,
...) for stdlib-free benchmark scripts. Preloading the real modules then trips
`defn: '<x>' is already defined by an auto-loaded stdlib module`. Audited the
current stub block (`src/main.c`, the `turi_eval(env, "(defn nil-value ...)")`
injection) against the 23 preloaded modules:

- **6 overlaps already removed** (the TI8.b/W1/W1b work): `vec-get`, `vec-set!`,
  `vec-free` (now `vec.tur`), and `ok?`, `err?`, `some?` (now
  `result.tur`/`option.tur`). The dropped-stub comments in `cmd_eval` record each.
- **No remaining overlaps.** Every stub still in the block is either
  native-backed (`nil-value`, `cons`, `head`, `tail`, `hamt-new/-set/-get/...`
  are overridden by `turi_env_register_native`, not a module defn -- so no
  "already defined" collision) or genuinely benchmark-only (`vec-new-filled`,
  `none?`, `run-ring`, `run-nbody`, `run-raytracer`, `random-access-bench`,
  `io-*`, the numeric helpers).

So the prelude-unification deletion list is **empty at the current prelude
size** -- the overlap was already burned down. The audit's standing value is
forward-looking: if a future module joins the prelude that defines a
benchmark-stub name (e.g. a hypothetical `io.tur` colliding with the `io-*`
stubs), `check_turi_native_parity.py`'s gap shrink plus the
`already defined` error will surface it; drop the matching stub at that point.

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

1. **1a (landed)** + **1b/1c (landed 2026-06-12)** -- the trace is in; the
   corrected count and matcher map are folded into the inline-C report. The
   actual `ic_exec_constructor` tightening (the W4 fix) remains the next code
   step.
2. **3a (landed 2026-06-12)** -- the native-registry diff shipped as
   `tools/check_turi_native_parity.py` + `docs/turi-preload-carve-out.txt`,
   gated from `tests/run.sh`. Quantifies the recoverable harness-flip bucket (4
   carved gap modules) and ratchets against drift.
3. **2b (already resolved -- #341)** -- non-int map values: closed by the
   `EX_ASCRIBE` bit-reinterpret fix (direction 2) ahead of this doc.
   `tce3-map-cstr-val` is on the `--interpret` allowlist; harness 982/0.
4. **2a (landed 2026-06-12)** -- HAMT `ctx` ABI: the `tur_hamt_*_eq_ctx` family
   plus `tests/test_hamt_eq_ctx.c` (ctest `tur_hamt_eq_ctx`). Mechanical
   runtime-C, no codegen/fixture churn; unblocks Tier B.
5. **3b (audited 2026-06-12)** + **3c/3d** -- prelude unification groundwork.
   3b's audit found the stub/module overlap already burned down (no deletions
   pending). **3c** (opt-in `TUR_TURI_FULL_PRELUDE`) and **3d** (carve markers)
   remain open; 3d is deferred until the denylist flip harness that would
   consume `requires.tur-only` markers exists (adding them now is dead weight).

### Status summary (2026-06-12)

| Prereq | State | Artifact |
| --- | --- | --- |
| 1a | landed (prior) | `TUR_IC_TRACE` / `ic_claim` |
| 1b/1c | **landed** | recount + matcher map folded into the inline-C report |
| 2a | **landed** | `tur_hamt_*_eq_ctx` + `tests/test_hamt_eq_ctx.c` |
| Tier B | **landed** | `map_turi_eq_tramp` trampoline + `tib-map-turi-comparator` fixture |
| 2b | **resolved (#341)** | `EX_ASCRIBE` bit-reinterpret; `tce3-map-cstr-val` on allowlist |
| 2c | **guarantee met** | clean rc=1 already holds; map.tur preloaded; error message now actionable |
| 3a | **landed** | `tools/check_turi_native_parity.py` + carve-out + run.sh gate |
| 3b | **audited** | overlap already burned down; no deletions pending |
| 3c | open | `TUR_TURI_FULL_PRELUDE` flag |
| 3d | deferred | needs the flip harness to consume the markers |
