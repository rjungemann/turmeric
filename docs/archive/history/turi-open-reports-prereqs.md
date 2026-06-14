# Prerequisite tasks to de-risk the three open turi-* reports

> **COMPLETE 2026-06-14 -- archived.** Every prereq in this doc landed or was
> otherwise resolved (see the Status summary table at the bottom: 1a-3d all
> landed/resolved). The three reports it de-risked are themselves closed -- the
> harness flip (W5) shipped, the inline-C matcher tightening (W4) landed, and the
> map/set/hamt comparator gap (Tier B) closed. Retained as the historical record
> of the groundwork; no open items remain.

A planning pass over the three still-open turi interpreter reports, decomposing
each big lift into smaller, independently-landable groundwork that eases (and
de-risks) the eventual fix. Grounded with file:line pointers and current
measurements taken 2026-06-12 against `./build/tur`.

Open reports surveyed:

- [docs/reported/turi-inline-c-silent-miscompiles.md](turi-inline-c-silent-miscompiles.md)
- [docs/archive/history/turi-map-set-hamt-interpreter-gap.md](turi-map-set-hamt-interpreter-gap.md)
- [docs/archive/history/turi-harness-flip-reconciliation.md](turi-harness-flip-reconciliation.md)

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
[the report](turi-inline-c-silent-miscompiles.md) (the stale
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

### Prereq 1c -- prioritize by matcher, not by fixture (W4 LANDED 2026-06-12)

The map showed **two matchers cover 16 of 20**: `ic_exec_constructor` (12) and
`ic_exec_snprintf_fmt` (4). **The full W4 tightening landed** -- all 20 now flip
to a clean `rc=1` error, via refuse-rather-than-guess guards on each matcher
(constructor / snprintf / accessor / simple-return). Each matcher was validated
against its correctly-claimed regression set; full interpreter harness 983/0,
compiled suite 1599/0, zero regressions. Details + per-matcher guard list:
[../../archive/history/turi-inline-c-silent-miscompiles.md](turi-inline-c-silent-miscompiles.md)
(the report's "RESOLVED (W4)" banner).

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
[../../archive/history/turi-map-nonint-value-carrier-ascription.md](turi-map-nonint-value-carrier-ascription.md).

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

### Prereq 3c -- opt-in full-prelude flag (LANDED 2026-06-12)

**Landed.** Since the doc was written, the typed-stdlib core became the *default*
interpreter prelude (the `prelude[]` block in `cmd_eval`, loaded unconditionally),
so the original "recover the typed-stdlib bucket" rollout is already done. The
residual lever -- the carved bucket (`contract`/`mutmap`/`json`/`schema`, exactly
the 3a preload gap) -- is now behind `TUR_TURI_FULL_PRELUDE=1`
(`turi_full_prelude_enabled()` / the full-prelude block in `cmd_eval`,
`src/main.c`). When set, the interpreter additionally `(load ...)`s those four
modules (each in its own `turi_eval`, so one failing module does not block the
rest), loaded BEFORE the native overrides so those still win. This makes the
interpreter prelude match the compiled auto-load set, so the carved bucket can be
iterated/measured fixture-by-fixture under `--interpret` without committing the
extra cost -- or `contract`'s `:pre/:post` behavior change -- to every run.

Off by default (strict `=1`, matching `TUR_TSAN`); the default path is byte-for-
byte unchanged. Gate `tests/run-turi-full-prelude.sh` (ctest `turi_full_prelude`)
asserts the toggle: flag-off leaves `mutmap-new` unbound (`rc=1`), flag-on loads
`mutmap.tur` so it resolves and runs (`rc=0`), and a non-`1` value does not
enable it. Uses `(load ...)` (not `turi_eval_file`) so each carved module gets a
distinct `file_id` (defmodule-per-file boundary).

### Prereq 3d -- carve markers for the genuine-divergence buckets (LANDED 2026-06-12)

The move/linearity-checker divergence (~20) and `if condition must be bool, got
int` (13) were interpreter/type-check divergences, not quick fixes. The plan was
to add `requires.tur-only` markers to those fixtures so the flip can land green
with them explicitly excluded, separating the "carve" decision from the "fix"
decision and keeping the denylist honest.

**Landed**, but the buckets shrank dramatically since the report was measured --
the prelude/interpreter work (3a/3c and the typed-stdlib preload) resolved almost
all of them. Re-measured 2026-06-12 by sweeping every positive (non-`errors/`)
fixture under `--interpret` (`./build/tur`, marker-bearing dirs excluded):

- **move/linearity divergence: 0 remain.** No positive fixture now fails with
  `TUR-E0100`/`E0101`, "linear value ... dropped/used after being consumed", or
  "was moved and cannot be used again" under `--interpret`. The interpreter no
  longer diverges from the compiled substructural checker on these. Nothing to
  carve; the `errors/*` negative fixtures (e.g. `errors/linear-dropped`) still
  legitimately assert these diagnostics and are untouched.
- **`if condition must be bool, got int`: 2 remain**, both carved with
  `requires.tur-only`:
  - `contract-release` -- the tree-walker's type checker does not see
    `contract-enabled?` as returning bool, so `(if (contract-enabled?) ...)` is
    rejected. Compiled prints `contracts-enabled`.
  - `result-question-op-sweet` -- the `?` operator desugars to an `if` on the
    Result ok-ness carrier, type-checked as int under `--interpret` (also
    inline-C-bound via its `u-ok`/`u-ok-val` helpers). Compiled prints `42/22`.

Both pass on the compiled suite (`run.sh` ignores `requires.tur-only`, so they
still run compiled and green); the marker only PASS-skips them under
`run-turi.sh`'s denylist path (`tests/run-turi.sh:1042`). Each marker carries a
one-line divergence reason. No other genuine-divergence bucket surfaced in the
sweep, so the carve set is exactly these two.

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
5. **3b (audited 2026-06-12)** + **3c/3d (landed 2026-06-12)** -- prelude
   unification groundwork. 3b's audit found the stub/module overlap already
   burned down (no deletions pending). **3c** (opt-in `TUR_TURI_FULL_PRELUDE`)
   landed; **3d** landed too -- the move/linearity bucket re-measured to 0 (the
   interpreter no longer diverges there) and only 2 `if`-bool fixtures remain,
   now carved `requires.tur-only` (`contract-release`,
   `result-question-op-sweet`). `run-turi.sh` already consumes the marker
   (`:1042`), so the carve is live, not dead weight.

### Status summary (2026-06-12)

| Prereq | State | Artifact |
| --- | --- | --- |
| 1a | landed (prior) | `TUR_IC_TRACE` / `ic_claim` |
| 1b/1c | **landed** | recount + matcher map folded into the report |
| W4 tightening | **landed** | all 20 inline-C silent miscompiles flip to clean errors |
| 2a | **landed** | `tur_hamt_*_eq_ctx` + `tests/test_hamt_eq_ctx.c` |
| Tier B | **landed** | `map_turi_eq_tramp` trampoline + `tib-map-turi-comparator` fixture |
| 2b | **resolved (#341)** | `EX_ASCRIBE` bit-reinterpret; `tce3-map-cstr-val` on allowlist |
| 2c | **guarantee met** | clean rc=1 already holds; map.tur preloaded; error message now actionable |
| 3a | **landed** | `tools/check_turi_native_parity.py` + carve-out + run.sh gate |
| 3b | **audited** | overlap already burned down; no deletions pending |
| 3c | **landed** | `TUR_TURI_FULL_PRELUDE=1` + `tests/run-turi-full-prelude.sh` |
| 3d | **landed** | move/linearity bucket re-measured to 0; 2 `if`-bool fixtures carved `requires.tur-only` |
