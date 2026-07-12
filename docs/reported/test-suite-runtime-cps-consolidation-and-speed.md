# Test-suite runtime: CPS-oracle consolidation, suite splitting, and speed levers

**Severity: LOW (test-infra / developer-experience, no correctness impact). A
grab-bag of concrete, mostly-independent improvements to suite run time. Filed
in response to "consolidate CPS tests / split the suite / speed it up" -- each
section stands alone and can be picked up piecemeal.**

> **Progress (2026-07-12):** **Section 1 landed.** Deleted the 48 duplicate
> `cps-oracle-*-cps` twins and cleaned the now-stale oracle header on the 48
> surviving base fixtures (dropped the dangling twin references and the moot
> `;; Backend: default backend (emit_cps.c...)` annotations). Full suite still
> green: **2107 passed, 0 failed**. **Sections 2 (suite splitting) and 3 (speed
> levers -- ccache, stamp-cache priming, prebuilt runtime archive, JOBS cap)
> remain open.** One small residual under Section 1: a handful of surviving base
> fixtures still mention `--enable=cps-backend`/`emit_cps.c` in *body* prose
> (explanatory, not dangling) -- left as-is to avoid rewriting varied comments;
> the plan docs in `docs/upcoming/v2/cps-backend-unification-*` also still name
> the removed twins (historical).

## Context / measurements

`bash tests/run.sh` on this 4-core Linux container (Debug build,
`-fsanitize=address,undefined`) took **~9m10s wall / ~30m user** for **2154**
checks. The corpus is **1784** fixture dirs (**372** negative under `errors/`,
**139** carrying a codegen snapshot `expected.c`). The suite is already parallel
(`xargs -P JOBS`, `JOBS` = physical cores capped at **8**, `tests/run.sh:135-155`)
and has sharding (`TUR_TEST_SHARD="i/n"`, `:184-199`), a content-hash **stamp
cache** (`tests/.stamp-cache/`, gitignored, `:245-254`), and opt-in `ccache`
(`:77-86`). Prior perf work is archived -- see
`docs/archive/history/test-perf-plan.md` (warm repeat run measured **~3.4s** with
ccache + stamp cache at `JOBS=16`),
`docs/archive/history/test-process-reduction-plan.md` (worker pool,
`TUR_WORKER_POOL=1`), and
`docs/archive/history/test-performance-optimization-plan.md`. The slowness here
is a **cold-cache fresh-container** effect, not a regression -- see section 3.

---

## 1. Consolidate the `cps-oracle-*` twin fixtures (CPS unification is complete)

**Finding (verified):** there are **48 `cps-oracle-*` / `cps-oracle-*-cps` twin
pairs** (96 fixtures). They were the "direct-vs-CPS value-equality net" from the
unification port (`docs/upcoming/v2/cps-backend-unification-u0-inventory.md`):
the same program was meant to run under the old whole-program `emit_cps.c`
backend **and** under `--enable=cps-backend`, asserting equal output.

That premise no longer holds:
- `emit_cps.c` (the direct/whole-program CPS lowering) was **removed**.
- `--enable=cps-backend` **graduated to always-on** (#657) and the flag was
  **removed** (#658).

I diffed a twin pair (`cps-oracle-async-basic` vs `...-cps`): **neither has a
`flags` file**, `expected.stdout` is **identical**, and the input files differ by
**exactly one comment line**:

```
< ;; Backend: default backend (emit_cps.c whole-program transform)
> ;; Backend: --enable=cps-backend (CT-IR; falls back to emit_cps.c today)
```

So both twins now compile through the **same single backend** and assert the
same output -- the `-cps` twin is a **pure behavioral duplicate**. Verified
across **all 48 pairs** by scan: every pair has identical `expected.stdout`, the
input files differ **only in `;;` comment lines**, and no `-cps` twin carries a
distinguishing `flags`/marker file.

**Recommendation:**
- Drop the 48 `cps-oracle-*-cps` twins (or merge each pair to one fixture),
  after confirming per-pair that the base twin's coverage is retained. This
  removes ~48 build+run fixtures (~3% of the corpus, each a full `cc` compile).
- Fix the now-stale `;; Backend:` header comment on the **surviving** base
  fixtures (it references `emit_cps.c` / `--enable=cps-backend`, both gone).
- **Verify before deleting:** run the full suite with the twins removed to
  confirm no unique coverage was lost, and grep the U-plans
  (`docs/upcoming/v2/cps-backend-unification-*`) for any harness that still
  enumerates `cps-oracle-*-cps` by name.

This is the "consolidate tests after CPS backend unification" item and is the
single most concrete win here.

---

## 2. Split the monolithic suite into named sub-suites

Today `run.sh` runs one flat list through `xargs -P JOBS`. The building blocks
for splitting already exist -- `TUR_TEST_FILTER` (regex, `:180-181`) and
`TUR_TEST_SHARD="i/n"` (`:184-199`) -- but there is no first-class notion of a
named suite, so:
- a developer touching effects/CPS still pays for the whole 1784-fixture run to
  get feedback;
- CI cannot fan the suite across machines without hand-rolled shard indices.

**Recommendation (low-risk, uses existing machinery):**
- Define a handful of named sub-suites by fixture-name convention or a
  `suite.<name>` marker file, e.g. `codegen-snapshot` (the 139 `expected.c`
  fixtures), `effects-cps`, `errors` (the 372 negatives), `stdlib+interp`,
  `everything-else`. Back each with a `TUR_TEST_FILTER` preset.
- Document `TUR_TEST_SHARD` for CI matrix use (N runners each run `i/N`), so the
  wall-clock drops ~linearly with runner count -- the sharding code is already
  there; it just is not wired into a CI matrix or documented for it.
- Note: splitting does **not** speed up a single-machine full run (same total
  work); its wins are faster developer feedback on a subset and CI horizontal
  scaling. Sending more cores at *one* machine is section 3.

---

## 3. Other run-time levers (grounded in this container's profile)

Per-fixture cost is dominated by the **build phase**: `tur build` emits C that
`#pragma`-autolinks ~19 runtime source files (`src/runtime/hamt.c`, ...) and
`cc` recompiles them **every fixture** (`run.sh:22`, autolink confirmed in
emitted C). Measured ~1.0s for one `tur build`. With ~1442 build+run fixtures /
4 cores that is the bulk of the 9 minutes.

- **Install `ccache` in the container (biggest lever, zero code change).**
  `run.sh:85` already prepends `ccache` when present and sets
  `CCACHE_NOHASHDIR=1` for cross-run hits -- but `ccache` is **not installed
  here** (`which ccache` -> not found). The archived `test-perf-plan.md` measured
  warm runs at **~3.4s** with ccache available. Add it to the environment setup /
  container image.
- **Persist / prime the stamp cache in CI.** `tests/.stamp-cache/` skips an
  unchanged fixture on a repeat run (`:245-254`) but is gitignored and therefore
  **cold on every fresh container** -- so each session pays a full cold run.
  Caching this dir between CI runs (keyed on `tur` binary hash + fixture content)
  makes incremental runs near-instant.
- **Prebuilt runtime archive.** Even with ccache, recompiling the autolinked
  runtime per fixture is wasteful. Building the runtime once into a static lib
  and linking fixtures against it would cut the structural cost and help the
  cold, ccache-less case (this container). Larger change; worth scoping.
- **Raise the `JOBS` cap on many-core CI.** Hard-capped at 8 (`:155`); a 16/32-core
  runner is left idle. Make the cap configurable (it already reads
  `TUR_TEST_JOBS`, but the `>8 -> 8` clamp overrides upward intent).
- **`TUR_WORKER_POOL=1`** (Tier 3 of `test-process-reduction-plan.md`) exists as
  opt-in; evaluate making it default if it is stable.

## Scope / non-goals

Sections are independent. Section 1 is a clean, verifiable delete-and-verify.
Section 2 is ergonomics + CI scaling on existing primitives. Section 3's top
item (install ccache) is an environment change, not a code change, and is the
highest speed-to-effort ratio. None of this blocks correctness work; it is
developer-experience and CI throughput. Prior archived plans already landed
parallelism, the stamp cache, ccache *support*, and the worker pool -- this
report is the residual (unused-in-container caches + post-unification dead
fixtures), not a re-proposal of that work.
