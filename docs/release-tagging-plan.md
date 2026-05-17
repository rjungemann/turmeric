# Release Tagging & Versioning -- Plan

> **Status:** Draft -- Not Started
>
> **Last updated:** 2026-05-16

---

## Overview

A semi-automatic release workflow: the developer bumps the version locally via a
`just` task, which commits the change and pushes a tag. The existing GitHub
Actions release workflow (`.github/workflows/release.yml`) fires automatically
on any `v*` tag, builds release binaries for all four platforms, and creates a
GitHub Release with those artifacts attached.

The human touch is deliberate -- no robot auto-increments versions. The
developer decides what kind of change this is (patch / minor / major) and runs
one command.

---

## Canonical Version Location

Add a single `VERSION` file at the repo root containing only the bare version
string (no `v` prefix):

```
0.2.1
```

This is the single source of truth. Everything else derives from it:

| Consumer | How it reads the version |
|---|---|
| `src/wasm_glue.h` | `TURMERIC_VERSION` macro, updated by bump tasks |
| GitHub Actions | `${GITHUB_REF_NAME}` (the pushed tag) |
| `just` bump recipes | `cat VERSION` |

---

## Just Tasks

Add three new recipes to `Justfile`. Each recipe:

1. Reads the current version from `VERSION`.
2. Computes the new version (bumps the appropriate component, resets lower
   components to zero).
3. Writes the new version back to `VERSION`.
4. Patches the `TURMERIC_VERSION` macro in `src/wasm_glue.h`.
5. Stages both files and creates a commit: `chore: bump version to vX.Y.Z`.
6. Creates an annotated tag `vX.Y.Z`.
7. Pushes the commit **and** the tag to `origin`.

The push of the tag is what triggers the GitHub Actions release build.

```just
# Bump the patch version (0.2.1 -> 0.2.2) and push a release tag.
bump-patch:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$MAJOR.$MINOR.$((PATCH + 1))"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/wasm_glue.h
    rm -f src/wasm_glue.h.bak
    git add VERSION src/wasm_glue.h
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# Bump the minor version (0.2.1 -> 0.3.0) and push a release tag.
bump-minor:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$MAJOR.$((MINOR + 1)).0"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/wasm_glue.h
    rm -f src/wasm_glue.h.bak
    git add VERSION src/wasm_glue.h
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"

# Bump the major version (0.2.1 -> 1.0.0) and push a release tag.
bump-major:
    #!/usr/bin/env bash
    set -euo pipefail
    OLD=$(cat VERSION)
    IFS='.' read -r MAJOR MINOR PATCH <<< "$OLD"
    NEW="$((MAJOR + 1)).0.0"
    echo "$NEW" > VERSION
    sed -i.bak "s/TURMERIC_VERSION \"$OLD\"/TURMERIC_VERSION \"$NEW\"/" src/wasm_glue.h
    rm -f src/wasm_glue.h.bak
    git add VERSION src/wasm_glue.h
    git commit -m "chore: bump version to v$NEW"
    git tag -a "v$NEW" -m "Release v$NEW"
    git push origin HEAD "v$NEW"
```

---

## GitHub Actions Integration

The existing `.github/workflows/release.yml` already:

- Triggers on `push` to tags matching `v*`.
- Builds `tur` + `libturi.a` for `linux-x86_64`, `linux-aarch64`,
  `macos-arm64`, and `macos-x86_64`.
- Packages each into a `.tar.gz` archive named
  `turmeric-<tag>-<target>.tar.gz`.
- Creates a GitHub Release with `generate_release_notes: true` and attaches
  all four archives.

No changes to the workflow are needed to support this plan.

---

## Migration Steps

These are the one-time setup steps to get from the current state to this
workflow.

1. **Create `VERSION`** -- write the current version string (`0.2.0` or
   whatever the live release is) into a new `VERSION` file at the repo root.
2. **Sync `src/wasm_glue.h`** -- confirm `TURMERIC_VERSION` matches the
   `VERSION` file. Adjust if they have drifted.
3. **Add `.gitattributes` entry** (optional) -- mark `VERSION` as
   `export-subst` if you ever want it substituted in archives.
4. **Add the three `bump-*` recipes** to `Justfile`.
5. **Commit the setup** -- `git add VERSION Justfile && git commit -m "chore: add VERSION file and bump-* just tasks"`.
6. **Test with a dry run** -- run `bump-patch` on a scratch branch and verify
   the commit, tag, and push look correct before doing it on `main`.

---

## Typical Release Workflow

```sh
# 1. Finish your work on main (or merge your PR).
git checkout main
git pull

# 2. Pick the appropriate bump command.
just bump-patch   # bug fix / tiny improvement
just bump-minor   # new feature, backwards-compatible
just bump-major   # breaking change

# 3. Watch CI.
# GitHub Actions fires automatically on the pushed tag.
# Binaries appear as release assets within ~5 minutes.
```

---

## Notes & Alternatives Considered

- **Why a `VERSION` file instead of CMake project version?** CMake's
  `project(turmeric VERSION x.y.z)` is the idiomatic C approach, but reading
  it from a shell script requires parsing CMakeLists.txt. A flat `VERSION` file
  is trivially shell-readable with `cat VERSION`.
- **Why not GitHub's auto-tag on merge?** Keeps the developer in control of
  when a release actually ships. Auto-tagging every merge creates noise and
  forces strict PR discipline to avoid accidental releases.
- **Why annotated tags?** Annotated tags (`-a`) carry a tagger, date, and
  message -- they show up cleanly in `git describe` and are preferred for
  release marks over lightweight tags.
- **`sed -i.bak` portability** -- macOS `sed` requires a backup extension with
  `-i`; the recipe removes the `.bak` file immediately after. This keeps the
  recipe working on both Linux and macOS without installing GNU sed.
