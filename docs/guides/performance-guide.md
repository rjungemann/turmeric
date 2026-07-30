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
just release          # builds build-rel/tur
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
[control-flow-completeness-plan.md](../control-flow-completeness-plan.md)
(Phase CF1) for the full scope.

### Prime sieve

The Sieve of Eratosthenes benefits from a `vec` (growable array backed by a C
`malloc`ed block) rather than a linked list:

```turmeric
(import "stdlib/vec.tur")

(defn sieve [limit] :vec
  (let [flags (vec/make limit true)]
    (vec/set! flags 0 false)
    (vec/set! flags 1 false)
    (let [loop (fn [i] :void
                 (when (<= (* i i) limit)
                   (when (vec/get flags i)
                     (let [inner (fn [j] :void
                                   (when (<= j limit)
                                     (vec/set! flags j false)
                                     (inner (+ j i))))]
                       (inner (* i i))))
                   (loop (+ i 1))))]
      (loop 2))
    flags))
```

```sweet-exp
import "stdlib/vec.tur"

defn sieve [limit] :vec
  let [flags vec/make(limit true)]
    vec/set!(flags 0 false)
    vec/set!(flags 1 false)
    let [loop (fn [i] :void
                (when (<= *(i i) limit)
                  (when vec/get(flags i)
                    (let [inner (fn [j] :void
                                  (when <=(j limit)
                                    vec/set!(flags j false)
                                    inner({j + i})))]
                      inner(*(i i))))
                  loop({i + 1})))]
      loop(2)
    flags
```

### Monte Carlo pi estimation

Use `rand` (from `stdlib/rand.tur`) rather than importing `<stdlib.h>` via
inline-C so the compiler can see through calls and optimise the loop:

```turmeric
(import "stdlib/rand.tur")

(defn estimate-pi [samples] :float
  (let [loop (fn [i inside] :float
               (if (= i 0)
                 (* 4.0 (/ (int->float inside) (int->float samples)))
                 (let [x (rand/float)
                       y (rand/float)]
                   (loop (- i 1)
                         (if (<= (+ (* x x) (* y y)) 1.0)
                           (+ inside 1)
                           inside)))))]
    (loop samples 0)))
```

```sweet-exp
import "stdlib/rand.tur"

defn estimate-pi [samples] :float
  let [loop (fn [i inside] :float
              (if (= i 0)
                (* 4.0 (/ (int->float inside) (int->float samples)))
                (let [x (rand/float)
                      y (rand/float)]
                  (loop (- i 1)
                        (if (<= (+ (* x x) (* y y)) 1.0)
                          (+ inside 1)
                          inside)))))]
    loop(samples 0)
```

---

## Data structures

### Lists vs vecs

Use `list` / `cons` for functional transformations where sharing is important.
Use `vec` for index-heavy access and mutation-in-place:

| Operation | `list` | `vec` |
|-----------|--------|-------|
| Prepend | O(1) | O(n) amortised |
| Random read | O(n) | O(1) |
| Append (single) | O(n) | O(1) amortised |
| Memory per element | 2 words (pointer + tag) | 1 word |

### Hash maps

`stdlib/hamt.tur` (the persistent hash-array-mapped trie) is the standard map.
For hot paths that need mutable semantics, combine `hamt` with `ref`:

```turmeric
(import "stdlib/hamt.tur")
(import "stdlib/ref.tur")

(defn freq-count [words] :hamt
  (let [m (ref (hamt/empty))]
    (list/for-each words
                   (fn [w] :void
                     (ref/update! m
                                  (fn [h] :hamt
                                    (hamt/insert h w (+ 1 (hamt/get-or h w 0)))))))
    (ref/get m)))
```

```sweet-exp
import "stdlib/hamt.tur"
import "stdlib/ref.tur"

defn freq-count [words] :hamt
  let [m ref(hamt/empty())]
    list/for-each(words
                  (fn [w] :void
                    ref/update!(m
                                (fn [h] :hamt
                                  hamt/insert(h w {1 + hamt/get-or(h w 0)})))))
    ref/get(m)
```

HAMT operations are O(log₃₂ n) in practice -- fast for lookups but slower
than a mutable hash table for write-heavy workloads. If you need the latter,
reach for an inline-C wrapper around `uthash` or a fixed-size open-addressing
table.

### Sorting

`stdlib/vec.tur` exposes `vec/sort!` (in-place quicksort) and `list/sort`
(merge sort returning a new list):

```turmeric
(import "stdlib/vec.tur")

; in-place, cache-friendly
(let [v (vec/of 5 3 8 1 9 2)]
  (vec/sort! v <)
  v)
```

```sweet-exp
import "stdlib/vec.tur"

; in-place, cache-friendly
let [v vec/of(5 3 8 1 9 2)]
  vec/sort!(v <)
  v
```

---

## String and text processing

### Avoid repeated concatenation

`str/concat` allocates a new buffer each call. For building large strings from
many pieces, use `str/builder`:

```turmeric
(import "stdlib/str.tur")

(defn join [sep parts] :str
  (let [b (str/builder)]
    (list/for-each-indexed parts
                           (fn [i s] :void
                             (when (> i 0)
                               (str/builder/append! b sep))
                             (str/builder/append! b s)))
    (str/builder/finish b)))
```

```sweet-exp
import "stdlib/str.tur"

defn join [sep parts] :str
  let [b str/builder()]
    list/for-each-indexed(parts
                          (fn [i s] :void
                            (when (> i 0)
                              (str/builder/append! b sep))
                            (str/builder/append! b s)))
    str/builder/finish(b)
```

`str/builder/finish` produced fresh bytes, but the `:str` return above is a
*borrowed view* -- fine while the builder's buffer is alive, but not a value the
result can safely outlive. When the joined string must be **returned, stored, or
used as a Map/Set key**, reach for the owned `String` builder instead:
`stdlib/string.tur`'s `StringBuilder` (`builder/new`, `builder/push-cstr!` /
`builder/push-string!`, `builder/finish`) accumulates bytes in the same linear
time and `builder/finish` freezes them into an **owned, immutable `String`** with
no lifetime tie to the builder. `Clone[String]` is an O(1) refcount bump, so
handing that `String` to a container is free. See
[strings-guide.md](strings-guide.md) for the `cstr` vs `str` vs `String` tiering.

### Text search

For simple substring search, `str/index-of` wraps `strstr` and is O(n·m) in
the worst case. For repeated pattern matching over a corpus, compile the
pattern once and reuse the handle -- the search itself is then O(n):

```turmeric
(import "stdlib/regex.tur")

(defn count-matches [pattern text] :int
  (let [re (regex/compile pattern)]
    (regex/count re text)))
```

```sweet-exp
import "stdlib/regex.tur"

defn count-matches [pattern text] :int
  let [re regex/compile(pattern)]
    regex/count(re text)
```

### Prefer `str/view` over copying

`str/view` is a non-owning slice into an existing string. Use it when you only
need to inspect a substring -- without allocating or copying:

```turmeric
(let [s "hello world"]
  (str/view s 6 11))   ; => "world", zero-copy
```

```sweet-exp
let [s "hello world"]
  str/view(s 6 11)   ; => "world", zero-copy
```

---

## Concurrency and parallelism

### Thread creation

Threads are spawned with `spawn` (from `stdlib/concurrency.tur`):

```turmeric
(import "stdlib/concurrency.tur")

(defn parallel-map [f xs] :list
  (let [handles (list/map xs (fn [x] :handle (spawn (fn [] :any (f x)))))]
    (list/map handles (fn [h] :any (await h)))))
```

```sweet-exp
import "stdlib/concurrency.tur"

defn parallel-map [f xs] :list
  let [handles list/map(xs (fn [x] :handle spawn((fn [] :any f(x)))))]
    list/map(handles (fn [h] :any await(h)))
```

Each `spawn` creates a POSIX thread. Thread creation overhead is several
microseconds; avoid spawning threads inside tight loops.

### Channels (producer-consumer)

Use `chan` for lock-free message passing between threads:

```turmeric
(import "stdlib/chan.tur")

(defn pipeline [producer-fn consumer-fn n] :void
  (let [c (chan/make 64)]
    (spawn (fn [] :void (producer-fn c)))
    (spawn (fn [] :void (consumer-fn c)))
    (chan/close c)))
```

```sweet-exp
import "stdlib/chan.tur"

defn pipeline [producer-fn consumer-fn n] :void
  let [c chan/make(64)]
    spawn((fn [] :void producer-fn(c)))
    spawn((fn [] :void consumer-fn(c)))
    chan/close(c)
```

Channel sends block when the buffer is full; size the buffer to amortise
context-switch cost for your throughput target.

### Dynamic vars and thread-local state

`defvar` / `with-var` (from `stdlib/dynamic-vars.tur`) provide thread-local
dynamic bindings. Reads have no locking overhead -- they index directly into a
thread-local slot:

```turmeric
(import "stdlib/dynamic-vars.tur")

(defvar *indent* 0)

(defn with-indent [body] :any
  (with-var [*indent* (+ *indent* 2)] (body)))
```

```sweet-exp
import "stdlib/dynamic-vars.tur"

defvar *indent* 0

defn with-indent [body] :any
  with-var [*indent* {*indent* + 2}] body()
```

---

## Memory and allocation

### Allocation patterns

Turmeric uses a precise, tracing garbage collector. Short-lived objects are
cheap to allocate but incur GC work proportional to their number. For
allocation-heavy workloads:

- Prefer stack-allocated scalars (concrete `int`, `float`, `bool`) -- they
  never hit the GC.
- Reuse `vec` buffers with `vec/clear!` instead of allocating a fresh vec each
  iteration.
- Use `ref` sparingly; each `ref` is a heap cell.

### GC pressure benchmarks

The simplest way to measure GC impact is to compare a version that allocates
aggressively against one that reuses buffers:

```turmeric
; high churn -- allocates a new list each iteration
(defn churn [n] :void
  (let [loop (fn [i] :void
               (when (> i 0)
                 (let [_ (list/range 0 1000)]
                   (loop (- i 1)))))]
    (loop n)))

; low churn -- reuses a vec
(defn no-churn [n] :void
  (let [v (vec/make 1000 0)]
    (let [loop (fn [i] :void
                 (when (> i 0)
                   (vec/fill! v 0)
                   (loop (- i 1))))]
      (loop n))))
```

```sweet-exp
; high churn -- allocates a new list each iteration
defn churn [n] :void
  let [loop (fn [i] :void
              (when (> i 0)
                (let [_ (list/range 0 1000)]
                  (loop (- i 1)))))]
    loop(n)

; low churn -- reuses a vec
defn no-churn [n] :void
  let [v vec/make(1000 0)]
    let [loop (fn [i] :void
                (when (> i 0)
                  (vec/fill! v 0)
                  (loop (- i 1))))]
      loop(n)
```

---

## Recursion and stack usage

### Tail recursion

The compiler performs tail-call optimisation (TCO) on self-tail calls. A
function whose recursive call is in tail position compiles to a C `goto` loop
and uses O(1) stack:

```turmeric
; tail-recursive -- safe for any n
(defn factorial [n acc] :int
  (if (= n 0)
    acc
    (factorial (- n 1) (* n acc))))

(factorial 1000000 1)
```

```sweet-exp
; tail-recursive -- safe for any n
defn factorial [n acc] :int
  if ={n 0}
    acc
    factorial({n - 1} {n * acc})

factorial(1000000 1)
```

Non-tail recursion is not optimised and will overflow the stack for deep
inputs. When you cannot restructure to tail position, use an explicit stack
(a `list` or `vec` as a workaround).

### Mutual recursion

Mutually recursive functions are currently not tail-call optimised across the
function boundary. Rewrite mutual recursion as a single function with a
discriminant, or use a trampoline:

```turmeric
(import "stdlib/trampoline.tur")

(defn even? [n] :thunk
  (if (= n 0)
    (done true)
    (bounce (fn [] :thunk (odd? (- n 1))))))

(defn odd? [n] :thunk
  (if (= n 0)
    (done false)
    (bounce (fn [] :thunk (even? (- n 1))))))

(trampoline/run (even? 100000))   ; => true, O(1) stack
```

```sweet-exp
import "stdlib/trampoline.tur"

defn even? [n] :thunk
  if ={n 0}
    done(true)
    bounce((fn [] :thunk odd?({n - 1})))

defn odd? [n] :thunk
  if ={n 0}
    done(false)
    bounce((fn [] :thunk even?({n - 1})))

trampoline/run(even?(100000))   ; => true, O(1) stack
```

---

## I/O operations

### Sequential file I/O

`stdlib/io.tur` wraps `fread`/`fwrite`. For sequential reads of large files,
use a buffer size that matches your OS page size (typically 4096 or 65536
bytes):

```turmeric
(import "stdlib/io.tur")

(defn read-file [path] :str
  (let [f (io/open path "rb")]
    (let [s (io/read-all f)]
      (io/close f)
      s)))
```

```sweet-exp
import "stdlib/io.tur"

defn read-file [path] :str
  let [f io/open(path "rb")]
    let [s io/read-all(f)]
      io/close(f)
      s
```

`io/read-all` reads in 65536-byte chunks internally; do not loop over
`io/read-byte` for large files -- the per-call overhead dominates.

### Buffered writes

Writes are buffered by the C `FILE*` layer. Flush explicitly only when
durability is required:

```turmeric
(defn write-lines [path lines] :void
  (let [f (io/open path "wb")]
    (list/for-each lines (fn [line] :void
      (io/write f line)
      (io/write f "\n")))
    (io/close f)))   ; flush happens here
```

```sweet-exp
defn write-lines [path lines] :void
  let [f io/open(path "wb")]
    list/for-each(lines
                  (fn [line] :void
                    io/write(f line)
                    io/write(f "\n")))
    io/close(f)   ; flush happens here
```

Calling `io/flush` inside the loop adds a syscall per line; omit it unless you
need crash-safety.

---

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
| MIR JIT | `tur --enable=jit jit f.tur` | in-process (c2mir) | run-edit-run loops, spice REPL reloads |
| cc | `tur build f.tur` + run | subprocess cc -O2 | long-running programs, deployment |

Measured triangle (x86-64 Linux, Release `tur`, best of 3, end-to-end wall
time; `bash benchmarks/run-triangle.sh` regenerates this from
`benchmarks/triangle/`):

| program | interpreter | tur jit | cc build | cc run | cc total |
|---|---|---|---|---|---|
| fib (fib 27, call-heavy) | 223ms | 273ms | 243ms | 4ms | 247ms |
| loop-sum (5M-iteration loop) | 2295ms | 261ms | 263ms | 4ms | 267ms |
| mandel (float inner loop) | 960ms | 301ms | 262ms | 7ms | 269ms |

How to read it:

- **The front end dominates one-shot latency on every engine.** Roughly
  200ms of each cell is elaboration and codegen shared by all three legs;
  the engines differ in what happens after. For a program this small the
  interpreter's zero-compile leg makes it competitive end to end even
  while its loop throughput is 9x behind (loop-sum).
- **`tur jit` matches the cc path's one-shot latency** on Linux (its
  engine is ~40% faster than cc's compile step, but that step is a
  minority of the wall). Its structural advantage is being IN PROCESS: no
  subprocess, no disk artifacts, and the spice REPL reload path is ~3.2x
  faster than the `tur build --shared` round trip it replaces (see the
  repl guide).
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

### Harness structure

Each benchmark file should follow this template so that the automated runner
(`scripts/run_all.sh`) can collect results consistently:

```turmeric
(import "stdlib/time.tur")
(import "stdlib/args.tur")

(defn benchmark [n] :void
  ; ... work under test ...
  )

(defn main [] :void
  (let [n (args/parse-int (args/get 1) 1000)
        t0 (time/now-ns)]
    (benchmark n)
    (let [elapsed (- (time/now-ns) t0)]
      (println (str/format "elapsed_ns={}" elapsed)))))
```

```sweet-exp
import "stdlib/time.tur"
import "stdlib/args.tur"

defn benchmark [n] :void
  ; ... work under test ...

defn main [] :void
  let [n  args/parse-int(args/get(1) 1000)
       t0 time/now-ns()]
    benchmark(n)
    let [elapsed {time/now-ns() - t0}]
      println(str/format("elapsed_ns={}" elapsed))
```

Pass problem size via `*args*` (never hardcode), so the runner can sweep
sizes without rebuilding.

### Iteration count and warm-up

- Run each benchmark at least **5 times**; discard the first run (cold caches,
  JIT warm-up for JVM-backed languages in comparisons).
- Report the **trimmed mean** (drop the highest and lowest reading) for a
  robust central estimate.
- Report **coefficient of variation** (CV = stddev / mean). A CV above 10%
  signals an unstable measurement -- check for background load or OS
  scheduling noise.

### Sizing inputs

Choose three input sizes:

| Label | Guideline | Rationale |
|-------|-----------|-----------|
| small | < 1 ms wall time | Exercises startup path |
| medium | 100 ms -- 1 s | Exercises algorithm body |
| large | 5 -- 30 s | Exercises memory and cache behaviour |

Avoid inputs so large that a single run takes minutes; reproducibility suffers.

### Reading results

`scripts/analyze_results.py` computes speedup ratios relative to a C baseline
and writes `results/processed/summary.json`. Use it rather than eyeballing raw
numbers:

```sh
python3 scripts/analyze_results.py
```

The output normalises all timings to C = 1.0. A Turmeric ratio of 1.4 means
Turmeric took 1.4× as long as C on that benchmark.

### Environment checklist

Before publishing numbers, run:

```sh
bash scripts/check_environment.sh
```

This prints exact compiler and runtime versions and hardware details. Always
include this output alongside benchmark results so readers can reproduce.

---

## Performance checklist

Use this list before calling a hot path done:

- [ ] Built with `just release` (not `just build`)
- [ ] Numeric types annotated concretely (`int`, `float`, not inferred `any`)
- [ ] Hot loops use `vec` instead of list where random access or mutation is
      needed
- [ ] No repeated `str/concat` inside loops -- use `str/builder`
- [ ] Recursive functions in tail position (verified by running with a large
      input without stack overflow)
- [ ] Profiled with `time` and at least 5 iterations; CV < 10%
- [ ] Input size swept from small to large to confirm O-complexity matches
      expectation
