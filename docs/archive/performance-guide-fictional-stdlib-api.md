# performance-guide.md's middle sections document modules and functions that do not exist

**RESOLVED 2026-08-20.** Every fictional reference is gone; each remaining
mention sits inside a note saying what used to be documented and why it
was wrong. 873 lines -> 583.

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


---

## Resolution

Every claim in the report was re-verified before acting on it -- all the named
modules and functions really are absent, and `scripts/` really does hold only
`wait-for-release.sh`.

Took the report's second option (cut and link the per-module guides) over the
first (rewrite against the real stdlib), for one reason it did not raise: the
fictional sections carry **performance claims**. Rewriting them onto real APIs
would have meant either running a full benchmark campaign or inventing numbers,
and invented performance data is worse than an obviously-broken example. So the
prose sections now say what the real API *is* and link the module guide, and
make no timing claims they cannot back.

### Sections rewritten

Data structures, String and text processing, Concurrency and parallelism,
Memory and allocation, Recursion and stack usage, I/O operations, and
Benchmarking methodology. Each opens with a short note naming what it used to
document, so a reader who remembers the old text knows it changed and why.

The benchmarking section was the most wrong and is now written against the real
harness: `benchmarks/run-benchmarks.sh`, the
`<name>.tur` / `<name>-baseline.c` / `<name>.time` layout, and
`benchmarks/benchmark-results.md`. The old template had the benchmark time
*itself* via `time/now-ns` and print `elapsed_ns=`; in reality the **runner**
measures wall time around the built executable (`measure_time`), so a benchmark
needs no clock and no argument parsing at all. Also records that reading args
means `*args*` or `args/parse`, per CLAUDE.md's ban on `g_tur_args` pokery.

### Two the report missed

The report scoped the damage to the "middle sections" and called the numerical
block accurate. It is not, in two places -- both found by grepping the whole
file for every name in the report's own list rather than trusting the section
boundaries:

- **Prime sieve** (inside the "accurate" numerical section) used `vec/make`,
  `vec/get`, `vec/set!` and `(import "stdlib/vec.tur")`.
- **Monte Carlo pi** loaded `stdlib/rand.tur` and called `rand/float`.

Both are now written on the real API and **were run**, not transcribed:

- the sieve prints `25`, the number of primes below 100;
- Monte Carlo lands in range (it is seeded from `time(NULL)`, so the guide says
  not to pin an exact value).

Writing them turned up three things worth having in the guide, which are now
called out there: `for` is the monadic comprehension and not a numeric-range
loop, so a counted loop is a `while` over a `^mut` binding; a `set!` target
must be `^mut` at its binding site; and `rand-float` returns an **int in
[0, 9999]**, not a float, while `int->float` lives in `stdlib/math.tur` and is
not auto-loaded.

The Performance checklist also still recommended `str/builder`; reworded.

### Left alone

Build flags, the arithmetic and Fibonacci benchmarks, Self-tail-call
optimization, Real-world algorithms (advisory prose on real forms), and the
engine triangle -- all accurate, as the report said.

## Verification

- `tools/check-guide-pairs.py` -- 6 pairs found, 6 ok, 0 failed (AST-equality
  between each `turmeric` block and its `sweet-exp` twin)
- Both new examples executed in **both** dialects, extracting the sweet-exp
  blocks straight out of the guide: sieve `25` / `25`, Monte Carlo
  `in range` / `in range`
- Full-file grep for every name in the report's list: no occurrence outside a
  correction note
