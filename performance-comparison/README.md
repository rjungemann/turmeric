# Performance Comparison: C, Turmeric, turi, `tur jit`, Rust, Haskell, Clojure, Racket, Python

A cross-language benchmark suite: 21 benchmarks across 9 categories
(numerical, data structures, string processing, concurrency, memory,
recursion, I/O, real-world algorithms, micro), run against as many of the
nine columns above as the local toolchain set permits.

Not every column exists in every category. C and turi/turjit are Turmeric's
own comparison points; C sources exist only for `io` and `real_world`
(everything else can be read straight off the assembly a competent C
compiler would produce, so those two categories are where a hand-written C
baseline earns its keep). Clojure requires a JVM (`java`); Rust requires
`cargo`; Haskell requires `ghc`; a machine missing one of those reports that
column as `absent`, loudly, rather than silently dropping it from the
report -- see "Reading a result" below.

## Language versions (as last measured)

| Language | Version | Implementation |
|---|---|---|
| C | Apple clang 21.0.0 | `clang -O3` |
| Turmeric | v0.42.2 | `tur build` -> native binary |
| turi | v0.42.2 | `tur --interpret` (same source as Turmeric) |
| `tur jit` | v0.42.2 | `tur jit` (in-process MIR JIT, same source) |
| Rust | rustc 1.96.0 | `cargo build --release` |
| Haskell | GHC 9.14.1 | plain `ghc -O2` (no cabal/Hackage needed) |
| Clojure | 1.12.5 | `clojure -M` (needs a JVM) |
| Racket | 9.2 | `racket` (Chez Scheme backend) |
| Python | 3.13.14 | CPython |

Versions are also captured per-row into every `results/raw/*.json` record
(`toolchain` field) and by `scripts/check_environment.sh` -- treat the table
above as a snapshot, the JSON as the record of what actually ran.

## Running it

```sh
# from the turmeric project root, build a Release compiler first:
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON
cmake --build build-release -j --target tur

cd performance-comparison
./scripts/check_environment.sh          # preflight: what will/won't run, before a multi-hour sweep
./scripts/run_all.sh                    # all categories, small inputs
./scripts/run_all.sh numerical          # one category
./scripts/run_all.sh numerical medium   # one category, larger input

python3 scripts/validate_correctness.py    # every implementation's stdout vs results/golden/
python3 scripts/aggregate_results.py --baseline rust   # trimmed-mean rankings, normalized to a column
python3 scripts/check_reproducibility.py   # flags any (benchmark, language) with CV > 10%
```

Rust and Haskell build automatically as part of `run_all.sh`'s build phase
(`cargo build --workspace --release` in `benchmarks/rust-workspace/`; plain
`ghc -O2` per file into `benchmarks/haskell-project/bin/` -- no cabal
install step, since every Haskell dependency is a GHC boot package).

`TUR` overrides the Turmeric binary path; without it, `run_all.sh` tries
`../build-rel/tur`, `../build-release/tur`, then `../build/tur` in that
order (a Debug build carries ASan, which is fine for `turi`/`turmeric`
correctness but not for reading its timing numbers). If your shell has
`TUR_STDLIB_DIR` set (mise's `turmeric` tool, or any other pinned-version
shim, may export it) and it points at a different Turmeric release, `tur`
refuses to build with a loud version-mismatch error rather than silently
miscompiling -- `unset TUR_STDLIB_DIR` before running the sweep if you hit
that.

## Reading a result

Every `(category, benchmark, language, size)` cell lands in
`results/raw/*.json` with a `status` of `ok`, `absent` (toolchain/source
missing), `failed` (ran, non-zero exit), or `timed_out`. A run that skipped
anything exits non-zero and prints how many cells were skipped -- an
unattended sweep can't produce a quietly-incomplete report.

`aggregate_results.py --baseline <lang>` (default `rust`) normalizes every
row to a chosen anchor language's time and records which one in the output
JSON (`baseline_language`), so a chart can never be read against the wrong
column by accident.

For the `turjit` column specifically: `tur jit --timing-json <path>` reports
`{compile_ms, run_ms, engine}` per invocation (`engine: "jit" |
"cc-fallback"`), embedded per row as `jit_timing`. `tur jit` compiles and
runs in one process, so raw wall-clock time is not comparable to an
already-built binary's run-only time -- see
[docs/methodology.md](docs/methodology.md) for the two-chart methodology
(steady-state throughput vs. time-to-first-result) this implies, and
[docs/guides/performance-guide.md](../docs/guides/performance-guide.md) for
where these results get folded into the published guide.

## Structure

```
performance-comparison/
├── benchmarks/
│   ├── numerical/{c,turmeric,turi,clojure,racket,python}/...
│   ├── data_structures/  string_processing/  concurrency/  memory/
│   ├── recursion/  io/  real_world/  micro/
│   ├── rust-workspace/       -- single cargo package, one src/bin/*.rs per benchmark
│   └── haskell-project/      -- one .hs per benchmark, .cabal kept for tooling
├── inputs/            -- per-benchmark small/medium/large/xlarge JSON parameters
├── results/
│   ├── golden/         -- pinned correct stdout per (category, benchmark, small)
│   ├── raw/            -- one JSON per (category, benchmark, language, size) run
│   └── processed/      -- aggregate_results.py / check_reproducibility.py output
├── scripts/            -- run_all.sh, aggregate_results.py, validate_correctness.py, ...
└── docs/methodology.md
```

## What this is not

This is a benchmark suite, not a research paper: trimmed mean over 10 runs
on one machine is a useful signal, not a claim of statistical significance.
Same-algorithm parity across columns (not idiomatic-per-language rewrites)
is the ground rule -- see `docs/methodology.md` for where a language's
natural idiom was deliberately not used, and why.
