# Post-JIT: resurrect the cross-language benchmark suite

> **Status: COMPLETE 2026-09-01.** B0-B5 all implemented and executed. A full small-size
> sweep ran on this machine (macOS, Apple M2): rust/haskell/racket/python/
> turmeric/turi/turjit all green (21/21 each but turi at 20/21, see below); C
> absent outside io/real_world by design; Clojure absent for lack of a JVM on
> this machine (a toolchain gap, not a suite defect -- reported loudly by
> check_environment.sh/run_all.sh's SKIP accounting, not silently). Proposed
> 2026-07-29.
> **Type:** Benchmarks / `performance-comparison/`
>
> **Correction (2026-09-01):** commit `2a7abfc25` (2026-08-17) claimed B1
> (`benchmarks/rust-workspace/`, 21 binaries validated against golden) as
> done, but its actual diff contained zero Rust files -- no `Cargo.toml`,
> no `src/`, nothing, tracked or untracked, anywhere in the tree. The claim
> was never true; B1 sat undone for two weeks under a banner that said
> otherwise. B2/B4/B0/B3 below were spot-checked against this session's
> rebuild and are real. B1 is now actually implemented and validated (see
> below) as of this date.
>
> **Implementation notes:** per the maintainer's direction the current
> language list is kept verbatim and the new columns ride on top (`rust`,
> `haskell`, `turjit`).
> - **B1 (2026-09-01, this session):** `benchmarks/rust-workspace/` -- a
>   single cargo package, 21 binaries auto-discovered from `src/bin/*.rs`
>   (no external crates; std covers HashMap/threads/files), all 21
>   validated byte-for-byte against `results/golden/` at `small`. Same
>   ground rules as Haskell: `list_ops` is a real `Box` cons chain (walked
>   iteratively so drop doesn't recurse), `hash_map` uses
>   `std::collections::HashMap`, `thread_ring` uses real `std::thread` +
>   `mpsc` channels mirroring the pthread mutex/cond ring, all LCG state
>   uses `wrapping_*` ops to match the C/Turmeric int64 wraparound. Rust's
>   `{:.6}` fixed-precision float formatting is an exact decimal expansion
>   like C's printf (unlike its shortest-repr `{}` `Display`), so
>   `float_arith` needed no Haskell-style Rational workaround.
> - **B2:** `benchmarks/haskell-project/` -- 21 executables. `cabal` is not
>   installed on this machine; built instead with plain `ghc -O2` per file
>   via `scripts/build_haskell.sh` into `benchmarks/haskell-project/bin/`
>   (all deps are GHC boot packages, so this needed no Hackage round-trip,
>   matching the plan's "works proxy-restricted" claim). Strictness +
>   ByteString decisions are in docs/methodology.md; `float_arith` needed
>   an exact-Rational `%.6f` because GHC formats doubles via shortest-repr.
>   All 21 spot-checked against golden this session.
> - **B4:** `tur jit --timing-json <path>` emits `{compile_ms, run_ms,
>   engine}` (engine: `"jit" | "cc-fallback"`); the harness embeds it per
>   turjit row and the two-chart methodology is written. Verified for real
>   this session (`src/jit_engine.c`/`src/main.c`, not just referenced in
>   the shell script).
> - **B0/B3:** preflight matrix in check_environment.sh; absent/failed/
>   timeout become recorded statuses and a non-zero exit; Linux peak-RSS;
>   per-row toolchain+platform capture; `aggregate_results.py --baseline`
>   (default rust) with the anchor recorded per row; the TUR path accepts
>   build-rel, build-release, and build. Confirmed wired into
>   `scripts/run_all.sh` for real this session.
> - One golden was stale: `micro_float_arith_small` matched NO current
>   column (old sequential-update formula); regenerated from the current
>   three-way-agreeing implementations.
> - Found while smoking: the run_timed JSON writer never worked on Linux at
>   all (empty RSS interpolated as a bare token) -- fixed via json.loads.
>
> **B5 execution (2026-09-01, this session):** full sweep run on macOS/Apple
> M2 against a Release+JIT `tur` build (`-DCMAKE_BUILD_TYPE=Release
> -DTUR_JIT=ON` -- the plain Release build from B1 had no JIT compiled in at
> all, so `turjit` was 0/21 until this was caught). Result:
> `rust/haskell/racket/python/turmeric/turjit` all 21/21; `turi` initially
> 20/21 (one `int->float`-under-`--interpret` gap) then, after the
> `bit-shr`/`int->float` compiler fixes below landed the same day, 21/21;
> `c` 5/21 by design (only `io`/`real_world` have C
> sources); `clojure` 0/21 for lack of a JVM on this machine (an
> environment gap, not a suite defect). `aggregate_results.py --baseline
> rust`, `validate_correctness.py` (21/21 against golden for the languages
> it checks), and `check_reproducibility.py` all ran clean over the final
> results (0 entries flagged CV > 10% on the post-fix re-run).
> `docs/guides/performance-guide.md` and this repo's
> `performance-comparison/README.md` are updated to match.
>
> Five real bugs surfaced and were fixed along the way, none previously
> caught because these code paths had apparently never been exercised
> end-to-end before this session -- full writeup in
> `docs/archive/turi-vec-new-filled-native-override-lost.md`:
> - `thread_ring.tur`'s inline-C referenced a sibling `defn` by a
>   hand-guessed mangled name (`ring_worker`); fixed to use the documented
>   `__TUR_CNAME_<name>__` splice (docs/guides/c-integration-guide.md).
> - `src/turi/preload.c`'s `vec-new-filled` stub was typed `:int` (a lazy
>   stand-in for a real `(Vec A)`) and, once retyped, was found to lose its
>   native override entirely because `turi_register_collection_natives` is
>   only ever registered once, at env-creation time, unlike the
>   early+late-registered `turi_env_register_interpreter_natives` pattern
>   every other benchmark-only native relies on. Fixed in `main.c`/`repl.c`/
>   `macro_env.c`.
> - `bit-shr` (`src/compiler/builtins.c`) is a compiler builtin lowering to a
>   bare C `>>` on signed `int64_t` -- an arithmetic, sign-extending shift,
>   backwards from its documented "logical (unsigned) right shift" contract.
>   **Not interpreter-only**: reproduced identically in compiled, `tur jit`,
>   and `--interpret` output. Fixed in all three shared emission sites
>   (`emit_core.c`, `emit_cps_ir.c`, `eval.c`) by casting to `uint64_t`
>   before the shift specifically when `c_op == ">>"` (the only builtin using
>   that operator string).
> - `int->float` had no compiler-builtin entry and no preload stub at all
>   under `--interpret` (its native, `native_int_to_float`, was correctly
>   implemented and registered but unreachable with no declared signature);
>   fixed by adding the missing stub to `turi_env_preload_native_stubs`.
> - `check_environment.sh` resolved the `tur` binary at a hardcoded (and
>   wrong -- one `..` too many) `../../build-rel/tur`, independent of the
>   build-rel/build-release/build fallback chain `run_all.sh` already had;
>   now shares that same fallback.
> - `scripts/check_reproducibility.py` crashed formatting a `None` mean/stdev
>   for any (benchmark, language) group where every raw record was an
>   absent/failed status JSON; fixed to print `n/a` for those cells instead.

## 0. Summary

`performance-comparison/` is a mostly-complete benchmark harness that
currently cannot produce the report it was built for. The target set for the
revival is six implementations:

| Column | Command | Status |
|---|---|---|
| `tur` | `tur build` -> native binary | sources exist |
| `turi` | `tur --interpret file.tur` | sources exist |
| `tur jit` | `tur jit file.tur` | **needs J1** |
| Rust | `cargo build --release` | **sources missing entirely** |
| Haskell | GHC `-O2` | **does not exist** |
| Racket | `racket file.rkt` | sources exist |

Clojure, Python, and C sources also exist in part and stay in the tree -- they
run when their toolchain is present and are simply absent from the headline
chart otherwise.

The work splits into three unequal parts: **write the missing Rust and Haskell
implementations** (the bulk), **add a JIT column with an honest methodology**
(the interesting part, §3), and **fix the harness defects that let a missing
language disappear silently** (small, but it is why the suite looks finished
and is not).

---

## 1. What is actually there

### 1.1 Present and working

- `scripts/run_all.sh` (589 lines) -- warm-up + 10 timed runs, per-invocation
  timeout, correctness capture, JSON per `(category, benchmark, language,
  size)` into `results/raw/`.
- `scripts/aggregate_results.py` -- trimmed mean (drop top/bottom 10%),
  normalization, rankings.
- `scripts/analyze_results.py`, `generate_analysis.py`,
  `visualize_results.py`, `validate_correctness.py`,
  `check_reproducibility.py`, `check_environment.sh`.
- `inputs/` per category, `results/golden/` with ~15 pinned outputs,
  `docs/methodology.md`.
- Nine categories: numerical, data_structures, string_processing,
  concurrency, memory, recursion, io, real_world, micro.

### 1.2 Present per category

```
concurrency      clojure python racket turi turmeric
data_structures  clojure python racket turi turmeric
io               c clojure python racket turi turmeric
memory           clojure python racket turi turmeric
micro            clojure python racket turi turmeric
numerical        clojure python racket turi turmeric
real_world       c clojure python racket turi turmeric
recursion        clojure python racket turi turmeric
string_processing clojure python racket turi turmeric
```

### 1.3 The gaps, stated exactly

- **No Rust at all.** `run_all.sh:26` points `RUST_RELEASE_DIR` at
  `benchmarks/rust-workspace/target/release`, and it invokes a Rust binary for
  every benchmark (`run_all.sh:190`, `:203`, `:216`, ...). The directory does
  not exist. `aggregate_results.py:LANGUAGES` lists `rust`. The harness has
  been written *as if* Rust were there since it was authored.
- **C exists in only two of nine categories,** yet
  `aggregate_results.py` normalizes everything to C (`speedup_vs_c`, C = 1.0).
  Seven categories therefore have no baseline to normalize against.
- **No Haskell** anywhere.
- **Missing implementations vanish silently.** `run_timed` prints
  `SKIP: command failed or timed out` and returns (`run_all.sh:89-92`), so a
  nonexistent binary produces no JSON, no error, and no mention in the report.
  A result table with four columns where seven were intended looks like a
  finished run.
- **`peak_rss_kb` is macOS-only.** It parses `/usr/bin/time -l`
  (`run_all.sh:74`); Linux `time` needs `-v` and a different field. On Linux
  every RSS number is null.
- **The `tur` path defaults to `../build-rel/tur`** (`run_all.sh:25`), which
  matches the Justfile's `release` recipe (`Justfile:422`) but *not* CLAUDE.md's
  documented bootstrap (`build-release`, CLAUDE.md:381). Someone following the
  docs gets a silent SKIP on every Turmeric row. Reconcile the two spellings.

---

## 2. Writing the missing implementations

### 2.1 Rust -- `benchmarks/rust-workspace/`

A single cargo workspace with one `[[bin]]` per benchmark, matching the layout
`run_all.sh` already assumes (`target/release/<benchmark>`). One workspace
rather than one crate per benchmark: `cargo build --workspace --release` is a
single command, and the README already documents it that way.

Ground rules, so the comparison means something:

- Same algorithm as the Turmeric source, not an idiomatic rewrite. If the
  Turmeric version walks a cons list, the Rust version walks a linked
  structure, not a `Vec`. Where the natural Rust differs materially, note it
  in a comment and in `docs/methodology.md` rather than quietly picking the
  faster one.
- Same inputs, read from `inputs/`, not regenerated.
- Same stdout, byte for byte, so `validate_correctness.py` can check it.

### 2.2 Haskell -- `benchmarks/haskell-project/`

A cabal project with one executable per benchmark, built `-O2`, producing
`dist-newstyle/.../<benchmark>`. Add a `HASKELL_RELEASE_DIR` to `run_all.sh`
mirroring `RUST_RELEASE_DIR`, and a `cabal build all` step to the build phase.

Haskell needs two decisions written down before any source is:

- **Strictness.** Idiomatic lazy Haskell in an accumulator loop builds thunks
  and measures the wrong thing. Use `foldl'`/bang patterns where the Turmeric
  version is strict, and say so in the methodology -- the goal is comparable
  work, not a lazy-evaluation demonstration.
- **String type.** `String` is a lazy list of `Char` and is not comparable to
  a `cstr`/`String` benchmark in any other column. `string_processing` uses
  `Data.Text` (or `ByteString` where the Turmeric version is byte-oriented),
  and this is stated, not silent.

### 2.3 Restore the C baseline (or replace it) -- §4.1

---

## 3. The JIT column, and how to measure it honestly

This is where the suite earns its keep, and where it is easiest to publish a
misleading number.

`tur jit` compiles *and then* runs, in one process. `tur build` compiles ahead
of time and the benchmark measures only the run. So a single "wall clock"
column silently compares different quantities: `tur jit` pays a compile the
`rust`/`tur`/`haskell` columns already paid off-camera, and `turi` pays a
parse+elaborate the same way.

**Two charts, not one.**

### 3.1 Chart A -- steady-state throughput

Run-only time, for the work the benchmark actually does. Every AOT column
(`tur`, Rust, Haskell, C) is measured on a prebuilt binary as today. `tur jit`
is measured with its compile subtracted -- which means the JIT needs to report
it:

> **Harness requirement on J1:** `tur jit --timing-json <path>` (or an
> equivalent on stderr) emitting `{compile_ms, run_ms}`. The JIT knows both
> numbers exactly; recovering them from the outside is guesswork. Fold this
> into the J1 CLI surface rather than bolting it on later.

`turi` has no compile phase to subtract and appears as-is, with a footnote.

### 3.2 Chart B -- time to first result

Total wall clock from invocation to output, compile included, for *every*
column including `rustc`/`ghc`/`cc`. This is the chart where a JIT is supposed
to win, and it is the one a user comparing "edit-run loop" latency cares
about. Excluding compile time from Rust and Haskell here would be exactly the
mirror of the mistake Chart A avoids.

Chart B needs the small-input sizes to be informative -- at `large`, run time
swamps compile time and the two charts converge. Report Chart B at `small`,
Chart A at `medium`/`large`.

### 3.3 Fixtures the JIT cannot run

`jit-engine-plan.md` §3.2 wires a fallback to `cc` for inline-C that `c2mir`
cannot compile. A benchmark that silently falls back is measuring `tur build`
with extra steps. The harness must **detect and report the fallback**, not
average it in -- another reason the `--timing-json` output should carry an
`engine: "jit" | "cc-fallback"` field.

### 3.4 Warm-up interacts badly with a JIT

`run_all.sh` does 3 warm-up runs then 10 timed ones (`WARMUP_RUNS`,
`MEASURE_RUNS`). For a process-per-run JIT there is no warm cache to build --
each run recompiles from scratch. That is correct and should stay, but the
methodology has to say it, because "warm-up" in a JIT context usually means
something else entirely (in-process tiering), and MIR as configured here does
none of that.

---

## 4. Harness fixes

### 4.1 Baseline

With C present in only two categories, `speedup_vs_c` is not computable for
the other seven. Make the baseline a parameter --
`aggregate_results.py --baseline <lang>` -- defaulting to **`rust`**: it is in
the requested set, it is a native-compiled anchor comparable to C, and it will
exist in every category once §2.1 lands. Emit `speedup_vs_baseline` and record
which language it was, in the JSON, so a chart can never be read against the
wrong anchor. C rows still normalize fine wherever C exists.

### 4.2 Missing implementations become loud

- `check_environment.sh` grows a per-language toolchain probe (`cargo`, `ghc`,
  `racket`, `clojure`, `python3`, `cc`) and a per-benchmark source/binary
  existence check, printing a matrix of what will and will not run **before**
  a two-hour sweep starts.
- `run_timed` distinguishes *absent* (no source, no binary) from *failed* (ran,
  non-zero) from *timed out*, writes a JSON record with a `status` field for
  each, and the report renders them as explicit `--` cells rather than
  omitting the column.
- A run that skipped anything exits non-zero and says how many, so an
  unattended sweep cannot look clean.

### 4.3 Linux support

`peak_rss_kb` gets a platform branch: `/usr/bin/time -l` (macOS, `maximum
resident set size` in bytes) vs `/usr/bin/time -v` (GNU, `Maximum resident set
size` in KB). `docs/methodology.md` currently pins one machine (MacBook Pro
M3, macOS 14.5); it becomes a *recorded environment* captured per run into the
results JSON, not a hardcoded claim in prose.

### 4.4 Toolchain versions are data

Every column's version (`rustc --version`, `ghc --version`, `racket
--version`, `tur --version`) is captured into each results file at run time.
The README's version table then reports what was measured rather than what was
true when someone last edited it.

---

## 5. Phases

- **B0 -- audit and preflight.** §4.2's environment matrix and the
  absent/failed/timeout distinction. Run it once and publish the honest
  picture of what the suite can produce *today*. Cheap, and it is the input to
  sizing everything else.
- **B1 -- Rust.** `rust-workspace/`, all nine categories, outputs validated
  against `results/golden/`. The single largest chunk of work.
- **B2 -- Haskell.** `haskell-project/`, same scope, with the strictness and
  string-type decisions written into `docs/methodology.md` first.
- **B3 -- baseline + Linux + versions.** §4.1, §4.3, §4.4. Independent of
  B1/B2; can land any time after B0.
- **B4 -- JIT column.** Requires `tur jit` at J1 plus the `--timing-json`
  surface from §3.1. Two-chart methodology into `docs/methodology.md`.
- **B5 -- publish.** Regenerate `results/golden/`, run
  `check_reproducibility.py`, and fold the results into
  `docs/guides/performance-guide.md`, which `jit-engine-plan.md` §5 (J3)
  already nominates as the home for the interpreter/JIT/`cc -O2` triangle.
  This plan's chart is the superset of that one.

B1 and B2 are independent and are the long poles; B3 is independent of both.
Only B4 is genuinely blocked on the JIT.

---

## 6. Scope boundaries

- **The top-level `benchmarks/` directory is a different thing** --
  `bench-*.tur`, `run-benchmarks.sh`, `benchmark-results.md` are Turmeric-only
  microbenchmarks (HAMT insert/lookup, clone overhead, poly specialization).
  They are not folded in here and are not renamed. If anything, `tur jit` gets
  added as a second column *there* too, which is a smaller separate change.
- **No new benchmark categories.** Nine is enough; the gap is columns, not
  rows.
- **No claim of statistical significance beyond what the methodology
  supports.** Trimmed mean over 10 runs on one machine is a useful signal and
  is not a paper. The published charts say so.
