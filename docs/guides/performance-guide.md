---
title: Performance Guide
category: Language Features
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

## 1. Numerical computation

### Integer and floating-point arithmetic

Arithmetic on `int` and `float` compiles to the corresponding C types.
There is no boxing overhead for scalars declared with concrete types:

```turmeric
(defn square [x] :int (* x x))
(defn hyp [a b] :float
  (sqrt (+ (* a a) (* b b))))
```

Avoid leaving numeric expressions untyped in hot loops -- the elaborator may
widen to a tagged value when it cannot infer a concrete numeric type.

### Fibonacci (iterative vs recursive)

Iterative is faster for large N because it avoids stack growth:

```turmeric
; iterative -- O(n) time, O(1) space
(defn fib-iter [n] :int
  (let [loop (fn [i a b] :int
               (if (= i 0) a (loop (- i 1) b (+ a b))))]
    (loop n 0 1)))

; recursive -- O(2^n) time, avoid for n > ~30
(defn fib-rec [n] :int
  (if (< n 2) n (+ (fib-rec (- n 1)) (fib-rec (- n 2)))))
```

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

### Monte Carlo pi estimation

Use `rand` (from `stdlib/rand.tur`) rather than importing `<stdlib.h>` via
inline-C so the compiler can see through calls and optimise the loop:

```turmeric
(import "stdlib/rand.tur")

(defn estimate-pi [samples] :float
  (let [loop (fn [i inside] :float
               (if (= i 0)
                   (* 4.0 (/ (int->float inside) (int->float samples)))
                   (let [x (rand/float) y (rand/float)]
                     (loop (- i 1)
                           (if (<= (+ (* x x) (* y y)) 1.0)
                               (+ inside 1)
                               inside)))))]
    (loop samples 0)))
```

---

## 2. Data structures

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
    (list/for-each words (fn [w] :void
      (ref/update! m (fn [h] :hamt
        (hamt/insert h w (+ 1 (hamt/get-or h w 0)))))))
    (ref/get m)))
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

---

## 3. String and text processing

### Avoid repeated concatenation

`str/concat` allocates a new buffer each call. For building large strings from
many pieces, use `str/builder`:

```turmeric
(import "stdlib/str.tur")

(defn join [sep parts] :str
  (let [b (str/builder)]
    (list/for-each-indexed parts (fn [i s] :void
      (when (> i 0) (str/builder/append! b sep))
      (str/builder/append! b s)))
    (str/builder/finish b)))
```

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

### Prefer `str/view` over copying

`str/view` is a non-owning slice into an existing string. Use it when you only
need to inspect a substring -- no allocation, no copy:

```turmeric
(let [s "hello world"]
  (str/view s 6 11))   ; => "world", zero-copy
```

---

## 4. Concurrency and parallelism

### Thread creation

Threads are spawned with `spawn` (from `stdlib/concurrency.tur`):

```turmeric
(import "stdlib/concurrency.tur")

(defn parallel-map [f xs] :list
  (let [handles (list/map xs (fn [x] :handle (spawn (fn [] :any (f x)))))]
    (list/map handles (fn [h] :any (await h)))))
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

---

## 5. Memory and allocation

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
                 (let [_ (list/range 0 1000)] (loop (- i 1)))))]
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

---

## 6. Recursion and stack usage

### Tail recursion

The compiler performs tail-call optimisation (TCO) on self-tail calls. A
function whose recursive call is in tail position compiles to a C `goto` loop
and uses O(1) stack:

```turmeric
; tail-recursive -- safe for any n
(defn factorial [n acc] :int
  (if (= n 0) acc (factorial (- n 1) (* n acc))))

(factorial 1000000 1)
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
  (if (= n 0) (done true) (bounce (fn [] :thunk (odd? (- n 1))))))

(defn odd? [n] :thunk
  (if (= n 0) (done false) (bounce (fn [] :thunk (even? (- n 1))))))

(trampoline/run (even? 100000))   ; => true, O(1) stack
```

---

## 7. I/O operations

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

Calling `io/flush` inside the loop adds a syscall per line; omit it unless you
need crash-safety.

---

## 8. Real-world algorithms

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

### Ray tracing

Ray-box intersection and dot products are the hot paths. Annotate return types
concretely (`float`) so the elaborator does not insert tag checks in the inner
loop:

```turmeric
(defn dot [ax ay az bx by bz] :float
  (+ (* ax bx) (+ (* ay by) (* az bz))))
```

---

## 9. Benchmarking methodology

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
  (let [n (args/parse-int (args/get 1) 1000)]
    (let [t0 (time/now-ns)]
      (benchmark n)
      (let [elapsed (- (time/now-ns) t0)]
        (println (str/format "elapsed_ns={}" elapsed))))))
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

## 10. Performance checklist

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
