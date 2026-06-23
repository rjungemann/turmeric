# Workspace transitive native-deps across `:members`

## Status: RESOLVED

`pkg_collect_transitive_cmake_deps` now seeds its walk from the enclosing
workspace's `:members` siblings in addition to the root manifest's `:spices`.
A new static helper `collect_workspace_sibling_dirs` (in
`src/compiler/pkg.c`) walks up to the enclosing workspace, confirms the root
project is itself a listed member, and returns the directories of the *other*
members; each is pushed onto the same worklist the `:spices` walk uses (the
visited set dedups any overlap). This mirrors the unconditional
workspace-sibling `src/` resolution already done by
`auto_append_spice_includes` in `src/main.c`: because any sibling member's
modules are importable without an explicit `:spices` entry, any sibling's
`:cmake-deps` must participate in the build too.

Open question resolved in favor of seeding **all** workspace members (not only
those a sibling explicitly depends on via `:spices`), to stay consistent with
the include-path resolver, which already adds every sibling's `src/`
unconditionally. (The "only when declared" alternative is already covered by
the pre-existing `:spices` traversal, so it would have been a no-op.)

Regression coverage: `tests/fixtures/workspace-member-cmake-deps/` plus a new
`workspace-member-cmake-deps-seed` case in
`tests/run-transitive-cmake-deps.sh` -- two members that do NOT list each
other in `:spices` declare the same-named `:cmake-deps` with different
`:ref`; building `member-b` must seed `member-a`'s dep from `:members` and
emit the conflict diagnostic.

## Summary

`pkg_collect_transitive_cmake_deps` does not currently seed itself from a
workspace's `:members` list. As a result, when a workspace member declares a
native (CMake) dependency, sibling members that transitively depend on it do
not automatically pick that native dep up at build time.

Today this is worked around on the spice side (each member redeclares the
native deps it needs). A recent spices-side change (commit `3fc3e76`) is
correct as-is under the current behavior -- no spice-side edit is warranted.

## Severity

Low / quality-of-life. Not a correctness bug; spices can and do redeclare
native deps locally. Becomes a papercut as workspaces grow and the same
native dep is referenced from multiple members.

## Repro

A workspace with two members where member B depends on member A, and A
declares a native CMake dep (e.g. via `:cmake-deps` or equivalent). Building
B does not automatically inherit A's native dep -- B must redeclare it.

## Root cause

`pkg_collect_transitive_cmake_deps` walks `:spices` transitively but is not
seeded from the parent workspace's `:members` list. Workspace siblings are
resolved for source/module purposes (see "Per-file Commands Inside a Spice"
in `CLAUDE.md`) but their native-dep contributions are not aggregated.

File:line to confirm during the fix: `pkg_collect_transitive_cmake_deps`
in the manifest/build pipeline (search the compiler source for the symbol).

## Fix direction

Seed `pkg_collect_transitive_cmake_deps` from the enclosing workspace's
`:members` in addition to the current `:spices` traversal. Treat each
member's declared native deps as transitively contributed to any sibling
that depends on that member.

Open questions for the implementer:

- Should this apply to all members unconditionally, or only when a sibling
  declares a dependency on the member? (The latter matches how `:spices`
  transitivity works today and is probably the right default.)
- Interaction with `:path`-based deps that point outside the workspace --
  presumably unchanged; only `:members` siblings get the new seeding.

## Scope

Turmeric-side only. No spice-side changes needed once this lands; existing
redeclarations in spice manifests can be removed opportunistically but are
not load-bearing.
