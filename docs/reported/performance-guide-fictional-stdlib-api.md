# performance-guide.md's middle sections document modules and functions that do not exist

**Severity: medium** -- the guide's data-structures, strings, concurrency,
memory, recursion, IO, and benchmarking sections cannot be followed. Found in
the 2026-08-20 docs audit.

## Repro

All verified absent from the tree: `stdlib/rand.tur`, `stdlib/regex.tur`,
`stdlib/trampoline.tur`, `stdlib/concurrency.tur`, `stdlib/dynamic-vars.tur`
(real file: `dynvar.tur`); `vec/make`/`vec/sort!`/`vec/fill!` (real:
`vec-new`/`vec-push!`/...); `hamt/insert`/`hamt/get-or` (real: `hamt/set`/
`hamt/get`); `str/concat`/`str/builder`/`str/view`/`str/format`; `io/open`/
`io/read-all`; `time/now-ns`; `args/parse-int`/`args/get`;
`scripts/run_all.sh`/`analyze_results.py`/`check_environment.sh` (`scripts/`
holds only `wait-for-release.sh`).

## Fix direction

Rewrite the middle sections against the real stdlib (`vec-*`, `hamt/*`,
`string.tur` builders, `thread`/`chan`, `benchmarks/run-benchmarks.sh`) or cut
them and link the per-module guides. The build-flags, self-tail-call, and
engine-triangle sections are accurate and should stay.

## Guides to update when fixed

- docs/guides/performance-guide.md
