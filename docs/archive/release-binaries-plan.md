# Plan: Fix Tagged-Release Precompiled Binaries

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** CI / Release

---

## Overview

`.github/workflows/release.yml` already exists and is wired to push of `v*`
tags. It builds a 2x2 matrix (Linux x86_64 / aarch64, macOS arm64 / x86_64),
packages each as `turmeric-<tag>-<target>.tar.gz` containing `tur`,
`libturi.a`, and a handful of headers, and a `release` job publishes them to
a GitHub Release with auto-generated notes.

Despite that, every recent tag (`v0.9.0` through `v0.12.0`) has produced
**no release page and no published binaries**. The workflow runs all
look the same: three matrix legs finish in ~30s, the fourth times out at
24h0m, and the dependent `release` job never executes.

---

## Root cause

```text
$ gh run view 26423084454 --repo rjungemann/turmeric
JOBS
  ✓ Build (macos-arm64)    in 19s
  X Build (macos-x86_64)   in 24h0m0s   <-- "exceeded max execution time
                                             while awaiting a runner"
  ✓ Build (linux-x86_64)   in 28s
  ✓ Build (linux-aarch64)  in 36s
  - Create Release         in 0s        <-- skipped (needs: build)
```

Two compounding problems:

1. **`macos-13` runners are no longer schedulable.** The matrix pins
   `macos-x86_64` to `runs-on: macos-13`. GitHub has retired the
   `macos-13` hosted-runner image, so the job sits in the queue until
   the 24-hour job-level timeout fires. Each tag therefore wastes
   ~24h of queue time and ends with a cancelled run.

2. **`release` job is all-or-nothing.** The publishing job declares
   `needs: build` with no `if:` override. When *any* matrix leg fails
   or is cancelled, the `release` job is skipped, so even the three
   successfully-uploaded artifacts (linux-x86_64, linux-aarch64,
   macos-arm64, which `gh run view` confirms exist) are discarded
   when the run cleans up its artifact retention window.

A secondary issue showing up in the same run logs:

3. **Node 20 actions deprecated.** `actions/checkout@v4` and
   `actions/upload-artifact@v4` still run on Node 20; GitHub forces
   Node 24 in June 2026. Not blocking today, but trivial to fix
   alongside the rest.

---

## Proposed fix

Two changes to `.github/workflows/release.yml`, plus a verification step.

### 1. Replace the `macos-13` leg with native arm64 on `macos-latest`

`macos-latest` is an Apple-silicon runner. Build natively for arm64,
ship one macOS tarball, and drop Intel-Mac support from precompiled
releases. (Originally this section proposed a universal2 build; the
attempt failed -- see "Discovered during execution: universal2 won't
work" below.) Intel-Mac users today fall back to
`brew install --HEAD turmeric` (now that the formula installs stdlib
correctly) or build from source.

Matrix shape after the change:

```yaml
matrix:
  include:
    - target: linux-x86_64
      os: ubuntu-latest
    - target: linux-aarch64
      os: ubuntu-24.04-arm
    - target: macos-arm64
      os: macos-latest
```

### 2. Decouple `release` from individual matrix-leg failures

Change the publish job so it runs as long as *some* artifacts were
produced, rather than requiring every leg to succeed:

```yaml
release:
  needs: build
  if: always() && needs.build.result != 'cancelled'
  ...
```

Combined with the matrix's existing `fail-fast: false`, this means a
single broken target (e.g. when a future GitHub runner image change
breaks Linux aarch64) still produces a release with the binaries that
did build. The release notes will reflect which targets shipped via
the file list itself.

If we want stricter guarantees, an alternative is to keep `needs:
build` strict but add a manually-triggerable `workflow_dispatch`
input that lets us re-run just the publish step against existing
artifacts. The lenient approach is preferred for now -- partial
releases are recoverable (re-run the workflow, attach missing
binaries by hand) while a fully-blocked release is the current
failure mode we are trying to escape.

### 3. Housekeeping

- Bump `actions/checkout@v4` -> `@v5` (Node 24).
- Bump `actions/upload-artifact@v4` -> `@v5` and
  `actions/download-artifact@v4` -> `@v5`.
- Add a `sha256sums.txt` artifact: after the matrix completes, the
  `release` job runs `sha256sum artifacts/* > sha256sums.txt` and
  uploads it alongside the tarballs. Cheap, and the typical
  "is this binary tampered with" check users expect.
- Add a one-line smoke test inside each `Build` job before packaging:
  `./build/tur --version` (or an equivalent invocation -- confirm
  what exit-zero command exists). Catches a broken binary before it
  ships rather than after.

---

## Verification

Plan-the-plan: how do we know this works without burning real version
numbers?

1. Push a throwaway tag (e.g. `v0.0.0-test1`) to a branch that touches
   only the workflow. Confirm the run finishes in ~5 minutes with all
   three legs green and a release page appears with three tarballs +
   `sha256sums.txt`.
2. Download `turmeric-v0.0.0-test1-macos-arm64.tar.gz` on an
   Apple-silicon Mac, extract, run `./tur --version`.
3. Repeat on Linux x86_64 and aarch64 (Docker `--platform=linux/arm64`
   on an AS Mac is fine for the latter).
4. Delete the test tag and release: `gh release delete v0.0.0-test1
   --cleanup-tag --yes`.
5. Cut the real next release tag and confirm artifacts appear.

---

## stdlib bundling (in scope)

`tur` cannot find its standard library without one of:

- `$TUR_STDLIB_DIR` set explicitly, or
- `<exe_dir>/stdlib/macros.tur` reachable (sibling-in-tarball layout), or
- `<exe_dir>/../share/turmeric/stdlib/macros.tur` reachable (prefix layout).

All three probes already exist in `src/main.c:188` (`resolve_stdlib_root`).
So bundling is a packaging change, not a code change:

- **Tarball:** add `cp -R stdlib dist/` to the Package step. Extracts to
  `tur` + `stdlib/` as siblings, picked up by probe #2. Adds ~200-300 KB
  compressed.
- **Homebrew formula (`Formula/turmeric.rb`):** add
  `(share/"turmeric").install "stdlib"` to the `install` block. Lands
  files at `<prefix>/share/turmeric/stdlib/`, picked up by probe #3.

The formula's `test do` block must actually exercise stdlib loading,
otherwise a regression in stdlib installation passes silently. Use
`tur --interpret` with a body that uses a stdlib-defined form (e.g.
`when` from `stdlib/macros.tur`); a plain `(println "hello")` against
`tur run` does not depend on stdlib and would not catch the bug. The
interpreter path is chosen over `tur run` deliberately -- see the
runtime-source caveat below.

## Discovered during execution: universal2 won't work

The first test tag (`v0.0.0-test1`) failed the macOS leg at the
Configure step because `;` in `-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64`
was parsed by `bash` as a command separator after
`${{ matrix.cmake_extra }}` was substituted into the run line.
Shell-quoting the value fixed Configure (`v0.0.0-test2`), but the
Build step then failed with:

```
src/async/fiber_ctx_arm64.S:18:19: error: brackets expression not
supported on this target
    stp x29, x30, [x0, #80]
```

The project has hand-written assembly for each architecture
(`src/async/fiber_ctx_arm64.S` and `src/async/fiber_ctx_x64.S`) and
`src/CMakeLists.txt:135` selects between them with:

```cmake
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_arm64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND TUR_CORE_SOURCES async/fiber_ctx_x64.S)
```

A universal2 build on an arm64 runner adds only the arm64 `.S`, then
the compile-driver tries to assemble that file *twice* -- once per
architecture slice -- and fails for x86_64. Even passing
`-DCMAKE_SYSTEM_PROCESSOR=x86_64` explicitly doesn't help: `project()`
on line 2 of the top-level `CMakeLists.txt` re-detects the host
processor and overrides the CLI value.

Two ways to make universal2 actually work, both deferred:

- Edit the conditional to prefer `CMAKE_OSX_ARCHITECTURES` over
  `CMAKE_SYSTEM_PROCESSOR` on Apple, *and* set per-source
  `OSX_ARCHITECTURES` properties so each `.S` file is excluded from
  the architecture slice it doesn't support.
- Build two thin static libraries (one per arch) and stitch them
  together with `lipo`.

Both expand scope beyond "make tagged releases work" and require
non-trivial CMake changes; arm64-only macOS is the simplest path
that ships a working binary today. Intel-Mac coverage can return
later if there's demand.

## Discovered during execution: runtime sources also missing

While verifying the smoke test, a second bundling gap surfaced.
`stdlib/hamt.tur` carries the marker

```
/* __tur_autolink__: src/runtime/hamt.c -Isrc/runtime */
```

…which `src/main.c:1167` (`Phase S2`) parses out of the generated C
and appends to the `cc` invocation. The path is **literal and
cwd-relative**, so any `tur run` / `tur build` from outside the
repository tree fails:

```
$ cd /tmp && tur run hello.tur
clang: error: no such file or directory: 'src/runtime/hamt.c'
tur: cc invocation failed (status 256)
```

`stdlib/turi/eval.tur:42` carries a similar marker (`-lturi -Isrc
-Isrc/compiler -Isrc/runtime`). So a distributed `tur` can run the
interpreter cleanly (no autolink involved) but cannot use the
C-codegen-and-link path that `tur run` / `tur build` rely on.

This is **out of scope for this plan** -- the immediate goal is just
to make tagged releases publish working binaries -- but it is the
next blocker for "a downloaded `tur` binary is fully usable."
Resolution sketch (for a follow-on plan):

1. Bundle `src/runtime/*.c` and `src/runtime/*.h` into the tarball
   (e.g. `dist/share/turmeric/runtime/`) and the Homebrew formula
   (`(share/"turmeric").install "src/runtime" => "runtime"`).
2. In `src/main.c`'s autolink parser, rewrite paths like
   `src/runtime/hamt.c` and flags like `-Isrc/runtime` to point at
   the resolved install root, in the same spirit as
   `resolve_stdlib_root()`. A new `resolve_turmeric_root()` (or
   extend the existing SN1 logic) returns the parent of `stdlib/`
   and `runtime/`, and the autolink output is path-rewritten through
   it before being handed to `cc`.
3. The `Formula/turmeric.rb` test block can then switch back to
   `tur run` and exercise the full codegen path.

## Out of scope

- **Windows binaries.** Tracked separately in
  `docs/upcoming/windows-support-plan.md`. The build doesn't support
  Windows yet; adding a release matrix entry is meaningless until the
  source builds.
- **Code signing / notarization for macOS.** A signed-and-notarized
  binary requires an Apple Developer ID, a signing cert in CI secrets,
  and a notary workflow. Users today can `xattr -d com.apple.quarantine
  ./tur` after download; that is acceptable for a pre-1.0 CLI. Revisit
  once there is demand or a 1.0 target.
- **macOS x86_64 / universal2.** See "Discovered during execution:
  universal2 won't work" above. Intel-Mac users use
  `brew install --HEAD turmeric` (source build) until per-arch
  asm-source selection lands in CMake.
- **Stable Homebrew URL.** `Formula/turmeric.rb` is currently HEAD-only
  (no `url`/`sha256`, just `head`). Once the first tagged release
  actually publishes, a follow-on PR can add a stable `url`/`sha256`
  stanza derived from the release's `sha256sums.txt`. Source-build via
  `head` can stay as a fallback.
- **`curl | sh` installer.** Depends on this plan landing.
- **Runtime-source bundling.** See "Discovered during execution"
  above -- a follow-on plan, not blocking the first publishable
  release.

---

## Acceptance criteria

- [ ] Pushing a `v*` tag produces a GitHub Release within ~10 minutes.
- [ ] Release contains tarballs for `linux-x86_64`, `linux-aarch64`,
      and `macos-arm64`, plus `sha256sums.txt`.
- [ ] Each tarball extracts to `tur` + `stdlib/` + `include/` as
      siblings, and `./tur --version` exits 0.
- [ ] From the extracted tarball, `./tur --interpret` of a file
      using a stdlib-only form (e.g. `when`) succeeds, proving the
      sibling-stdlib resolver works.
- [ ] `brew install --HEAD turmeric` installs `tur` to
      `<prefix>/bin/` and `stdlib/` to `<prefix>/share/turmeric/`,
      and `brew test turmeric` passes.
- [ ] No matrix leg exceeds 10 minutes wall-clock.
- [ ] No Node-20-deprecation warnings on the run page.
