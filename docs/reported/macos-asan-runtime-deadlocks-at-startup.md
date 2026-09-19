---
title: An outdated clang's ASan runtime deadlocks every Debug tur at startup on newer macOS
category: Reported
description: On a mismatched toolchain/OS pairing the Debug build spins forever in InitializeShadowMemory before main(), so every tur invocation hangs, `tur --version` included. Latent, not fixed -- the tree deliberately keeps sanitizers on and stays loud. Mechanism corrected 2026-09-18: the macOS ASan runtime is a dylib loaded from an absolute rpath at startup, not code linked into the binary, so a CLT update fixes already-built binaries and no rebuild is needed.
---

# An outdated clang's ASan runtime deadlocks every Debug `tur` at startup on newer macOS

**Severity: medium when live, and total while it lasts** -- every `tur`
invocation hangs, including `tur --version`, so the toolchain is unusable
rather than degraded. Currently **latent**: it does not reproduce on a matched
pairing (see "Does it reproduce today?").

**Status: OPEN as a standing environmental hazard, deliberately not fixed
in-tree.** Filed 2026-09-18 at the maintainer's request. It has been documented
in [CLAUDE.md](../../CLAUDE.md) since 2026-07-11 but never carried a report, so
there was nowhere to record whether it is still live -- which is the only
question anyone hitting it actually has.

This is a **toolchain/OS bug, not a defect in `tur`.** It is filed because it
presents as a total `tur` failure, and because the tree makes a deliberate
choice to let it stay loud rather than paper over it. That choice is the part
worth keeping written down.

## Symptom

Every `tur` invocation from a Debug build hangs, before `main()` runs and
before any argument is parsed:

```sh
$ ./build/tur --version     # hangs forever
```

The process spins in a startup spinlock inside ASan's
`InitializeShadowMemory` while walking the dyld shared cache. Nothing in the
`tur` source is on the stack, and no turmeric source change triggers it --
*rebuilding* with the outdated toolchain is what does.

## Repro

The finding does not need `tur` at all:

```sh
$ printf 'int main(void){return 0;}\n' > bare.c
$ cc -fsanitize=address -g -o bare-asan bare.c
$ perl -e 'alarm 15; exec @ARGV' ./bare-asan     # hangs when live
```

`perl -e 'alarm N; exec @ARGV'` rather than `timeout N` on purpose: macOS
runners ship no coreutils `timeout`, and this is the same portable form CI
uses.

## Root cause, corrected 2026-09-18

**This report's first version said the ASan runtime is "baked into the binary
by the compiler at link time." On macOS that is not how it works**, and the
difference changes both the trigger and the set of remedies. Measured:

```sh
$ otool -l build/tur | grep -A2 LC_RPATH
  path /Library/Developer/CommandLineTools/usr/lib/clang/21/lib/darwin

$ DYLD_PRINT_LIBRARIES=1 ./build/tur --version
  .../CommandLineTools/usr/lib/clang/21/lib/darwin/libclang_rt.asan_osx_dynamic.dylib
```

What is baked in is an **absolute, version-pinned path**. The runtime is a
dylib loaded out of the toolchain directory at every startup. Three
consequences the original text got wrong:

1. **A rebuild is not required to change the runtime.** Updating the Command
   Line Tools in place swaps the dylib under every binary already built. The
   original "triggered by *rebuilding* with the outdated toolchain" describes
   when the rpath gets re-pointed, not when the runtime changes. So if this is
   ever live, **updating the CLT is a remedy**, and it was not on the list.

2. **A major CLT bump breaks the path; it does not hang.** The rpath names
   `clang/21`. When CLT moves to 22 that directory is gone and an old binary
   fails to *load*, with a dyld error. That is loud and instant, and should not
   be filed as this.

3. **Runtimes are not interchangeable, by design.** The binary imports a guard
   symbol naming the compiler that built it. Forcing a foreign runtime under
   it is rejected rather than silently used:

   ```sh
   $ DYLD_LIBRARY_PATH=/opt/homebrew/Cellar/llvm/22.1.4/lib/clang/22/lib/darwin \
       ./build/tur --version
   dyld: Symbol not found: ___asan_version_mismatch_check_apple_clang_2100
   ```

   This matters for the diagnosis: "an old clang links an old runtime" cannot
   produce a *silent* mismatch. The pairing that deadlocks has to be a runtime
   the binary accepts -- its own toolchain's -- that the running dyld defeats.

The variable remains the **pairing** of toolchain to OS, not either alone.
That part of the original account survives.

## The recommended fix collides with a documented trap

Remedy 1 below (build with Homebrew LLVM) is the same route
[CLAUDE.md:245](../../CLAUDE.md) warns about from the other direction: a `tur`
built with Homebrew LLVM, against fixtures that link with Apple `cc`, fails
**every** fixture on `___asan_version_mismatch_check_v8`, which the harness
reports as `build failed` -- i.e. it reads exactly like a compiler regression.
The guard symbol above is that same mechanism, confirmed live on this host.

So the two pieces of macOS advice in CLAUDE.md interact, and neither section
says so. If you take remedy 1, either pin the fixture compiler to the same
toolchain (`CC=/opt/homebrew/opt/llvm/bin/clang bash tests/run.sh`) or build
unsanitized with Apple clang, which sidesteps both.

## Does it reproduce today?

**No, on the pairing this was checked against (2026-09-18):**

| | |
|---|---|
| macOS | 27.0 (build 26A5378n), Darwin 27.0.0 |
| Apple clang | 21.0.0 (clang-2100.3.25.1), arm64-apple-darwin27.0.0 |
| Homebrew clang | 22.1.4 |

- The bare `int main(void){}` repro above exits 0 well inside a 15s alarm.
- `./build/tur` is genuinely ASan-linked -- `otool -L` shows
  `@rpath/libclang_rt.asan_osx_dynamic.dylib` and `nm -u` lists 44 undefined
  `asan` symbols -- and it runs.

So a hang on this class of machine today is **not** this report; look
elsewhere before reaching for `-DTUR_DEBUG_SANITIZE=OFF`.

Both runtimes on this host work: Apple clang 21 and Homebrew clang 22.1.4 each
run the bare repro to exit 0. `build-release/tur` links no ASan at all, as
claimed.

**Why it is latent here, stated as a rule rather than a date.** The Command
Line Tools are `com.apple.pkg.CLTools_Executables` **27.0.0.0** shipping clang
21, on macOS 27.0 -- toolchain and OS from the same generation. The hazard is
an *OS-ahead-of-toolchain* window, and on this host that window is closed. So
the thing to compare when it recurs is **the CLT version against the OS
version**, not the Darwin version against a threshold.

## Provenance: one commit message

`InitializeShadowMemory` appears in exactly two commits in this repository's
history -- #660, which filed the CLAUDE.md text, and the one that filed this
report -- and nowhere else in `docs/`. There is no CI log, stack trace, or
prior report recording the original observation. The spinlock / dyld
shared-cache detail rests on a single author's note from 2026-07-11. It is the
best available account and is probably right, but it has never been
independently reproduced in anything this tree keeps, which is worth knowing
before building on it.

## Why there is no auto-disable, and why that is right

The first fix (#660, 2026-07-11) auto-defaulted `TUR_DEBUG_SANITIZE` OFF on
Darwin >= 27. A later commit in the same PR **deliberately reverted that**, on
the grounds that silently stripping ASan/UBSan from macOS CI the moment the
runner image upgraded would carve a platform out of testing with nobody
noticing. `CMakeLists.txt` now says so in a comment, and defaults ON
everywhere.

**The measurement above is direct evidence the revert was correct.** This host
is Darwin 27.0 -- exactly what the reverted heuristic keyed on -- and ASan
works fine on it. That heuristic would today disable sanitizer coverage on a
machine that does not need it, which is precisely the silent-loss failure the
revert was guarding against. Darwin version was the wrong variable; the clang
that linked the runtime is the right one.

The one auto-disable that survives is scoped to WIN32 + GNU
(`CMakeLists.txt:47`), and it is a different situation: MinGW GCC ships no
ASan/UBSan runtime at all, so `-fsanitize=address,undefined` fails at *link*
time and no Debug build can succeed. There the feature does not exist; here it
exists and misbehaves, so staying loud is the right call.

## The backstop

`.github/workflows/ci.yml:150` runs `tur --version` under
`perl -e 'alarm 10; exec @ARGV'` right after Build on both `ubuntu-latest` and
`macos-latest`, asserting the version banner. A genuine runner deadlock
surfaces as a non-zero exit rather than stalling the job to its timeout. That
step is what makes "stay loud" a safe policy rather than a hopeful one -- it is
armed and present as of this filing, and should not be removed without
replacing the coverage.

## Workarounds

Ordered cheapest first. The third is the one CLAUDE.md leads with; it is listed
last here because it is the one that costs you something downstream.

- **Update the Command Line Tools first** -- new since the mechanism was
  corrected, and it is the only remedy that fixes binaries you have already
  built, because the runtime is loaded from the toolchain at startup rather
  than linked in:

  ```sh
  softwareupdate --list        # look for a Command Line Tools update
  softwareupdate --install "Command Line Tools for Xcode-<version>"
  ```

  Nothing needs rebuilding afterwards. Verify with `DYLD_PRINT_LIBRARIES=1
  ./build/tur --version` that the dylib now loading is the new one.

- **Keeps sanitizer coverage, but read the trap above** -- build with a current
  LLVM whose ASan runtime understands the current dyld cache. This is the one
  that makes every fixture fail on `___asan_version_mismatch_check_v8` unless
  you pin the fixture compiler to the same toolchain:

  ```sh
  brew install llvm
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang"
  ```

- **Drops leak/UB detection** -- explicit opt-out, stripping
  `-fsanitize=address,undefined` from the whole Debug build:

  ```sh
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTUR_DEBUG_SANITIZE=OFF
  ```

A Release build never carries the sanitizers (verified: `otool -L
build-release/tur` matches no ASan dylib), so `tur --version` on one always
works regardless -- which is the quickest way to tell this apart from a hang in
`tur` itself.

## What would close this report

Nothing in this tree can fix it; it closes when the hazard stops existing --
i.e. when no clang a contributor plausibly has installed still links a
pre-current-dyld ASan runtime. Until then the honest status is "latent,
documented, backstopped". Re-run the bare repro before claiming it is live.

## Guide upkeep

CLAUDE.md's "macOS startup hang -- outdated ASan runtime" section is the
user-facing copy of this. If this report is ever resolved, that section is
deletable in one piece, along with the `TUR_DEBUG_SANITIZE` rationale comment
at `CMakeLists.txt:25-32`. Do not let the two drift: the workarounds above are
duplicated from it deliberately, so a reader who lands on either one has the
whole answer.
