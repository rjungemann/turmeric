# Benchmarking Methodology

## Environment
- **Hardware**: MacBook Pro (M3, 16GB RAM, 1TB SSD)
- **OS**: macOS 14.5
- **Compiler/Interpreter Versions**:
  - C: Apple clang 17.0.0
  - Turmeric: latest (local build, compiled to native via C)
  - turi: latest (same source as Turmeric; tree-walking interpreter mode)
  - Rust: 1.95.0 stable (cargo build --release)
  - Clojure: 1.12.5
  - Racket: 9.1
  - Python: 3.13.1

## Measurement Protocol
1. **Warm-up**: 3 iterations (discarded)
2. **Measurement**: 10 iterations
3. **Metrics**:
   - Time: `/usr/bin/time -v` (wall clock, CPU, memory)
   - Memory: Language-specific tools
   - Correctness: Validate output before benchmarking

## Test Harness Requirements
1. Identical input generation across languages
2. Structured output (JSON)
3. Automated validation
4. Consistent error handling

## Statistical Analysis
- Discard top/bottom 10% as outliers
- Report mean ± standard deviation
- Normalize against C baseline
## Additions (post-jit-benchmark-resurrection-plan, 2026-08-17)

### Languages

The original column set is kept verbatim; `rust`, `haskell`, and `turjit`
ride on top of it. Rust is a single cargo package with one `[[bin]]` per
benchmark (`cargo build --release`); Haskell builds each executable with
plain `ghc -O2` (every dependency is a GHC boot package, so no Hackage
round-trip -- the `.cabal` file exists for tooling).

### Same-algorithm notes

- **Haskell strictness:** accumulator loops use bang patterns / `foldl'`
  wherever the Turmeric column is strict -- the goal is comparable work,
  not a lazy-evaluation demonstration.
- **Haskell strings:** `string_processing` uses strict `ByteString`
  (byte-oriented like the Turmeric column), never lazy `String`.
- **`float_arith` output:** GHC's `printf`/`showFFloat` format doubles via
  the shortest decimal representation; the Haskell column formats through
  exact `Rational` expansion so `%.6f` matches C bit-for-bit.
- **`hash_map`:** Turmeric uses its HAMT; Rust uses `std::collections::
  HashMap`, Haskell `Data.IntMap.Strict` -- each language's analogous map
  structure, stated here rather than silently picking a faster shape.
- **`list_ops`:** a linked cons structure in every column (Rust: `Box`
  chain with iterative drop), never a growable array.

### The JIT column (`turjit`) -- two charts, not one

`tur jit` compiles and runs in one process, so a single wall-clock column
would compare different quantities. `tur jit --timing-json` reports
`{compile_ms, run_ms, engine}` per invocation and the harness embeds the
last run's record in each `turjit` result (`jit_timing`):

- **Chart A (steady-state):** run-only -- AOT columns as measured;
  `turjit` uses `run_ms` (compile subtracted). Report at medium/large.
  Note: under the default lazy generation mode, `run_ms` includes
  first-call code generation; set `TUR_JIT_GEN=eager` to move codegen
  into `compile_ms`.
- **Chart B (time to first result):** total wall clock, compile included,
  for every column. Report at small, where compile time is visible.
- `engine: "cc-fallback"` marks a run the JIT could not execute (it fell
  back to the cc path); such rows are reported, never averaged into the
  JIT column.
- Warm-up runs build no in-process JIT cache -- each invocation
  recompiles from scratch; "warm-up" here only stabilizes OS caches.

### Recorded environment

Each result JSON carries `status` (`ok`/`absent`/`failed`/`timeout`),
`toolchain` (the column's compiler/runtime version at run time), and
`platform`. Peak RSS is captured on macOS (`/usr/bin/time -l`) and Linux
(`/usr/bin/time -v`). `aggregate_results.py --baseline <lang>` (default
`rust`) emits `speedup_vs_baseline` with the anchor recorded per row;
`speedup_vs_c` remains wherever C exists. A run that skipped any cell
exits non-zero and says how many.
