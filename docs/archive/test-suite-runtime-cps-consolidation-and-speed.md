# Test-suite runtime: CPS-oracle consolidation, suite splitting, and speed levers

**Severity: LOW (test-infra / developer-experience, no correctness impact). A
grab-bag of concrete, mostly-independent improvements to suite run time. Filed
in response to "consolidate CPS tests / split the suite / speed it up" -- each
section stands alone and can be picked up piecemeal.**

> **ARCHIVED (2026-07-12) -- disposition:**
> - **Section 1 (CPS-oracle consolidation): landed** (48 twins deleted, suite green).
> - **Section 3 (speed levers): the JOBS-clamp fix landed; the ccache/prebuilt-runtime
>   work is now tracked in
>   [`docs/upcoming/v2/tur-link-and-build-split-plan.md`](../upcoming/v2/tur-link-and-build-split-plan.md)**
>   (which folds in the corrected finding that installing ccache is a no-op until
>   `tur build` splits compile from link).
> - **Section 2 (suite splitting): LANDED (2026-07-12).** Two pieces:
>   (a) **named sub-suites** in `tests/run.sh` -- a `TUR_TEST_SUITE` knob with
>   robust file-based groups (`happy`/`errors`/`snapshots`/`all`), validated
>   (`snapshots`=139, `errors`=372, unknown->exit 2), composes with
>   `TUR_TEST_FILTER` + `TUR_TEST_SHARD`. (b) **CI matrix sharding** in
>   `.github/workflows/ci.yml` -- the `test` job now fans the big `tur_tests`
>   suite across a 4-way `TUR_TEST_SHARD` matrix per OS (aux suites run once on
>   shard `1/4`). Sharding partition verified locally: 4 shards = 527/527/527/526
>   = 2107 total, and one real shard ran in **2m21s** vs ~9 min full. This is the
>   cross-machine parallelism win; it needed none of the build-split work.

> **Progress (2026-07-12):**
> - **Section 1 landed.** Deleted the 48 duplicate `cps-oracle-*-cps` twins and
>   cleaned the now-stale oracle header on the 48 surviving base fixtures. Full
>   suite green: **2107 passed, 0 failed**. Residual: a few survivors still
>   mention `--enable=cps-backend`/`emit_cps.c` in *body* prose (explanatory, not
>   dangling), and the `docs/upcoming/v2/cps-backend-unification-*` plans still
>   name the removed twins (historical) -- both left as-is.
> - **Section 3 partially landed + corrected.** Fixed the `TUR_TEST_JOBS` clamp
>   (explicit override now honored uncapped). **Empirically disproved the
>   "install ccache" recommendation**: `tur build` compiles+links in one `cc`
>   call (uncacheable -- measured 0% cacheable), so ccache is a no-op until the
>   build splits `-c` compile from link. Section 3 rewritten accordingly.
> - **Section 2 (suite splitting) remains open.**

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
`#pragma`-autolinks runtime source files (`src/runtime/hamt.c`, ...) and `cc`
compiles+links them **every fixture**. Measured ~1.0s for one `tur build`. With
~1442 build+run fixtures / 4 cores that is the bulk of the 9 minutes.

> **Investigated 2026-07-12 -- the "install ccache" idea below is a NO-OP as-is;
> corrected in place.** `run.sh:85` prepends `ccache` when present, and I
> installed ccache and re-ran a 48-fixture subset (`TUR_FORCE=1`, stamp cache
> bypassed): **`ccache -s` reported 48/48 calls "Uncacheable", 0 hits/misses.**
> Reason (confirmed by logging `cc` argv): `tur build` runs a **single `cc`
> invocation with multiple `.c` sources and NO `-c`** -- i.e. it compiles AND
> links in one call, e.g.
> `cc /tmp/tur-build/<name>.c src/runtime/hamt.c -o <exe>`. ccache cannot cache a
> combined compile+link. So **installing ccache alone changes nothing** on the
> current build path; the archived `test-perf-plan.md` ~3.4s figure must have
> come from the stamp cache and/or a `-c`-split build mode, not this path. Do
> **not** add a bare `ccache` step to CI expecting a speedup.

Corrected levers, in rough value order:

- **Split `tur build`'s `cc` call into `-c` compile + link (unlocks ccache).**
  Compile each autolinked runtime `.c` (and the generated program `.c`) with
  `-c` to a `.o`, then link the `.o`s. Only then can ccache cache the runtime
  object compiles across fixtures. This is a build-driver change in the compiler
  (where `tur build` assembles the `cc` command), not a run.sh tweak. Biggest
  structural win but the largest change -- scope before committing.
- **Prebuilt runtime archive.** A lighter variant of the above that skips ccache
  entirely: compile the runtime `.c` set **once** per suite run into a static
  `.a`/`.o` set and link every fixture against it, instead of recompiling the
  autolinked sources per fixture. Helps even a cold, ccache-less run (this
  container).
- **Persist / prime the stamp cache in CI.** `tests/.stamp-cache/` skips an
  unchanged fixture on a repeat run (`:245-254`) but is gitignored and cold on
  every fresh container. Caching it between CI runs helps *incremental* runs --
  but note it keys on fixture content **and** the `tur` binary, so a PR that
  changes `src/` (the common case) invalidates every stamp and gets no benefit.
  Value is real but narrower than it first appears.
- **[DONE 2026-07-12] Honor an explicit `TUR_TEST_JOBS` above the auto-cap.**
  Was: `JOBS` hard-clamped to 8 even when a user set `TUR_TEST_JOBS=16`. Fixed in
  `tests/run.sh` -- the `>8 -> 8` clamp now applies only to the *auto-detected*
  core count; an explicit `TUR_TEST_JOBS` is honored uncapped (unit-tested:
  explicit 16/32 pass through, auto still caps at 8, bad/zero sanitize to 4/1).
  A many-core CI runner can now use its cores via `TUR_TEST_JOBS`.
- **`TUR_WORKER_POOL=1`** (Tier 3 of `test-process-reduction-plan.md`) exists as
  opt-in; evaluate making it default if it is stable.

## Scope / non-goals

Sections are independent. Section 1 (landed) is a clean delete-and-verify.
Section 2 is ergonomics + CI scaling on existing primitives. Section 3's
remaining items are real code changes: the ccache/prebuilt-runtime work is the
only thing that meaningfully cuts a *cold single-machine* run, and it requires
splitting `tur build`'s compile from its link -- **not** merely installing
ccache (verified no-op). None of this blocks correctness work. Prior archived
plans landed parallelism, the stamp cache, ccache *support* (dormant on this
path), and the worker pool -- this report is the residual.
