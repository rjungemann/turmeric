---
title: An outdated clang's ASan runtime deadlocks every Debug tur at startup on newer macOS
category: Reported
description: On a mismatched toolchain pairing (a clang whose bundled ASan runtime predates the running dyld shared-cache layout) the Debug build spins forever in InitializeShadowMemory before main(), so every tur invocation hangs, `tur --version` included. Latent, not fixed -- the tree deliberately keeps sanitizers on and stays loud.
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

## Root cause

The ASan runtime is **baked into the binary by the compiler at link time**, so
the binary carries whatever runtime its clang shipped. An old clang links a
runtime that predates the current dyld shared-cache layout, and that runtime's
shadow-memory setup walks a cache it does not understand. The variable is the
**pairing** of clang to OS, not either one alone.

That distinction is load-bearing, and the tree has already been bitten by
getting it wrong -- see "Why there is no auto-disable" below.

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

## Workarounds (both already in CLAUDE.md)

- **Keeps sanitizer coverage** -- build with a current LLVM whose ASan runtime
  understands the current dyld cache:

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

A Release build never carries the sanitizers, so `tur --version` on one always
works regardless -- useful for telling this apart from a `tur` hang.

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
