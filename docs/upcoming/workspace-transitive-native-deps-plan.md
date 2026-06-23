# Workspace transitive native-deps across `:members`

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
