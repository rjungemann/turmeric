---
title: Drop Just Dependency -- CMake Bootstrap + tur run
category: Plans
description: Plan to remove references to the upstream `just` binary from documentation, replacing bootstrap-build steps with plain CMake commands and spice-task steps with `tur run`.
---

# Drop `just` Dependency from Docs

## Goal

Remove the requirement to have the upstream `just` binary installed. Replace
all user-facing doc references with two simpler primitives:

| Context | Replacement |
|---------|-------------|
| Bootstrap-build (compiling `tur` itself) | Plain `cmake` commands |
| Spice / project tasks (build, test, etc.) | `tur run <recipe>` |

This is already the path CLAUDE.md recommends for fresh containers ("use CMake
directly"). The plan extends that recommendation to every doc that still shows
`just …` to a human reader.

---

## Rationale

- `just` is an extra binary users must install separately; `cmake` ships with
  every major IDE and CI image, and `tur run` is built into the compiler itself.
- `tur run` implements the same Justfile subset that `just` does, so the
  Justfiles stay in the repo -- only the invocation style changes for users.
- Removing the `just install` prerequisite shortens the "getting started" story
  by one step.

---

## Two canonical commands

### 1. Bootstrap (compile `tur` from source)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
```

The compiler lands at `./build/tur`. Run tests with:

```sh
bash tests/run.sh
```

Release build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-release -j
```

### 2. Spice / project tasks (`tur run`)

```sh
tur run              # list recipes
tur run build        # compile the spice (debug)
tur run test         # build + run tests
tur run release      # release build
tur run docs         # generate API docs
tur run wasm         # build WASM module
tur run web-dev      # Vite dev server
tur run clean        # remove build artefacts
```

---

## Files to update

### Core docs / meta

| File | Current text | Change |
|------|-------------|--------|
| `CLAUDE.md` (Build System section) | `tur run build  # (also: just build)` / `just docs` / `just wasm` / `just web-dev` | Drop `(also: just …)` annotations; replace bare `just` calls with `tur run`; keep the "Fresh containers" CMake block as the primary bootstrap path |
| `README.md` | Any `just build` / `just test` invocations | Replace with CMake bootstrap + `tur run` |

### Guides

| File | Change |
|------|--------|
| `docs/guides/devcontainer-guide.md` | Replace "Run just tasks" section (`just`, `just test`, `just repl`, etc.) with `tur run` equivalents; remove `just` from container tool-list description |
| `docs/guides/quickstart.md` | If any `just` setup steps are present, replace with CMake bootstrap |
| `docs/guides/releases-and-installation-guide.md` | Replace any `just build` / `just install` with CMake + `tur run` |
| `docs/guides/developing-spices-guide.md` | Replace `just build` / `just test` recipe invocations with `tur run build` / `tur run test` |
| `docs/guides/tur-run-guide.md` | Already `tur run`-centric; review intro paragraph for any lingering `just` cross-references |

### Design / archive docs

These are historical artefacts; the change is lower-priority but keeps the
archive accurate.

| File | Change |
|------|--------|
| `docs/archive/build-and-test-ux-plan.md` | Add a note at the top that `just` references are superseded by CMake + `tur run` |
| Other archive plans that mention `just` | Same brief supersession note |

---

## Non-goals

- **Do not remove `Justfile` from the repo.** `tur run` reads it; it remains
  the single source of recipe truth.
- **Do not remove `just` from `.devcontainer/`.** Developers who prefer the
  upstream binary can still use it; we just stop requiring it in documentation.
- **Do not rewrite the `tur-run-guide.md`.** It already documents `tur run`
  correctly.

---

## Acceptance criteria

1. No doc visible to a first-time user says "install `just`" as a prerequisite.
2. Bootstrap-build instructions show only `cmake` commands.
3. Post-bootstrap task invocations show only `tur run <recipe>`.
4. `just build` / `just test` etc. may appear in parenthetical "equivalently
   with upstream just" notes, but are not the primary instruction.
