# Setting up the runtimes for the benchmark sweep

Setup memo for [post-jit-benchmark-resurrection-plan.md](../upcoming/post-jit-benchmark-resurrection-plan.md)
B5 -- "a full sweep on a dedicated machine with all toolchains."  This is what
"all toolchains" actually means, how to get each one (via `mise` where that
works), and the four things that are broken or missing on a machine that looks
set up.

Everything below was verified on the author's box (Apple M2, macOS 27.0,
16 GB) on 2026-08-19.  Where a claim is "this is currently broken," it was
reproduced, not inferred.

## 0. TL;DR -- what is missing right now

| Column | Toolchain | Source of truth | Status here |
|---|---|---|---|
| `c` | `cc` / `clang -O3` | Xcode CLT | present (Apple clang 21.0.0) |
| `turmeric` | `tur build` | this repo | **must build `build-rel/`** |
| `turi` | `tur --interpret` | same binary | same |
| `turjit` | `tur jit` | same binary, `-DTUR_JIT=ON` | same |
| `rust` | `cargo build --workspace --release` | `benchmarks/rust-workspace/` | **sources gitignored -- absent (§5)** |
| `haskell` | `ghc -O2` | `benchmarks/haskell-project/` | present (GHC 9.14.1 via ghcup) |
| `racket` | `racket <f>.rkt` | in tree | present (9.2 via mise) |
| `python` | `python3 <f>.py` | in tree | present (3.13.14 via mise) |
| `clojure` | `clojure -M <f>.clj` | in tree | CLI present, **no JVM (§2.4)** |

Four blockers, in descending order of cost:

1. **`rust-workspace/` is not in the repo** -- `.gitignore:132` ignores the
   whole directory, so B1's 21 Rust binaries were never committed.  §5.
2. **`cargo` is not on `PATH`** even though mise reports `rust 1.96.0`
   installed.  A one-line config fix.  §2.3.
3. **No JVM**, so every `clojure` row fails.  §2.4.
4. **No `build-rel/tur`**, so every Turmeric/turi/JIT row silently SKIPs.  §3.

`racket`, `python3`, `ghc`, and `cc` need nothing.

## 1. Scope

`performance-comparison/` is 21 benchmarks across 9 categories, times up to 9
language columns.  Only six of those columns are in the headline chart (`tur`,
`turi`, `turjit`, `rust`, `haskell`, `racket`); `clojure`, `python`, and `c`
stay in the tree and run when their toolchain is present.  You can produce a
publishable Chart A/Chart B with no JVM at all -- the Clojure rows just render
as `--`.

Nothing in `performance-comparison/` needs a package registry.  No `deps.edn`,
no Hackage round-trip, no pip requirements: the Racket sources are all
`#lang racket`, the Python sources import only `sys`/`threading`/`queue`, the
Clojure sources have no `require`, and the Haskell sources' only dependencies
(`bytestring`, `containers`, `directory`) are GHC boot packages present in
9.14.1's global db.  This is deliberate -- it makes the sweep runnable on an
offline or proxy-restricted machine.  Only Rust wants the network, and only
if the workspace ends up with third-party crates.

## 2. The mise-managed half

### 2.1 What mise covers well

`python`, `racket`, `rust`, and `java` all have first-class mise support and
should be pinned there.  A project-local pin is better than relying on the
global `~/.config/mise/config.toml`, because the sweep's whole point is that
the versions are reproducible.  Drop this at
`performance-comparison/mise.toml`:

```toml
[tools]
python = "3.13"
racket = "9.2"
rust   = "1.96.0"
java   = "temurin-25"   # only needed for the clojure column

[env]
# mise's rust (rustup) backend puts ~/.cargo/bin on PATH, which does not
# exist once CARGO_HOME is redirected.  See section 2.3.
_.path = ["{{env.HOME}}/.local/share/mise/cargo/bin"]
```

Then `mise trust performance-comparison && mise install`.

### 2.2 Versions the harness expects

`scripts/check_environment.sh` hardcodes expected prefixes and prints `[WARN]`
on a mismatch.  Current expectations vs. what a modern machine has:

| Tool | `check_environment.sh` expects | Reality here | Verdict |
|---|---|---|---|
| `clang` | `17` | `21.0.0` | spurious WARN -- see §6 |
| `python3` | `3.13` | `3.13.14` | ok |
| `clojure` | `1.12` | `1.12.5` | ok |
| `racket` | `9` | `9.2` | ok |

The clang expectation is stale and the WARN is cosmetic; `clang -O3` is the
only flag the C column uses.

### 2.3 Rust -- installed but not on `PATH`

This is the non-obvious one.  `mise ls` reports `rust 1.96.0 (symlink)` and
the toolchain is genuinely there, but `command -v cargo` finds nothing and
`mise which cargo` says "cargo is not a mise bin."  The preflight matrix
prints `NO cargo` and the Rust column vanishes.

The cause is an interaction between two things that are individually correct.
`~/.config/mise/config.toml` redirects rustup's state to keep the toolchain
self-contained:

```toml
[env]
CARGO_HOME  = "{{env.HOME}}/.local/share/mise/cargo"
RUSTUP_HOME = "{{env.HOME}}/.local/share/mise/rustup"
```

but mise's rust backend unconditionally prepends **`~/.cargo/bin`** to `PATH`
-- the default location, not `$CARGO_HOME/bin`.  `~/.cargo` does not exist,
so nothing resolves.  The real proxies are sitting at
`~/.local/share/mise/cargo/bin/`, and they work as soon as `RUSTUP_HOME` is
set (which mise does):

```console
$ RUSTUP_HOME=~/.local/share/mise/rustup ~/.local/share/mise/cargo/bin/cargo --version
cargo 1.96.0 (30a34c682 2026-05-25)
```

Note that running that proxy *without* `RUSTUP_HOME` gives a misleading
"rustup could not choose a version of cargo to run ... no default is
configured" -- the default **is** configured
(`rustup/settings.toml: default_toolchain = "1.96.0-aarch64-apple-darwin"`),
rustup was just reading an empty `~/.rustup`.  Do not chase that error; it is
a symptom of the same `PATH`/home split.

**Fix** (verified: prepends correctly and `cargo --version` resolves):

```toml
[env]
_.path = ["{{env.HOME}}/.local/share/mise/cargo/bin"]
```

in either the global config or the project-local `mise.toml` above.  The
alternative -- dropping the `CARGO_HOME` override so rustup installs to
`~/.cargo/bin` where mise already looks -- also works but gives up the
self-contained-toolchain property the override was added for.

### 2.4 Java, for the Clojure column

The Clojure CLI is installed (`1.12.5.1645`) but there is no JVM:

```console
$ java -version
The operation couldn't be completed. Unable to locate a Java Runtime.
```

`clojure -M <script>` therefore fails on every row.  mise has a core Java
backend:

```sh
mise use -g java@temurin-25    # or zulu-25 / temurin-26
```

This is genuinely optional -- Clojure is not a headline column.  Skip it and
accept nine `--` cells if you would rather not carry a JDK.

The Clojure CLI itself is also mise-installable
(`mise use -g clojure@1.12.5.1664`) if you want it pinned alongside
everything else rather than coming from Homebrew.

## 3. Turmeric -- build `build-rel/` with the JIT on

`run_all.sh` uses **one** binary for three columns: `tur build` for the AOT
`turmeric` row, `tur --interpret` for `turi`, and `tur jit` for `turjit`.  So
that binary must be Release **and** configured `-DTUR_JIT=ON`.  Neither
existing build tree in this repo qualifies:

| Tree | `CMAKE_BUILD_TYPE` | `TUR_JIT` | `TUR_DEBUG_SANITIZE` |
|---|---|---|---|
| `build/` | Debug | **OFF** | ON |
| `build-jit/` | Debug | ON | OFF |

`build/tur jit` fails outright with `tur: this build carries no JIT engine;
reconfigure with -DTUR_JIT=ON`.  `build-jit/tur` works but is a Debug build,
which is not what you want to publish a throughput number from.

Build the benchmark compiler:

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-rel -j
```

**Build all targets, not `--target tur`.**  The `tur` executable does not link
`libturi`, so a `--target tur` build produces no `libturi.a` and every
`tur build` in the sweep dies with `ld: library 'turi' not found`.  This is
the same trap CLAUDE.md documents for the fixture suite.

`build-rel` is the spelling `run_all.sh` looks for first; it also accepts
`build-release` and then plain `build`, and `TUR=<path>` overrides all three.
A Release build carries no sanitizers, so the `ASAN_OPTIONS=detect_leaks=0`
that `run_all.sh` exports for the `turi` rows is a no-op -- it only matters if
you point `TUR` at a Debug tree.

Verify the JIT column end to end -- it should print a timing record with
`engine: "jit"`, not `"cc-fallback"`:

```console
$ ./build-rel/tur jit /tmp/jt.tur --timing-json /tmp/jt.json && cat /tmp/jt.json
{"compile_ms": 201.302, "run_ms": 0.300, "engine": "jit"}
```

`tur jit` writes a handful of `unknown pragma` / `Unsupported compiler
detected` / `different macro redefinition of NULL` warnings to **stderr** on
macOS as c2mir walks the SDK headers.  They are noise, not a fallback signal;
`run_all.sh` captures stdout for correctness checking, so they do not
contaminate anything.  The thing to actually watch is the `engine` field --
`"cc-fallback"` means that row is measuring `tur build` with extra steps
(plan §3.3).

## 4. Haskell -- leave it on ghcup

GHC is the one runtime where mise is the worse answer.  `mise registry ghc`
resolves to `conda:ghc` first and `asdf:mise-plugins/mise-ghcup` second; the
conda backend needs conda, and `mise ls-remote` against the ghcup plugin timed
out here.  ghcup is the upstream-blessed installer, it is already installed,
and `~/.ghcup/bin` is already on `PATH`:

```console
$ ghc --version
The Glorious Glasgow Haskell Compilation System, version 9.14.1
```

On a fresh machine: `curl --proto '=https' --tlsv1.2 -sSf https://get-ghcup.haskell.org | sh`.

No `cabal` is needed.  `run_all.sh:684` builds with plain `ghc -O2` straight
out of `src/*.hs` into `bin/`, deliberately, because every dependency is a
boot package.  The `.cabal` file is retained for IDE tooling only.  Verified
against 9.14.1:

```console
$ ghc -O2 -outputdir /tmp/hstest -o /tmp/hstest/fibonacci src/Fibonacci.hs
[1 of 2] Compiling Main             ( src/Fibonacci.hs, /tmp/hstest/Main.o )
[2 of 2] Linking /tmp/hstest/fibonacci
```

One wrinkle to know about: the build loop derives each binary name by
snake-casing the module name, and special-cases `thread_ring` to build with
`-threaded`.  If you add a Haskell benchmark that spawns threads, it needs the
same treatment or it will measure a single-capability runtime.

## 5. The Rust workspace is not in the repo

The plan's status header says B1 landed -- "`benchmarks/rust-workspace/` --
21 binaries, one `[[bin]]` each, all validated against `results/golden/` at
`small`."  That directory does not exist in this clone, and it is not on any
branch:

```console
$ git ls-files | grep -c rust-workspace
0
$ grep -n rust-workspace .gitignore
132:performance-comparison/benchmarks/rust-workspace/
```

The ignore rule covers the **whole directory**, not just `target/`, so the 21
Rust sources were written, validated, and then never committed.  Contrast the
sibling rules a few lines up, which ignore build output but re-include
sources:

```
performance-comparison/benchmarks/*/turmeric/*
!performance-comparison/benchmarks/*/turmeric/*.tur
```

Installing a Rust toolchain does not fix this.  Before B5 can run, someone
has to either recover the workspace from the machine it was authored on, or
rewrite it.  Either way, `.gitignore:132` should be narrowed to
`performance-comparison/benchmarks/rust-workspace/target/` in the same change,
or the sources will fall out of the repo a second time.

This also has a knock-on: `aggregate_results.py --baseline` defaults to
`rust` (plan §4.1), so with no Rust column every `speedup_vs_baseline` is
undefined.  Pass `--baseline tur` for an interim sweep.

## 6. Verify before committing to a sweep

```sh
cd performance-comparison
bash scripts/check_environment.sh
```

This prints the runtime versions and then B0's preflight matrix -- what will
and will not run, per `(benchmark, language)` -- **before** a multi-hour sweep
starts.  Current output on this machine, trimmed:

```
  [WARN] clang                got=21.0.0  expected prefix=17
  [ok  ] python3              3.13.14
  [ok  ] clojure              1.12.5
  [ok  ] racket               9.2
  [WARN] turmeric (tur)       binary not found at .../performance-comparison/../../build-rel/tur
  [info] java (for clojure)   not found

CATEGORY           BENCHMARK         c   tur  turi  jit  rust haskell  clojure  racket  python
concurrency        thread_ring       -   -    y     y    -    -        y        y       y
...
toolchains: cc | NO cargo | ghc | racket | clojure | python3
```

Two of those lines are the script's own defects, not your environment:

- **The `tur` path is off by one.**  `check_environment.sh` looks for
  `$(pwd)/../../build-rel/tur` while `run_all.sh:30` looks for
  `$(pwd)/../build-rel/tur`.  From `performance-comparison/` those are
  `<repo>/../build-rel/tur` and `<repo>/build-rel/tur` respectively -- so
  check_environment can WARN about a missing binary that `run_all.sh` will
  find perfectly well.  Trust `run_all.sh`'s spelling; fix the other when
  convenient.
- **`check_environment.sh` is macOS-only** in its hardware block
  (`sysctl`/`sw_vers`), unlike `run_all.sh`, which got a Linux branch for
  peak-RSS in B3.  On Linux the CPU/RAM/OS lines degrade to `uname` output.

Then smoke the smallest thing that exercises every column:

```sh
./scripts/run_all.sh micro small
```

`run_all.sh` exits **non-zero** if any cell was absent, failed, or timed out,
and prints the count -- that is B0 working as designed, not a run that broke.
Read the `status` field in `results/raw/*.json` to see which.

## 7. Order of operations

1. `mise trust performance-comparison && mise install` -- python, racket,
   rust, (java).
2. Add the `_.path` line from §2.3; confirm `cargo --version` resolves.
3. `cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON && cmake --build build-rel -j`
   (all targets).
4. Confirm `ghc --version` -- ghcup, not mise.
5. **Resolve §5** -- without `rust-workspace/` there is no Rust column and no
   default baseline.
6. `bash scripts/check_environment.sh`; expect the two known-bogus lines above
   and nothing else.
7. `./scripts/run_all.sh micro small` as a smoke, then the full sweep.

Steps 1-4 are ~20 minutes.  Step 5 is the whole cost of B5.
