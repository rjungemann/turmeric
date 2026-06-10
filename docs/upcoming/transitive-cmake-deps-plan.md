# Transitive `:cmake-deps` Resolution Plan

> **Status:** Phase 1+2 landed (single-level walk); deeper recursion + dedicated fixtures still pending
> **Last Updated:** 2026-06-09
> **Type:** Build system / `tur` resolver
> **Affects:** `src/main.c` (`tur run` / `tur build <dir>` / `tur test`),
> `src/compiler/pkg.{c,h}`, fixture coverage in this repo, and unblocks
> the cascade fixture in `../turmeric-spices/spices/tourist/tests/fixtures/cascade/`.
> **Related:**
> [docs/reported/cascade-mbedtls-header-not-found.md](../reported/cascade-mbedtls-header-not-found.md)

---

## Landed (2026-06-09)

A first-cut implementation shipped as part of the `tur run` project-mode
gate (`src/main.c` around the former `if (m.n_cmake_deps > 0)` site):

- New public API in `src/compiler/pkg.{c,h}`:
  `pkg_collect_transitive_cmake_deps(root_dir, root_manifest, &out_deps, &out_n)`
  and its counterpart free `pkg_cmake_deps_free`.
- The resolver walks the enclosing manifest's `:spices` block **one
  level deep**, resolves each spice to its on-disk directory (workspace
  sibling first, then `:path`, then `<root>/spices/<name>[-<ref>]`,
  then optional `:subdir`), reads each sibling's `build.tur`, and
  unions its `:cmake-deps` block into a freshly allocated
  `PkgCmakeDep[]` after deep-copy.
- Conflict policy keys on `:name`: identical `(url, ref, path)` is a
  silent dedup; mismatched is a hard error with both origin dirs
  printed to stderr (does not reach the cmake generator).
- `:path`-form deps from sibling manifests are absolutized against
  the sibling's directory so the existing
  `pkg_gen_cmake_deps`/`pkg_cmake_build` join (`%s/%s`) still resolves
  correctly when the root project is a different directory.
- The gate at the original site is now an unconditional
  `pkg_collect_transitive_cmake_deps` call followed by a synthetic
  manifest pass to the existing cmake generator + builder. No change
  to the on-disk CMake layout (still `<root>/cmake/CMakeLists.txt`
  and `<root>/cmake/spice-deps-manifest.json`).

Out of scope for this first cut (tracked as follow-ups below):

- **Deep transitive walk.** Only the enclosing manifest's `:spices`
  entries are inspected. Sibling-of-sibling cmake-deps aren't picked
  up. Add a visited set keyed on absolute path + recursion before
  declaring the plan complete.
- **`cmd_pkg_fetch` gate** (`src/compiler/pkg.c:3537`) and any other
  `n_cmake_deps > 0` site outside `cmd_run` still see the un-unioned
  manifest. Walk them in a follow-up.
- **`cmd_build`'s flag-read path** (`src/main.c:1656` ff.) consumes
  the generated `spice-deps-manifest.json` directly, so once the run
  generates the merged manifest it Just Works -- but a fresh
  `tur build <file>` invocation without a prior `tur run` won't
  trigger cmake generation.
- **Regression fixtures** (`transitive-cmake-deps-basic`,
  `transitive-cmake-deps-conflict`, `transitive-cmake-deps-cycle`)
  are NOT added; the cascade fixture in `../turmeric-spices` does
  not stress the path because http's mbedtls includes are guarded by
  `__has_include`, so the compile proceeds (without TLS) even when
  cmake-deps are unresolved.

## Problem

`tur run` (and its siblings `tur build <dir>` / `tur test`) only honor the
`:cmake-deps` block of the **enclosing** spice's `build.tur`. When the
enclosing spice imports a workspace sibling whose own manifest declares
`:cmake-deps`, those deps are silently ignored -- the sibling's `src/` is
added to the include path (so `(import sib/mod ...)` resolves at elab
time), but its native C dependencies are never fetched, configured, or
linked.

The result is a hard `cc` error whenever the sibling emits inline-C that
needs those native headers, on what looks like a clean workspace build.

### Concrete repro -- cascade

`turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur` does
`(import http/client :refer [http-get])`. The workspace's `:members` list
makes `spices/http/` visible as a sibling, so the import resolves and
elab/codegen succeed. The emitted C contains
`http__client___un_undo_hyrequest`, which is an inline-C body that opens
with:

```c
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
```

`spices/http/build.tur:11-15` correctly declares mbedtls in
`:cmake-deps` (same shape as `spices/tls/build.tur:6-13`). But the
enclosing manifest at `spices/tourist/build.tur` has zero `:cmake-deps`,
and `src/main.c`'s `tur run` project path reads only `&m` (the enclosing
manifest):

```c
/* src/main.c:2885-2907 */
if (m.n_cmake_deps > 0) {
    ...
    if (!pkg_gen_cmake_deps(root, &m) ||
        !pkg_cmake_build(root, &m, &lock, NULL)) {
        ...
    }
}
```

So `pkg_gen_cmake_deps` is skipped entirely, `cc` never sees the mbedtls
include path, and the build dies with
`fatal error: 'mbedtls/net_sockets.h' file not found`.

### Confirming repros

- `build/tur build ../turmeric-spices/spices/http` (single-spice build,
  from inside the spice with `:cmake-deps`): **succeeds** when the cmake
  cache is warm; if `cmake/build/_deps/` is wiped, `tur run` from any
  fixture inside `spices/http/` does the right thing because the
  *enclosing* manifest is `http/build.tur`.
- `build/tur run .../spices/tls/tests/fixtures/ctx-lifecycle/main.tur`:
  **succeeds**, for the same reason -- enclosing manifest is
  `spices/tls/build.tur`, which carries mbedtls in `:cmake-deps`.
- `build/tur run .../spices/tourist/tests/fixtures/cascade/cascade.tur`:
  **fails** with the mbedtls error above. Enclosing manifest is
  `spices/tourist/build.tur` (no cmake-deps); the http sibling's cmake
  deps are dropped on the floor.

The bug is structural, not a stale manifest. Two distinct workspace
spices -- `http` and (in the docs) `tls` -- already keep their native
build deps in the right place; the resolver is the part that lies.

---

## Goals

1. When the enclosing manifest's `:spices` block references a workspace
   sibling that itself declares `:cmake-deps`, those native deps are
   fetched, configured, built, and exposed (include + link flags) to the
   enclosing TU's `cc` invocation.
2. Behavior is independent of the import surface used: `:path`-resolved
   spices, workspace siblings (resolved via the parent `:members` list),
   and `:url`-fetched spices all participate equally.
3. The transitive walk terminates and is deterministic: a sibling that
   itself depends on a third sibling carrying `:cmake-deps` works
   correctly, with cycles handled the same way `pkg_fetch_all` already
   handles `:spices` BFS (visited set + skip).
4. Conflict policy: if two transitively-reached spices declare a
   `:cmake-dep` with the **same `:name`**, identical `:url` + `:ref` are
   silently unioned (only one fetch/build). Mismatched `:url`/`:ref` is a
   hard build error with both pointers in the diagnostic.
5. The cascade fixture compiles end-to-end on a host that does *not*
   have system mbedtls, with no additional manifest edits in `tourist`
   or `httpd`.

---

## Non-goals

- **No manifest format change.** `:cmake-deps` stays where it is, in
  each spice's own `build.tur`. No "workspace cmake-deps" merge layer.
- **No eager workspace-wide build.** The transitive walk follows the
  enclosing manifest's `:spices` block (and what each of those declares
  transitively), not the workspace `:members` list as a whole. A spice
  the enclosing build doesn't import contributes nothing.
- **No incremental rebuild redesign.** The existing "build once, cache
  on disk under `<spice>/cmake/build/`" semantics carry over per
  participating spice. Cross-spice incremental orchestration is out of
  scope.
- **No change to feature-gating or TLS-vs-plain HTTP split in the
  spices.** That is a separate `../turmeric-spices` discussion; this
  plan just makes the deps the spice *does* declare actually land.
- **No skip-marker harness.** The previously proposed `requires.mbedtls`
  marker is *not* the fix; this plan makes it unnecessary.

---

## Current architecture

The relevant `tur run` path lives in `src/main.c` around lines 2820-2945:

1. **Manifest read.** `pkg_manifest_read(root, &m)` reads
   `<root>/build.tur` into `PkgManifest m`.
2. **`:spices` BFS fetch.** `pkg_fetch_all(root, &m, &lock, false)`
   (`src/compiler/pkg.c:1380`) walks the `:spices` block transitively
   for `:url`-backed deps. Its body explicitly comments that "cmake deps
   are handled in `pkg_gen_cmake_deps`, not fetched here"
   (`src/compiler/pkg.c:1438`) -- so the BFS already exists, it just
   doesn't know about cmake.
3. **Include-path resolution.** `resolve_include_dirs_from_manifest`
   (`src/main.c:2429-2521`) walks **only the enclosing manifest's**
   `:spices` block one level deep, picking each dep's `src/` for the
   include path. It does *not* recurse into sibling manifests.
4. **CMake step.** `if (m.n_cmake_deps > 0)` (`src/main.c:2886`) calls
   `pkg_gen_cmake_deps(root, &m)` and `pkg_cmake_build(root, &m, ...)`
   against the enclosing manifest only. Output lands under
   `<root>/cmake/build/_deps/`.

`pkg_gen_cmake_deps` (`src/compiler/pkg.c:1780`) and `pkg_cmake_build`
(`src/compiler/pkg.c:1991`) both take a single `(project_dir, manifest)`
pair. They emit a single `<project_dir>/cmake/CMakeLists.txt` and run
cmake from `<project_dir>/cmake/build/`. The resulting include/link
flags are read back via `pkg_cmake_manifest_read` (`src/main.c:2968`
comment) and folded into the `cc` invocation.

So the four places that need to change are:

| Site | Today | Needs to |
|---|---|---|
| `resolve_include_dirs_from_manifest` | one-level walk over `:spices` | also collect, for each workspace-sibling dep, the dep's own manifest and surface its `:spices` (recursively) for transitive include-path coverage |
| The `if (m.n_cmake_deps > 0)` gate (`src/main.c:2886`) | only fires for enclosing manifest | fire whenever **any** spice in the import closure has cmake-deps |
| `pkg_gen_cmake_deps` / `pkg_cmake_build` | take one manifest | accept a union of cmake-deps from N participating manifests, with conflict detection |
| `pkg_cmake_manifest_read` (flag write-back) | reads one manifest's flags | read the union manifest's flags |

---

## Proposed approach

### Phase 1 -- introduce a transitive closure helper

Add to `src/compiler/pkg.{c,h}`:

```c
/* Walk the :spices block of `root_manifest` rooted at `project_dir`,
 * including workspace siblings via the parent :members list. Returns a
 * newly-allocated array of (path, PkgManifest*) pairs covering every
 * spice reachable from the root (root inclusive), deduped by absolute
 * path. Caller owns the result and must call pkg_closure_free.
 *
 * Cycles are skipped (visited set keyed on resolved absolute path).
 * Spices that fail to resolve (e.g. :url fetch failed earlier) are
 * skipped, matching pkg_fetch_all's "continuing with healthy deps"
 * policy. */
typedef struct {
    char        *project_dir;   /* owned */
    PkgManifest  manifest;      /* owned, free with pkg_manifest_free */
} PkgClosureEntry;

bool pkg_resolve_closure(const char       *root_project_dir,
                         const PkgManifest *root_manifest,
                         PkgClosureEntry **out_entries,
                         int              *out_n);

void pkg_closure_free(PkgClosureEntry *entries, int n);
```

Implementation reuses `pkg_workspace_member_path` + the path-resolution
logic already in `resolve_include_dirs_from_manifest` (factor it out).

### Phase 2 -- thread the closure through the four sites

1. **Include-path resolution.** Add a closure-aware variant:
   ```c
   void resolve_include_dirs_from_closure(const PkgClosureEntry *entries,
                                          int n,
                                          bool include_own_src,
                                          const char ***out_dirs,
                                          int *out_n);
   ```
   The existing one-level version becomes a thin wrapper that builds a
   closure of size 1.
2. **CMake gate.** Replace `if (m.n_cmake_deps > 0)` with
   `if (closure_any_cmake_deps(entries, n))`.
3. **`pkg_gen_cmake_deps` / `pkg_cmake_build`.** Either:
   - **Option A (preferred):** add closure-taking variants that union
     `:cmake-deps` from every entry into a single CMakeLists generated
     at `<root>/cmake/CMakeLists.txt`. One cmake configure + build for
     the whole TU. Cache lives under the enclosing project.
   - **Option B:** generate per-spice CMakeLists under each participant's
     own `cmake/` dir, run cmake N times, union the resulting flag
     manifests. Reuses the existing per-spice cmake cache (warm builds
     stay warm across fixtures), but configures cmake more than once.

   Start with **A**; revisit if cache invalidation becomes painful.

4. **Flag write-back.** `pkg_cmake_manifest_read` reads the unioned
   manifest under the enclosing project. No call-site change beyond
   passing the closure-built artifact.

### Phase 3 -- conflict detection

Inside the union step, key on `cmake-dep.name`. When two participants
declare the same name:

- Identical `:url`, `:ref`, `:options`, `:targets` (deep-equal): keep one
  copy silently.
- Any divergence: emit
  ```
  tur run: conflicting :cmake-deps for "<name>":
    <spice-A>/build.tur: url=..., ref=..., targets=[...]
    <spice-B>/build.tur: url=..., ref=..., targets=[...]
  ```
  and fail the build. We deliberately do **not** auto-merge -- a
  divergence between, say, mbedtls v3.6.2 and v3.5.0 is a real
  decision the human has to make.

### Phase 4 -- regression fixtures (this repo)

Add under `tests/fixtures/`:

1. `transitive-cmake-deps-basic/` -- minimal `:path`-linked sibling that
   declares a trivial `:cmake-deps` (a header-only library e.g. one we
   vendor under `tests/fixtures/transitive-cmake-deps-basic/vendor/`).
   The fixture's program does an inline-C `#include <foo.h>` from the
   sibling and exits 0.
2. `transitive-cmake-deps-conflict/` -- two siblings declaring the same
   `:cmake-dep` name with different `:ref`s. Expected outcome: cc never
   runs; `tur run` exits non-zero with the conflict diagnostic above.
   Mark `expected.cc-fail` (already a recognized harness convention) and
   add an `expected.stderr-grep` for the diagnostic stem.
3. `transitive-cmake-deps-cycle/` -- A → B → A loop in `:spices`. Should
   build and run without infinite recursion (visited set check).

These fixtures cover the resolver in this repo, independent of the
spices repo's cascade fixture, so they survive the spices checkout being
absent.

### Phase 5 -- validate cascade end-to-end

With Phases 1-4 in:

```sh
build/tur run \
  ../turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur
```

must reach the fixture's `run-all` summary on a host without system
mbedtls, exercising the http client → mbedtls path through the
transitively-resolved `:cmake-deps`. The companion report
[`docs/reported/cascade-mbedtls-header-not-found.md`](../reported/cascade-mbedtls-header-not-found.md)
gets a Resolution block pointing at this plan.

---

## Edge cases / open questions

1. **`:optional true` spices.** httpd and tourist declare `http` as
   `:optional true`. If the optional sibling is absent, should its
   `:cmake-deps` participate at all? Proposal: no -- "absent" means
   "do not pull in." If present but resolved, cmake-deps participate.
   This matches the existing include-path rule.

2. **External `:url` spices with `:cmake-deps`.** `pkg_fetch_all` clones
   them into `<root>/spices/<name>-<ref>/`. Their `build.tur` would then
   be readable by `pkg_resolve_closure`, and their `:cmake-deps` would
   participate the same way a workspace sibling's would. No extra
   plumbing -- the path resolution is the same code path.

3. **Cache layout.** Option A puts the unioned `cmake/build/_deps/` at
   `<enclosing-root>/cmake/build/`. This means the same mbedtls source
   tree may be re-fetched/built per enclosing project (tourist vs.
   another consumer). Acceptable for v1; revisit with a shared cmake-
   deps cache (`~/.cache/tur/cmake-deps/<name>-<ref>/`) later if hot.

4. **Conflict on `:options`.** Two consumers might want different cmake
   options for the same lib (one wants `BUILD_SHARED_LIBS=ON`, the
   other OFF). This is a real divergence and should fail per Phase 3.
   Future: an explicit `:override` field in the enclosing manifest's
   own `:cmake-deps` could force a choice.

5. **Per-spice CI workflows
   ([per-spice-ci-workflows-plan.md](v1/per-spice-ci-workflows-plan.md)).**
   That plan runs `tur test` flat-or-nested per spice. With this plan,
   `tur test` inside tourist correctly pulls mbedtls in via the http
   sibling; the per-spice CI plan does not need a workaround.

---

## Validation

- `bash tests/run.sh` passes with all three new fixtures green; existing
  fixtures unchanged (the closure logic collapses to a one-level walk
  whenever no sibling declares `:cmake-deps`, which is the case for
  every in-repo fixture today).
- `cd ../turmeric-spices/spices/tls/tests/fixtures/ctx-lifecycle && ./run.sh`
  still passes (enclosing manifest *is* tls; closure of size 1; same
  cmake build as before).
- `build/tur run ../turmeric-spices/spices/tourist/tests/fixtures/cascade/cascade.tur`
  reaches the fixture's run-all summary on a host without system
  mbedtls.
- A host *with* system mbedtls in standard paths is unaffected (cmake's
  `FetchContent` for mbedtls still wins because the cmake-deps generator
  emits `FetchContent_Declare` / `_MakeAvailable`, which is hermetic).

## Out-of-scope follow-ups

- Move the cmake-deps cache to a shared, content-addressed location.
- Add an `:override` mechanism in the enclosing manifest for forced
  conflict resolution.
- Surface "this build pulled in `:cmake-deps` from spice X, Y, Z" as
  a `tur run --verbose` line so the dependency surface is auditable.
