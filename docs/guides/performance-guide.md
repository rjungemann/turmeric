---
title: Performance Guide
category: Performance
description: Writing fast Turmeric programs -- numerical computation, data structures, string processing, concurrency, memory, recursion, I/O, and benchmarking methodology
---

# Turmeric Performance Guide

Turmeric compiles to optimised C99 (release builds use `-O2` by default). This
means its performance ceiling is close to hand-written C, but the patterns you
choose matter. This guide covers the major performance dimensions -- numerical
computation, data structures, string processing, concurrency, memory and GC,
recursion, and I/O -- and finishes with a methodology section for writing and
interpreting benchmarks.

---

## Build flags

Always benchmark release builds:

```sh
just release          # cmake --build build -j --config Release
```

The debug build (`just build`) inserts contract checks and disables
optimisations; its timing numbers are not meaningful for comparison.

---

## Numerical computation

### Integer and floating-point arithmetic

Arithmetic on `int` and `float` compiles to the corresponding C types.
There is no boxing overhead for scalars declared with concrete types:

```turmeric
(defn square [x] : int (* x x))
(defn hyp [a b] : float
  (sqrt (+ (* a a) (* b b))))
```

```sweet-exp
defn square [x] :int
  {x * x}
defn hyp [a b] :float
  sqrt((+ (* a a) (* b b)))
```

Avoid leaving numeric expressions untyped in hot loops -- the elaborator may
widen to a tagged value when it cannot infer a concrete numeric type.

### Fibonacci (iterative vs recursive)

Iterative is faster for large N because it avoids stack growth:

```turmeric
; iterative -- O(n) time, O(1) space.  The named-let `loop` call is a
; self-tail-call, so it is lowered to an iterative backedge (see
; "Self-tail-call optimization" below): the stack does not grow with n.
(defn fib-iter [n] : int
  (let loop [i n a 0 b 1]
    (if (= i 0)
      a
      (loop (- i 1) b (+ a b)))))

; recursive -- O(2^n) time, avoid for n > ~30
(defn fib-rec [n] : int
  (if (< n 2)
    n
    (+ (fib-rec (- n 1)) (fib-rec (- n 2)))))
```

```sweet-exp
; iterative -- O(n) time, O(1) space (self-tail-call -> loop; see below)
defn fib-iter [n] :int
  let loop [i n a 0 b 1]
    if ={i 0}
      a
      loop({i - 1} b {a + b})

; recursive -- O(2^n) time, avoid for n > ~30
defn fib-rec [n] :int
  if {n < 2}
    n
    {fib-rec({n - 1}) + fib-rec({n - 2})}
```

### Self-tail-call optimization

A **self-tail-call** -- a call to the enclosing function (or named-let `loop`
binding) in *tail position* -- is lowered to an iterative loop rather than a C
function call.  The compiler reassigns the parameter variables and jumps back to
the top of the function body, so iteration count no longer drives C-stack depth:
a self-recursive countdown of 10,000,000 iterations completes instead of
overflowing the stack.

The guarantee applies to:

- a self-recursive `defn` whose recursive call is in tail position, and
- the named-let / loop idiom `(let loop [...] ... (loop ...))`,

with tail position computed through `if`, `cond`/`when` (which macro-expand to
`if`), `do`, and `let`/`letrec`.  For example, both of these are lowered to a
loop:

```turmeric no-check
; self-recursive defn -- tail call in the `if` else-branch
(defn count-down [n :int acc :int] :int
  (if (= n 0)
    acc
    (count-down (- n 1) (+ acc 1))))

; named-let -- the (loop ...) call is the self-tail-call
(defn sum-to [n :int] :int
  (let loop [i   :int 0
             acc :int 0]
    (if (>= i n)
      acc
      (loop (+ i 1) (+ acc i)))))
```

A named let lowers one of two ways -- to a lifted closure when the loop body
reads an enclosing variable, and to a plain lifted function when it does not
-- and both are optimized.  In the closure case the backedge reassigns the
loop parameters and leaves the captured environment alone (it does not change
across a self-call).  Pinned by `tests/fixtures/tco-named-let-capture-deep`
and `tests/fixtures/tco-named-let-nocapture-deep` at 5,000,000 iterations
each.

**Boundary (1.0).** Only *self*-tail calls are optimized.  The following are
left as ordinary recursive calls -- correct, but not stack-optimized:

- **non-tail recursion** (e.g. `(+ n (sum-to (- n 1)))`, where work remains
  after the call returns) -- never eligible, by definition;
- **mutual / general tail calls** (function A tail-calls B which tail-calls A);
- **tail calls inside `match` arms**;
- self-recursive functions with pass-by-pointer struct, function-typed, or
  poly-fn parameters;
- a self-recursive function that genuinely uses a control operator
  (`perform`/`handle`/`shift`/`await`) -- it is CPS-lowered, and the loop
  runs on the delimited-control path rather than as a C backedge.

General/mutual tail-call elimination and trampolining are deferred to the
post-1.0 CPS pass.  See
[control-flow-completeness-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/control-flow-completeness-plan.md)
(Phase CF1) for the full scope.

### Prime sieve

The Sieve of Eratosthenes benefits from a `vec` (a growable array over a
`malloc`ed block) rather than a linked list.

> Corrected on 2026-08-20: this example previously used `vec/make`,
> `vec/get`, `vec/set!` and `(import "stdlib/vec.tur")`, none of which exist.
> The version below was run: it prints `25`, the number of primes below 100.

```turmeric
(load "stdlib/vec.tur")

(defn sieve-count [limit : int] : int
  (let [flags   (:: (vec-new) (Vec int))
        ^mut k  0
        ^mut i  2
        ^mut n  0]
    (while (<= k limit) (vec-push! flags 1) (set! k (+ k 1)))
    (vec-set! flags 0 0)
    (vec-set! flags 1 0)
    (while (<= (* i i) limit)
      (when (= (vec-get flags i) 1)
        (let [^mut j (* i i)]
          (while (<= j limit)
            (vec-set! flags j 0)
            (set! j (+ j i)))))
      (set! i (+ i 1)))
    (set! k 2)
    (while (<= k limit)
      (when (= (vec-get flags k) 1) (set! n (+ n 1)))
      (set! k (+ k 1)))
    n))
```

```sweet-exp
load("stdlib/vec.tur")

defn sieve-count [limit : int] : int
  let [flags   (:: (vec-new) (Vec int))
       ^mut k  0
       ^mut i  2
       ^mut n  0]
    while <=(k limit)
      vec-push!(flags 1)
      set!(k {k + 1})
    vec-set!(flags 0 0)
    vec-set!(flags 1 0)
    while <=(*(i i) limit)
      when =(vec-get(flags i) 1)
        let [^mut j (* i i)]
          while <=(j limit)
            vec-set!(flags j 0)
            set!(j {j + i})
      set!(i {i + 1})
    set!(k 2)
    while <=(k limit)
      when =(vec-get(flags k) 1)
        set!(n {n + 1})
      set!(k {k + 1})
    n
```

Note there is no numeric-range `for`: `for` is the monadic comprehension, so a
counted loop is a `while` over a `^mut` binding. A binding that `set!` writes
to must be declared `^mut` at its binding site.

### Monte Carlo pi estimation

Use the stdlib RNG rather than reaching into `<stdlib.h>` from inline-C, so the
compiler can see through the calls.

> Corrected on 2026-08-20: this example previously loaded `stdlib/rand.tur`
> and called `rand/float`. The module is **`stdlib/random.tur`** and the
> function is `rand-float`, which returns an **int in [0, 9999]** -- divide by
> 10000.0 for a float in [0, 1). `int->float` lives in `stdlib/math.tur` and is
> not auto-loaded. The version below was run and lands in range.

```turmeric
(load "stdlib/math.tur")
(load "stdlib/random.tur")

(defn estimate-pi [samples : int] : float
  (let [^mut i      samples
        ^mut inside 0]
    (while (> i 0)
      (let [x (/ (int->float (rand-float)) 10000.0)
            y (/ (int->float (rand-float)) 10000.0)]
        (when (<= (+ (* x x) (* y y)) 1.0) (set! inside (+ inside 1))))
      (set! i (- i 1)))
    (* 4.0 (/ (int->float inside) (int->float samples)))))
```

```sweet-exp
load("stdlib/math.tur")
load("stdlib/random.tur")

defn estimate-pi [samples : int] : float
  let [^mut i      samples
       ^mut inside 0]
    while >(i 0)
      let [x {int->float(rand-float()) / 10000.0}
           y {int->float(rand-float()) / 10000.0}]
        when <=({*(x x) + *(y y)} 1.0)
          set!(inside {inside + 1})
      set!(i {i - 1})
    {4.0 * {int->float(inside) / int->float(samples)}}
```

Note `rand-float` is seeded from `time(NULL)` on first use, so successive runs
differ -- do not pin an exact value in a test.

## Data structures

> The API names in this section were corrected on 2026-08-20. It previously
> documented `vec/make`, `vec/sort!`, `vec/fill!`, `hamt/insert` and
> `hamt/get-or`, none of which exist.

### Lists vs vecs

Use `list` / `cons` for functional transformations where sharing matters; use
`vec` for index-heavy access and mutation in place.

| Operation | `list` | `vec` |
|-----------|--------|-------|
| Prepend | O(1) | O(n) |
| Random read | O(n) | O(1) |
| Append (single) | O(n) | O(1) amortised |

The real `vec` surface is `vec-new`, `vec-push!`, `vec-get`, `vec-set!`,
`vec-len` (`stdlib/vec.tur`), plus the `[...]` literal, which lowers to
`vec-of` in expression position. See
[data-literals-guide.md](data-literals-guide.md).

### Hash maps

`stdlib/hamt.tur` (a persistent hash-array-mapped trie) is the standard map,
wrapped by `stdlib/map.tur`. The operations are `hamt/set` and `hamt/get` --
persistent, so each `set` returns a new map sharing structure with the old.
`#map{...}` literals construct one directly.

For a key that must outlive its source buffer, use `String` rather than a
computed `cstr`: `MapKey` for `String` copies and owns the key bytes, where a
`cstr` key can dangle. See [strings-guide.md](strings-guide.md).

### Sorting

No `vec/sort!` ships today. Sort by moving through a `list` or writing the
comparison loop directly over the `vec`; if you add a sort to `stdlib/vec.tur`,
this section should name it.

## String and text processing

> Corrected on 2026-08-20. This section previously documented `str/concat`,
> `str/builder`, `str/view`, `str/format` and `stdlib/regex.tur`. None of them
> exist.

Turmeric has three string-shaped types and the choice between them is the
performance decision that matters. `cstr` is a borrowed `const char *` with no
length; `str` (`stdlib/str.tur`) is a borrowed pointer+length view, so
substring-without-copy over a buffer you already own; `String`
(`stdlib/string.tur`) owns refcounted immutable bytes, so `Clone` is an O(1)
retain and structural sharing in a persistent map is free.

Repeated concatenation is the usual hot spot: each concat allocates and copies,
so building a string in a loop is quadratic. Prefer accumulating pieces and
joining once, and prefer a `str` view over copying a substring out.

There is no regex engine in the tree. `stdlib/cstr.tur` provides the byte
primitives (`cstr-len`, `cstr-nth`, `cstr-sub`, `cstr-eq?`) that scanning code
is written against.

See [strings-guide.md](strings-guide.md) for the full comparison.

## Concurrency and parallelism

> Corrected on 2026-08-20. This section previously documented
> `stdlib/concurrency.tur` and `stdlib/dynamic-vars.tur`; neither exists. The
> dynamic-variable module is `stdlib/dynvar.tur`.

Threads and channels live in `stdlib/concurrent.tur` (mutexes, rwlocks,
condvars, and the `mutex-guard-*` pair behind the `with-lock` /
`with-read-lock` / `with-write-lock` macros). `stdlib/schan.tur` provides
session-typed channels, where the protocol is a phantom threaded through each
operation, so a send that must precede a receive is checked at compile time.
`stdlib/stm.tur` and `stdlib/stm-sync.tur` cover transactional variables and
TMVar/TChan.

Thread-local and dynamically-scoped state is `stdlib/dynvar.tur`
(`defdynamic` / `let-dyn`).

The costs worth knowing: a thread is an OS thread, so creation is not free and
a work-queue over a fixed pool beats spawning per item; an uncontended
`with-lock` is cheap while a contended one parks; and STM retries the whole
transaction on conflict, so keeping transactions short matters more than
keeping them few.

See [threading-guide.md](threading-guide.md) and [stm-guide.md](stm-guide.md).

## Memory and allocation

> Corrected on 2026-08-20. The GC-pressure benchmark scripts this section
> referenced (`scripts/run_all.sh`, `analyze_results.py`) do not exist; see
> [Benchmarking methodology](#benchmarking-methodology) for the real harness.

Turmeric is refcounted, not tracing, so the cost model is retain/release
traffic and drop glue rather than collection pauses. Practical consequences:

- `Clone` on a `String` or an `Arc` is a refcount bump, not a copy -- sharing
  is cheap and does not need avoiding.
- A cycle is never reclaimed by refcounting alone. `tests/` carries a
  `tur_stdlib_no_rc_cycles` check for exactly this in the stdlib.
- `(gc-auto!)` exists and is strictly opt-in; it is not becoming the default.
- A by-value struct parameter is copied on bind, so a wide struct passed
  through a hot loop is worth passing by pointer or borrowing.

See [memory-management-guide.md](memory-management-guide.md).

## Recursion and stack usage

> Corrected on 2026-08-20. This section previously pointed at
> `stdlib/trampoline.tur`, which does not exist.

Self-tail-calls are optimised into a loop -- see
[Self-tail-call optimization](#self-tail-call-optimization) above, which is
measured and accurate. A tail-recursive accumulator therefore runs in constant
stack and is the idiomatic way to write a loop over a list.

Mutual recursion is **not** turned into a loop: `even?` calling `odd?` calling
`even?` grows the stack, so deep mutual recursion needs restructuring into a
single self-recursive function with an explicit state parameter, or into an
explicit worklist. There is no trampoline module to reach for.

Generators (`gen` / `yield`) and the CPS-lowered paths have their own cost
model; the tree-walking interpreter retains roughly 4 KiB per trampolined step,
which is a memory multiplier under `--interpret` and nothing at all compiled.

## I/O operations

> Corrected on 2026-08-20. This section previously documented `io/open` and
> `io/read-all`, neither of which exists.

`stdlib/io.tur` provides `read-file` (whole file into a malloc'd,
NUL-terminated buffer; NULL on error; **caller frees**) and `write-file`, plus
the lower-level file handle operations. `stdlib/fs.tur` covers path and
directory work.

The performance points that hold: one `read-file` beats a per-line read loop
for a file that fits in memory; writes should be batched rather than issued
per record; and the returned buffer is owned by the caller, so a read in a loop
that never frees is a leak, not merely garbage.

Asynchronous file and socket I/O is a separate surface -- see
[async-await-guide.md](async-await-guide.md).

## Real-world algorithms

### N-body simulation

Float-heavy simulations benefit from struct-of-arrays layout when possible.
Turmeric structs are currently records (array-of-structs), so prefer
separating coordinate vectors into dedicated `vec`s if profiling reveals cache
pressure:

```turmeric
(defstruct body [x :float y :float z :float
                 vx :float vy :float vz :float
                 mass :float])
```

```sweet-exp
defstruct body [x :float y :float z :float
                vx :float vy :float vz :float
                mass :float]
```

### Ray tracing

Ray-box intersection and dot products are the hot paths. Annotate return types
concretely (`float`) so the elaborator does not insert tag checks in the inner
loop:

```turmeric
(defn dot [ax ay az bx by bz] :float
  (+ (* ax bx) (+ (* ay by) (* az bz))))
```

```sweet-exp
defn dot [ax ay az bx by bz] :float
  {{ax * bx} + {{ay * by} + {az * bz}}}
```

---

## Execution engines: interpreter vs `tur jit` vs `cc -O2`

Turmeric has three execution engines, and which one is fastest depends on
what you are optimizing for -- startup latency or steady-state throughput:

| engine | invocation | compile step | best for |
|---|---|---|---|
| interpreter | `tur --interpret f.tur` | none | tiny scripts, REPL turns, debugging |
| MIR JIT | `tur jit f.tur` | in-process (c2mir) | run-edit-run loops, spice REPL reloads |
| cc | `tur build f.tur` + run | subprocess cc -O2 | long-running programs, deployment |

A project can select its default engine for `tur run` declaratively:
`:engine "cc" | "jit" | "interp"` in `build.tur`, overridden by `TUR_ENGINE`
in the environment, overridden by `--engine` on the command line (the same
ladder shape as `:build-dir`).  An unknown value is a hard error
(TUR-E0311), and a `"jit"` selection on a build without the engine, or
without the `jit` experiment enabled, fails loudly rather than silently
substituting -- the engines differ in SEMANTICS (`#?(:tur ... :turi ...)`,
inline-C carve-outs, c2mir divergences), not just speed, so pair a
load-bearing `:engine` with a `:tur-version` floor (older binaries silently
ignore unknown manifest keys and run under cc).

Measured triangle (x86-64 Linux, Release `tur`, best of 5, end-to-end wall
time; `bash benchmarks/run-triangle.sh` regenerates this from
`benchmarks/triangle/`):

| program | interpreter | tur jit | cc build | cc run | cc total |
|---|---|---|---|---|---|
| fib (fib 27, call-heavy) | 137ms | 127ms | 178ms | 2ms | 180ms |
| loop-sum (5M-iteration loop) | 1567ms | 137ms | 180ms | 3ms | 183ms |
| mandel (float inner loop) | 639ms | 142ms | 187ms | 5ms | 192ms |

All three legs are re-measured together on each run, so the columns are
comparable to each other. They are NOT comparable to a snapshot taken on a
different machine.

How to read it:

- **The front end dominates one-shot latency on every engine.** Roughly
  200ms of each cell is elaboration and codegen shared by all three legs;
  the engines differ in what happens after. For a program this small the
  interpreter's zero-compile leg makes it competitive end to end even
  while its loop throughput is 9x behind (loop-sum).
- **`tur jit` beats the cc round trip end to end**, by ~25-30% on these
  programs. Code generation is serialized and LAZY -- only the functions a
  run actually calls get compiled (worth 23-36% off the JIT leg alone
  versus eager generation). Its other advantage is structural, being IN PROCESS: no
  subprocess, no disk artifacts, and the spice REPL reload path is ~3.2x
  faster than the `tur build --shared` round trip it replaces (see the
  repl guide). `TUR_JIT_GEN=eager` restores whole-program generation, which
  is slower but compiles every function up front.
- **Compiled native runtime is 4-7ms** for these workloads -- for any
  long-running or repeatedly-invoked program, `tur build` once and run
  the binary; nothing else is close in steady state.
- The MIR tier generates good-but-not-gcc code: expect JIT'd loop bodies
  within ~1-2x of cc -O2, not parity, and note the JIT runs the program
  on a sized entry stack (`TUR_JIT_STACK_MB`, default 64) because
  MIR does not perform gcc's sibling-call optimization -- so a deep
  recursion the cc path survives only because gcc turned the self-call into
  a jump will overflow here.  That is a real difference in what the two
  engines forgive, and it is worth knowing which of your loops are actually
  lowered to loops (see the self-tail-call section above).

The engine triangle is exact on OUTPUT: `benchmarks/run-triangle.sh`
refuses to time a program whose three engines disagree, and the fixture
corpus runs under all three harnesses (`tests/run.sh`, `tests/run-turi.sh`,
`tests/run-jit.sh`).

## Benchmarking methodology

> Corrected on 2026-08-20. This section previously described a
> `scripts/run_all.sh` / `analyze_results.py` / `check_environment.sh` harness
> and a benchmark template built on `time/now-ns`, `args/parse-int`,
> `args/get` and `str/format`. None of those scripts or functions exist.
> `scripts/` contains only `wait-for-release.sh`.

### The real harness

Benchmarks live in `benchmarks/` and are run by
`benchmarks/run-benchmarks.sh` -- all of them, or one by name:

```sh
./benchmarks/run-benchmarks.sh            # every benchmark
./benchmarks/run-benchmarks.sh bench-logic-query
```

The layout it expects:

| File | Role |
|---|---|
| `benchmarks/<name>.tur` | the benchmark source |
| `benchmarks/<name>-baseline.c` | optional monomorphic C baseline to compare against |
| `benchmarks/<name>.time` | optional upper bound in milliseconds |

Results are written to `benchmarks/benchmark-results.md`.

### A benchmark does not time itself

This is the part the old template got backwards. The **runner** measures wall
time around the built executable (`measure_time` in `run-benchmarks.sh`); the
benchmark itself just does the work. So a benchmark needs no clock, no
argument parsing, and no `elapsed_ns=` output -- it is an ordinary program with
a `main`, and `benchmarks/bench-logic-query.tur` and friends are the models to
copy.

If a benchmark does need to size its work from the command line, read `*args*`
or use `args/parse` from `stdlib/args.tur`. Reading `g_tur_args` directly, or
via a hand-rolled `parse-first-arg`, is forbidden -- see CLAUDE.md.

### Build flags

The runner compiles with `-O2` by default and honours `TUR_CC_FLAGS` and `CC`:

```sh
CC=clang TUR_CC_FLAGS="-O3 -march=native" ./benchmarks/run-benchmarks.sh
```

### Reading a result honestly

- Compare against the `-baseline.c` where one exists; an absolute number on an
  unspecified machine is not a result.
- A `.time` bound is a regression tripwire, not a target -- it is an upper
  bound chosen for the slowest machine expected to run it.
- Run one benchmark at a time. A concurrent build or test suite competing for
  cores makes the number meaningless, and this repo has been bitten by exactly
  that in its test suite (see
  [test-suite-portability-guide.md](test-suite-portability-guide.md)).
- The three-engine comparison has its own runner,
  `benchmarks/run-triangle.sh`; see
  [Execution engines](#execution-engines-interpreter-vs-tur-jit-vs-cc--o2)
  above.

## Performance checklist

Use this list before calling a hot path done:

- [ ] Built with `just release` (not `just build`)
- [ ] Numeric types annotated concretely (`int`, `float`, not inferred `any`)
- [ ] Hot loops use `vec` instead of list where random access or mutation is
      needed
- [ ] No repeated string concatenation inside loops -- accumulate the
      pieces and join once; prefer a `str` view over copying a substring
- [ ] Recursive functions in tail position (verified by running with a large
      input without stack overflow)
- [ ] Profiled with `time` and at least 5 iterations; CV < 10%
- [ ] Input size swept from small to large to confirm O-complexity matches
      expectation
