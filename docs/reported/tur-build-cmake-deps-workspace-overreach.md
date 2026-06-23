---
status: open
severity: medium
discovered: 2026-06-23
discovered-by: tourist-build-investigation
---

# `tur build .` cmake-deps transitive walk over-reaches workspace siblings

## Summary

When `tur build .` runs from a spice that has transitive `:cmake-deps`,
the natural completion of the project-mode dep-build chain is to call
`pkg_collect_transitive_cmake_deps` + `pkg_cmake_build` -- the same path
`cmd_run` already uses (`src/main.c:3205-3242`).

Doing so for `../turmeric-spices/spices/tourist` discovers far more than
tourist's actual closure of `:spices`: `pkg_collect_transitive_cmake_deps`
in `src/compiler/pkg.c:2316` explicitly seeds the worklist with EVERY
workspace sibling, comment:

> A sibling member's modules are importable from the root spice without an
> explicit :spices entry (see auto_append_spice_includes in main.c), so its
> :cmake-deps must participate in the build too.

For tourist that drags in `glfw`, `rtaudio`, `rtmidi`, `sndfile`, and other
libraries that tourist neither imports nor needs. The cmake configure
then fails, dragging the whole `tur build` down.

## Real-world impact

After the header-generation fix landed
(`docs/archive/project-mode-no-stdlib-autoload.md` → resolved + the
`realpath(root)` fix in `cmd_build_project`), tourist's `.h` files now
generate correctly under `tur build .`. The cc invocation still fails
with `yyjson.h not found` because `tur build .` does not chain into the
transitive cmake-deps build that would have produced yyjson's headers
and link-time library.

A minimal `tur build .` cmake-deps autobuild patch would unblock that
yyjson case, but is currently held back by the over-eager workspace
seeding: a tourist build that recursively pulls in glfw / rtaudio /
sndfile to satisfy "any workspace sibling's cmake-deps" is not the right
default.

## Repro

```
$ cd ../turmeric-spices/spices/tourist
$ rm -rf build cmake && tur build . 2>&1 | grep yyjson
fatal error: 'yyjson.h' file not found
```

The header chain itself works (httpd__types.h, template__render.h, etc.
all generate into tourist's build/obj/). Only the cmake-deps-supplied C
include path is missing.

## Root cause

Two overlapping gaps:

1. **`cmd_build_project` doesn't run cmake-deps.** A minimal patch would
   mirror `cmd_run`'s block in `src/main.c:3199-3242` (collect transitive
   cmake-deps, gen + build them, write the lockfile) inside
   `cmd_build_project` at `src/main.c:4106+`. Wiring this up was
   prototyped and reverted in this session -- it produces the over-reach
   below.
2. **`pkg_collect_transitive_cmake_deps` seeds workspace siblings.**
   `src/compiler/pkg.c:2316` calls `collect_workspace_sibling_dirs` and
   appends EVERY sibling to the worklist. For a project that lives in a
   large workspace (turmeric-spices has ~30 members), this turns the
   "cmake-deps for tourist" question into "cmake-deps for the entire
   workspace" -- which (a) is slow, (b) fails when unrelated members'
   cmake_minimum_required is below 3.5, and (c) is the wrong semantic:
   tourist should only need its own transitive `:spices` closure's
   cmake-deps, not every sibling's.

## Fix direction

The right shape is probably:

- Narrow the workspace seeding in
  `pkg_collect_transitive_cmake_deps` to only the closure reachable via
  the manifest's own `:spices` (transitively); drop the
  every-sibling seed.
- Then add cmake-deps autobuild to `cmd_build_project`.

Either change in isolation is wrong: narrowing without adding the
autobuild leaves `tur build` still unable to find yyjson; adding the
autobuild without narrowing fails configure on unrelated workspace
siblings.

A simpler intermediate workaround: have `pkg_collect_transitive_cmake_deps`
take a `bool include_workspace_siblings` flag; pass `true` from `cmd_run`
(preserving today's behaviour for the "I want to run this spice
locally" case) and `false` from `cmd_build_project` (deliberately
restrictive: only build cmake-deps reachable from the manifest's own
`:spices`). The semantic-preserving signature edit lets us land the
narrower walk without touching `cmd_run`'s long-standing behaviour.

## Links

- `src/compiler/pkg.c:2316` -- workspace-sibling seeding site
- `src/main.c:3199-3242` -- the cmd_run cmake-deps block to mirror
- `docs/archive/project-mode-no-stdlib-autoload.md` -- the upstream
  header-generation fix that unblocks the cc-time chain to here
