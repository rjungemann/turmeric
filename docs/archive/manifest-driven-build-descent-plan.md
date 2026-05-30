# Plan: Manifest-Driven `tur build <dir>` Descent

> **Status:** Phases 1-3 + follow-ups T1/T2/T3 implemented
> **Last Updated:** 2026-05-29
> **Type:** CLI / build-system change (`tur` source, this repo)
> **Related:**
> - `docs/scscm-spice-import-refactor-plan.md` (Phase 1 step 1 is blocked on this)
> - `src/main.c` -- `cmd_build_multi`, `collect_tur_files`, the `build` dispatch,
>   and the project-mode resolution already implemented in `cmd_run` / the test runner
> - `src/compiler/pkg.c` -- `pkg_manifest_read`, `parse_str_vec`,
>   `collect_exports_from_src`
> - `CLAUDE.md` ("Per-file Commands Inside a Spice", "Build System")

---

## Overview

`tur build <dir>` does not understand spice projects. When pointed at a spice
root it fails with a misleading error:

```
$ tur build ../turmeric-spices/spices/scscm
build.tur:1:13: error [TUR-E0003]: unbound symbol 'tur-scscm'
tur: failed to compile build.tur to header
```

This is **not** a malformed `build.tur` and **not** a `defpackage`
regression. It reproduces identically for *every* spice (`ansi`, `math`,
`json`, ...). The root cause is in the CLI:

- `tur build <dir>` dispatches to `cmd_build_multi` (`src/main.c`).
- `cmd_build_multi` calls `collect_tur_files`, a **non-recursive glob** of the
  directory's top-level `*.tur` files.
- That glob (a) does **not** read `build.tur`, (b) does **not** skip the
  manifest, and (c) does **not** descend into `src/`.

All 31 first-party spices keep their sources under `src/` (often nested one
level deeper, e.g. `src/scscm/lexer.tur`) and have **zero** `.tur` files at the
root. So the only file the glob finds is `build.tur` itself, which then gets
compiled as ordinary source -- and `defpackage`'s package name (`tur-scscm`)
reads as an unbound symbol.

This plan makes `tur build <dir>` manifest-aware: when the directory contains a
`build.tur`, read the manifest, resolve the project's own `src/` tree (plus
each declared `:spices` dependency's `src/`) as the include search path, build
the module set found under `src/`, and never feed the manifest to the compiler.
This brings `tur build <dir>` in line with what `CLAUDE.md` already claims is
true ("`tur build <dir>` and `tur run` (project mode) already configure
themselves from `build.tur`") and with what `cmd_run` already does.

## Motivation

- **Unblocks the scscm refactor.** `docs/scscm-spice-import-refactor-plan.md`
  Phase 1 step 1 ("make the spice build as a project") and its validation
  checklist item `tur build <spice> exits 0` are impossible today without this
  change. Editing `build.tur` cannot fix a CLI globbing bug.
- **Fixes it for the whole ecosystem.** Every spice is affected, so the fix is
  general, not scscm-specific.
- **Consistency.** `tur run <dir>` and the test runner already resolve projects
  from `build.tur`; `tur build <dir>` is the odd one out.

## Current state

### Dispatch path (the bug)

`src/main.c`, `build` subcommand:

```c
if (is_directory(input)) {
    rc = cmd_build_multi(input, out, shared, manifest_out);  // never looks at build.tur
}
```

`cmd_build_multi` -> `collect_tur_files(dir)`:

```c
// non-recursive opendir loop; takes every *.tur, including build.tur
while ((ent = readdir(d)) != NULL) {
    if (ent->d_type != DT_REG) continue;
    if (len >= 4 && strcmp(ent->d_name + len - 4, ".tur") == 0) { /* add */ }
}
```

### What already exists and can be reused

- **`pkg_manifest_read`** (`pkg.c`) parses `build.tur` into a `PkgManifest`
  (`name`, `version`, `spices[]`/`n_spices`, `exports[]`/`n_exports`,
  `reader_macros[]`, ...).
- **Project-mode include resolution** is already implemented twice:
  - `cmd_run` (`src/main.c`, project-mode branch): `find_project_root`,
    `pkg_manifest_read`, then build `spice_inc_dirs` = own `src/` + each
    `:spices` dep's resolved `src/` (with `:path` / `:ref` / `:subdir`
    handling and a monorepo-sibling fallback).
  - The test runner (`src/main.c`, the `passed`/`failed` loop) repeats the same
    own-`src/` + per-dep-`src/` resolution block.
- **`collect_exports_from_src`** (`pkg.c`): scans `src/` for `.tur` files when
  `:exports` is absent. Used today by `cmd_pkg_emit_cmake`.

### Two gaps that this plan must close

1. **`:exports` map form is not parsed.** Every spice writes
   `:exports #{ "scscm/lexer" [...syms...] ... }` -- an `F_MAP`. But
   `parse_str_vec` only handles `F_STR` / `F_VEC` / `F_LIST` and silently
   returns empty for an `F_MAP`. So `m.n_exports == 0` for *all* real spices;
   the map is effectively ignored. Any descent that wants to drive off
   `:exports` keys must first teach the parser to extract the map keys
   (the module names, which map to `src/<key>.tur`).

2. **`src/` scanning is shallow.** `collect_exports_from_src` does a single
   `opendir("src")` with no recursion, so it misses nested layouts like
   `src/scscm/*.tur`. A manifest-driven descent must recurse into `src/`.

## Plan

### Phase 1 -- Manifest-aware dispatch + recursive source collection

1. **Detect a project in the `build` dispatch.** In `src/main.c`, before
   calling `cmd_build_multi`, check for `<dir>/build.tur`. If present, route to
   a new manifest-driven path (call it `cmd_build_project`); otherwise keep the
   current bare-directory behavior (so non-project directories still work).
2. **Add a recursive `src/` collector.** Either extend
   `collect_exports_from_src` with a recursion flag or add
   `collect_src_recursive(project_dir)`. It must:
   - walk `<project>/src/` recursively, gathering `*.tur`;
   - **exclude `build.tur`** and any lockfile (`tur.lock`) defensively;
   - skip conventional non-source subtrees (`tests/`, `examples/`, anything the
     existing project tooling already ignores -- confirm against `cmd_run` /
     the test runner so the file set matches).
3. **Reuse the include-path resolution.** Factor the own-`src/` + per-`:spices`
   resolution that `cmd_run` and the test runner duplicate into a single helper
   (e.g. `resolve_project_include_dirs(root, &m, &dirs, &n)`), then call it from
   `cmd_build_project`. This avoids a third copy of that logic and keeps the
   three commands consistent.
4. **Build the collected module set** through the existing `cmd_build_multi`
   machinery (two-pass ABI specialization, `compile_to_h`, link), but with:
   - the recursively collected `src/` files as the module set,
   - the resolved include dirs threaded into `compile_to_h` / `cmd_build`,
   - reader-macros applied from the manifest (mirror
     `resolve_manifest_reader_macros`, already used by `cmd_run`).

Smallest viable version: steps 1, 2, and 4 with a recursive collector make
`tur build <spice>` collect the right files. Step 3 is a refactor that prevents
a third divergent copy of the include logic -- do it as part of this phase to
keep the three entry points in sync.

### Phase 2 -- Parse the `:exports` map (optional but recommended)

The recursive `src/` scan is layout-driven and works without touching the
parser. But the manifest's `:exports` map is the *authoritative* module list,
and parsing it has independent value (docs, `emit-cmake`, validation).

1. Teach `pkg.c` to parse the `F_MAP` form of `:exports`: extract the map
   **keys** (module names like `"scscm/lexer"`) into `m.exports`, ignoring the
   per-module symbol vectors (or storing them separately if useful later).
2. Once keys are available, `cmd_build_project` can optionally prefer the
   manifest's declared modules (`src/<key>.tur`) over a raw `src/` scan, which
   lets the build fail loudly when a declared export is missing a file.
3. Audit `cmd_pkg_emit_cmake`: it currently branches on `m.n_exports > 0` but
   that branch is dead for map-form manifests (it always falls through to
   `collect_exports_from_src`). Fixing the parser will activate it -- verify
   the generated CMake still matches expectations, or keep emit-cmake on the
   src-scan path explicitly.

This phase is *recommended* because the silently-ignored `:exports` map is a
latent correctness gap, but Phase 1 does not strictly depend on it.

**Implemented.** Decisions made during implementation:

- `parse_exports` (`src/compiler/pkg.c`) handles the `F_MAP` form (captures
  the module-name keys into `m.exports`; the per-module symbol vectors are not
  stored) and delegates the legacy vector/string form to `parse_str_vec`.
- The build set in `cmd_build_project` **remains the recursive `src/` scan**,
  not the `:exports` list -- a project's internal, non-exported modules must
  still compile. The parsed exports are used only to **validate**: each
  declared export must have a backing source file (`src/<key>.tur`, or the
  path itself for the legacy `.tur` form), and the build fails loudly with
  `build.tur declares export '<m>' but no source file exists at ...` otherwise.
- `tur emit-cmake` normalizes each `:exports` entry to a source path
  (`src/<key>.tur` for a module name; used as-is when it already ends in
  `.tur`). Previously the map form parsed to empty and `emit-cmake` fell
  through to a shallow `src/` scan that missed nested `src/<pkg>/` layouts; now
  it enumerates the declared modules correctly. The existing vector-form
  `test-emit-cmake.sh` cases are unaffected (they end in `.tur`).
- `tur add-cmake` round-trips `build.tur` through the lossy `pkg_manifest_write`
  serializer. It already dropped the map-form `:exports` entirely (it parsed to
  empty); it now rewrites it as a vector of module names. This is a pre-existing
  lossy-rewrite limitation (it also drops `:bin`, comments, formatting), out of
  scope here -- noted so it isn't mistaken for a new regression.

### Phase 3 -- Tests + docs

1. **CLI tests.** Add a fixture/project under `tests/` that exercises
   `tur build <dir>` against a small multi-module project living in `src/`
   (and a nested `src/<pkg>/` variant to lock in the recursion). Assert exit 0
   and that the manifest is not compiled as source. Mirror existing dedicated
   runners in `CMakeLists.txt`.
2. **Regression guard.** Add a case asserting `tur build <dir>` on a directory
   that contains a stray `build.tur` does not try to compile the manifest.
3. **Docs.** Update `CLAUDE.md` ("Build System" / "Per-file Commands") and the
   spice-development guide to document that `tur build <spice-root>` is now
   manifest-aware and descends into `src/`. Reference this plan.

**Implemented.**

- The `build-project-smoke` fixture (`build.tur` + nested `src/app/` with a
  cross-module import) and `tests/run-build-project.sh` (the `tur_build_project`
  ctest target) cover: exit 0, the manifest not being compiled as source, a
  resolved cross-module import (runs to exit 42), the Phase 2 missing-export
  failure, and a flat-layout project (`build.tur` + root-level source, no
  `src/`) that exercises the shallow-scan fallback and re-confirms the stray
  manifest is skipped.
- `CLAUDE.md` ("Per-file Commands Inside a Spice") and
  `docs/guides/developing-spices-guide.md` now describe the descent/skip/
  include-resolution/export-validation behavior and link back to this plan,
  replacing the previously aspirational "already configures itself" wording.

## Interaction with the scscm refactor

This change is **necessary but not sufficient** for the scscm validation item
`tur build ../turmeric-spices/spices/scscm exits 0`. Even with manifest-driven
descent, the scscm build still fails at a deeper layer: `codegen.tur` uses
`head` / `tail` (defined only in `lexer.tur`) without stubbing or importing
them, and separate compilation does not resolve cross-file helpers. Those names
are also not in `scscm/lexer`'s `:exports`. That layer is fixed by
`docs/scscm-spice-import-refactor-plan.md` **Phase 2** (real `(import ...)` +
expanded exports). So:

- This plan unblocks scscm Phase 1 step 1 mechanically (no more
  manifest-as-source error).
- The scscm checklist item `tur build <spice> exits 0` only passes once both
  this plan **and** scscm Phase 2 land.

Recommend sequencing: land this CLI change first, then scscm Phase 2, then
re-run the scscm validation checklist.

## Risks

1. **File-set drift between `build` / `run` / `test`.** Three commands resolve
   "what files make up this project" with slightly different logic. If
   `cmd_build_project` collects a different set than `cmd_run`, users get
   confusing "works under run, fails under build" reports. Mitigation: the
   Phase 1 step 3 refactor into one shared helper.
2. **Sweeping up non-source `.tur` files.** Recursive `src/` collection might
   pull in files that were never meant to be modules. Mitigation: scope
   recursion to `src/` only, exclude known non-source subtrees, and confirm the
   exclusion list against what `cmd_run` already honors.
3. **`:exports` map parsing changes behavior elsewhere.** Activating
   `m.n_exports` for map-form manifests changes the `cmd_pkg_emit_cmake`
   branch that is currently dead. Mitigation: Phase 2 step 3 audit; gate behind
   tests for `emit-cmake` output.
4. **`--shared` / `--manifest` semantics.** The directory build also serves the
   shared-library path (`--shared`, `--manifest <path>`). The manifest-driven
   collector must preserve those (exported defns getting extern linkage, the
   `exports.manifest` accumulation). Mitigation: route `cmd_build_project`
   through the same `cmd_build_multi` core rather than reimplementing the link
   step.

## Validation checklist

- [x] `tur build <small-project-with-src>` exits 0 and produces the expected
      artifact, without compiling `build.tur`. (`build-project-smoke`)
- [x] `tur build <project-with-nested-src/<pkg>/>` exits 0 (recursion works).
      (`build-project-smoke` uses `src/app/`.)
- [x] `tur build <bare-dir-no-manifest>` behaves exactly as before
      (no regression to the non-project path). (`run-build-shared` green.)
- [x] `tur build --shared <project>` still emits a shared lib + manifest.
      (`run-build-shared` exercises the shared core unchanged.)
- [x] `tur run <project>` and `tur build <project>` agree on the module set.
      (Structural after T1: both call the shared
      `resolve_include_dirs_from_manifest`. T2 and T3 (below) are now fixed, so
      both same-project (`tur run` from an arbitrary cwd) and cross-spice
      (`:spices` dep modules linked under `tur build <dir>`) cases hold
      behaviorally; `build-project-links-cross-spice-dep` exercises the latter.)
- [x] `ctest` for the new CLI fixtures passes (`tur_build_project`);
      `bash tests/run.sh` green modulo the pre-existing ASan leaks
      (`docs/asan-debug-leaks-plan.md`).
- [x] (Phase 2) `:exports` map keys populate `m.exports`; a declared export
      with no source file fails the build (`build-project-missing-export-fails`);
      `emit-cmake` resolves module-name exports to `src/<key>.tur`
      (`test-emit-cmake.sh` green, 15 passed).
- [ ] After scscm Phase 2 lands: `tur build ../turmeric-spices/spices/scscm`
      exits 0.

## Follow-up tasks

### T1 -- Consolidate `cmd_run`'s include-resolution onto the shared helper

Phase 1 factored the test runner's project include-dir resolution into
`resolve_project_include_dirs` (`src/main.c`) and pointed `cmd_test` and
`cmd_build_project` at it. **`cmd_run` still carries its own inline copy** of
the same logic (the `spice_inc_dirs` build-up around `src/main.c:2556-2618`),
left untouched in Phase 1 because it is interleaved with dependency
fetch/verify (`pkg_fetch_all`, lock checks) and would have widened the
blast radius. This task removes that third copy so all three commands resolve
the include path identically (the drift risk called out under "Risks").

What the consolidation must account for (why it was deferred):

- **Workspace members.** `cmd_run` resolves `:spices` entries that are
  workspace siblings via `pkg_is_workspace_member` / `pkg_workspace_member_path`
  (`src/main.c:2483,2514,2568-2574`), preferring the sibling's on-disk path
  over any declared `:url`/`:ref`. `resolve_project_include_dirs` does **not**
  yet handle workspace members -- it only does `:path` / `:ref` / `:subdir`
  plus the monorepo-sibling fallback (mirrored from the old `cmd_test` block).
  Before `cmd_run` can adopt the helper, the helper must learn the
  workspace-member resolution, or it will regress workspace builds.
- **Fetch/verify stays in `cmd_run`.** The helper is pure path resolution; the
  fetch/lock/offline logic must remain in `cmd_run` and run *before* the helper
  is called (the helper assumes deps are already on disk).

Steps:
1. Extend `resolve_project_include_dirs` to resolve workspace-member `:spices`
   entries (port the `pkg_workspace_member_path` branch), keeping the existing
   `:path`/`:ref`/`:subdir` + monorepo-fallback behavior.
2. Replace the inline `spice_inc_dirs` build-up in `cmd_run` with a call to the
   helper (after fetch/verify), keeping `cmd_run`'s existing ownership/free
   paths intact.
3. Verify `tur run` against a workspace project and a `:path`/`:ref` dep
   project; confirm `tur_spice_resolver_tests` and the REPL spice tests stay
   green.
4. Add a check that `tur run <project>` and `tur build <project>` collect the
   same module/include set (closes the open validation item above).

**Implemented.** Done as designed, with two scope clarifications:

- The duplicated logic is now a single core, `resolve_include_dirs_from_manifest
  (root, m, include_own_src, ...)`, that does workspace-member -> `:path` ->
  `:ref` -> `spices/<name>` resolution plus `:subdir`, the `src/` preference,
  and the monorepo-sibling fallback. `resolve_project_include_dirs` is now a
  thin walk-up + read-manifest wrapper over it (`include_own_src = true`), used
  by `tur test` and `tur build <dir>`; `cmd_run` calls the core directly with
  its already-loaded manifest and `include_own_src = false` (its entry file
  lives inside `src/`, so the resolver already searches it -- preserving
  `cmd_run`'s exact include set). Unifying gave `tur test`/`tur build`
  workspace-member resolution and gave `cmd_run` the monorepo fallback +
  skip-nonexistent behavior; both are strict improvements that no longer drift.
- Regression coverage: `tur_spice_resolver_tests` (50), `tur_build_project`
  (6), the REPL spice ctest targets (7), and the full `bash tests/run.sh`
  (1043) all stay green after the change.

**Step 4 -- agreement is now structural, not sampled.** Because `cmd_run` and
`cmd_build_project` call the *same* resolver (differing only in the
`include_own_src` flag), the include set is identical by construction; a
sampled end-to-end comparison would be weaker than that guarantee. A dedicated
`tur run <project>` vs `tur build <project>` behavioral test is also blocked by
two *unrelated, pre-existing* limitations surfaced while validating T1, now
recorded as follow-ups:

### T2 -- project-mode `tur run` runtime path is relative to cwd

Project-mode `tur run` (`tur run --offline` / `--release` with no file arg)
must `cd` into the project to find `build.tur`, but the cc step then references
the turmeric runtime by a cwd-relative path (`src/runtime/hamt.c`), which only
resolves when cwd happens to be the turmeric source tree. From any other
project dir the link fails (`fatal error: src/runtime/hamt.c: No such file`).
The runtime source path should be resolved absolutely (relative to the located
stdlib/runtime root, as `tur build` does) so project-mode `tur run` works from
an arbitrary directory.

**Implemented.** The fix is in `cmd_build` (`src/main.c`), the shared final
compile/link step that both `tur run` and `tur build` route through, so it
covers every entry point at once:

- `resolve_turmeric_root` derives the turmeric source root (the directory that
  holds both `stdlib/` and `src/runtime/`) as the parent of the already-located
  stdlib root from `resolve_stdlib_root`. This works for the dev layout
  (`<root>/stdlib`) and the prefix-installed layout
  (`<prefix>/share/turmeric/stdlib`), and falls back to `.` (legacy
  cwd-relative behavior) when only the bare `"stdlib"` fallback is available.
- `rewrite_autolink_relative_paths` rewrites each `__tur_autolink__` flag token
  so a relative path -- a bare path like `src/runtime/hamt.c`, or the argument
  of `-I` / `-L` like `-Isrc/runtime` -- is anchored at that root. Absolute
  paths and non-path flags (`-l`, `-f`, `-D`, ...) pass through unchanged. This
  is applied to the collected autolink flags just before the cc command is
  assembled; the pre-existing `-lturi` SDK block still prepends its own absolute
  `-I/-L` flags, so the rewrite is harmless redundancy there and the actual fix
  for the bare-source-file (`hamt.c`) case.
- Regression coverage: `run-project-resolves-runtime-from-foreign-cwd` in
  `tests/run-build-project.sh` (the `tur_build_project` ctest target) builds a
  hamt-using project via `tur run --offline` from a scratch cwd, in both
  explicit-file and walk-up project modes, asserting the
  `src/runtime/hamt.c: No such file` failure no longer occurs. The test fails
  against the pre-fix binary.

### T3 -- `tur build <dir>` does not link cross-spice `:spices` modules

`cmd_build_project`'s build set is the project's own `src/` scan, so a
`:spices` dependency's modules are resolved for type-checking (the include path
is correct) but never compiled/linked -- the dependent's generated
`#include "<dep>__<mod>.h"` has no backing file and cc fails. `tur build <dir>`
should also compile each resolved `:spices` dep's modules (or build deps as
libraries and link them). Until then, cross-spice builds work only via
`tur run`'s single-entry inlining, not separate-compilation `tur build`.

**Implemented.** `cmd_build_project` (`src/main.c`) now folds each resolved
`:spices` dependency's modules into the build set so the dependent links under
separate compilation:

- After collecting the project's own `src/` modules, it re-resolves the
  dependency `src/` dirs via `resolve_include_dirs_from_manifest(root, m,
  include_own_src=false, ...)` (the same resolver `tur run`/`tur test` use --
  workspace member -> `:path` -> `:ref` -> `spices/<name>`, with `:subdir`,
  `src/` preference, and the monorepo-sibling fallback), then
  `collect_tur_recursive`s each dep dir.
- `cmd_build_multi_files` gained a per-file module-name-root array
  (`file_src_roots`).  A dep module is named relative to its *own* dep `src/`
  (e.g. `<dep>/src/sib/api.tur` -> module `sib/api` -> `sib__api.h`), which is
  exactly the header the importer's generated `#include` references; project
  modules keep using the single project `src_root`.
- Module-name collisions are de-duplicated (the build keeps the project's own
  module, or the first dep to define a given qualified name), so adding deps
  cannot silently shadow a project module or emit duplicate `.c` files.
- Gated to executable builds (`!shared`): a `--shared` `.so` links its
  dependencies separately and must not absorb their modules or accumulate their
  exported defns into the library's `exports.manifest`.
- Scope: direct `:spices` deps only.  Transitive deps-of-deps are not pulled in
  (the resolved include/build set covers this project's direct dependencies);
  a dep that itself imports a third spice would still need that spice on the
  build's include path.  No autolink-marker scanning is added to the multi-file
  link path -- that remains a separate pre-existing limitation of
  `cmd_build_multi_files`, unrelated to cross-spice linking.
- Regression coverage: `build-project-links-cross-spice-dep` in
  `tests/run-build-project.sh` builds a `consumer` project that imports
  `sib/api` from a `:path`-linked `sib` spice via `tur build <dir>`, asserts
  exit 0 and a runnable binary that returns the dep's value (42).  The test
  fails against the pre-fix binary (`sib__api.h: No such file`).

With T3 in place, the previously-deferred validation item -- `tur run
<project>` and `tur build <project>` agreeing on the module set -- holds
behaviorally for cross-spice projects too, not just structurally.

## Out of scope

- The scscm stub-to-import migration itself (covered by
  `docs/scscm-spice-import-refactor-plan.md`).
- Cross-file helper resolution under separate compilation (solved by real
  imports + exports, not by the build driver).
- Workspace `:members` resolution (tracked in
  `docs/local-spice-dev-workflow-plan.md`); the per-dep `:path` / `:ref`
  resolution reused here is the existing mechanism.
