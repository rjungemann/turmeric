# Performance Comparison Plan: C, Turmeric, Clojure, Racket, and Python

## Overview

This document outlines a plan for conducting performance comparisons between five programming languages — C, Turmeric, Clojure, Racket, and Python — on similar computational tasks. The goal is to identify relative strengths, weaknesses, and trade-offs for different categories of programming problems.

---

## Objectives

1. **Benchmark Execution Speed**: Measure raw computation speed across languages
2. **Memory Usage Analysis**: Track memory consumption patterns
3. **Startup Time Comparison**: Measure cold-start and warm-start performance
4. **Idiomatic Code Performance**: Compare performance of idiomatic implementations
5. **Scalability Testing**: Observe behavior with increasing workload sizes
6. **Identify Language Strengths**: Determine which language excels at which task types

---

## Languages and Versions

| Language | Version | Implementation | Binary / Invocation | Notes |
|----------|---------|----------------|---------------------|-------|
| C | C17/C23 | Apple clang 17.0.0 | `clang` | Baseline for compiled performance |
| Turmeric | latest | Custom | `build-rel/tur` (local release build) | Primary subject of comparison |
| Clojure | 1.12.5 | JVM (Clojure CLI) | `clojure` | Functional, dynamic, hosted |
| Racket | 9.1 | Chez Scheme | `racket` | Functional, dynamic, with JIT |
| Python | 3.13.1 | CPython (pyenv) | `python3` | Dynamic, interpreted |

---

## Task Categories

### 1. Numerical Computation
Tasks involving heavy arithmetic, matrix operations, and mathematical algorithms.

| Task | Description | Metrics |
|------|-------------|---------|
| Fibonacci sequence | Recursive and iterative implementations | Time, stack usage |
| Prime number generation | Sieve of Eratosthenes, trial division | Time, memory |
| Matrix multiplication | N×N matrix multiplication | Time, memory |
| Floating-point operations | Monte Carlo π estimation, mandelbrot | Time, precision |
| Fast Fourier Transform | FFT on generated signal data | Time, accuracy |

### 2. Data Structure Operations
Tasks testing memory access patterns and data structure performance.

| Task | Description | Metrics |
|------|-------------|---------|
| List operations | Append, prepend, reverse, sort | Time per operation |
| Hash map operations | Insert, lookup, delete (1K, 10K, 100K entries) | Time per operation |
| Tree traversal | Binary search tree operations | Time, memory |
| Graph algorithms | BFS, DFS, shortest path | Time, memory |
| Sorting algorithms | QuickSort, MergeSort on random and sorted data | Time |

### 3. String and Text Processing
Tasks involving string manipulation and text parsing.

| Task | Description | Metrics |
|------|-------------|---------|
| String concatenation | Build large string from many small strings | Time, memory |
| Regex matching | Pattern matching on large text corpus | Time |
| JSON parsing | Parse and validate JSON documents | Time, memory |
| CSV parsing | Read and process CSV data | Time |
| Text search | Find all occurrences of substring | Time |

### 4. Concurrency and Parallelism
Tasks measuring multi-threaded and parallel processing capabilities.

| Task | Description | Metrics |
|------|-------------|---------|
| Thread creation | Create and join N threads | Time |
| Parallel map | Map function over list in parallel | Time, speedup |
| Producer-consumer | Thread-safe queue operations | Throughput |
| Web server | Handle concurrent HTTP requests | Requests/sec, latency |
| Lock contention | Measure overhead of synchronization primitives | Time |

### 5. Memory and Garbage Collection
Tasks testing memory allocation patterns and GC behavior.

| Task | Description | Metrics |
|------|-------------|---------|
| Object allocation | Allocate N objects in loop | Time, memory growth |
| Memory churn | Repeated allocation and deallocation | Time, peak memory |
| GC pressure | Create short-lived objects | Time, GC pauses |
| Large data structures | Build and traverse large arrays/maps | Memory, time |

### 6. Recursion and Stack Usage
Tasks testing function call overhead and stack behavior.

| Task | Description | Metrics |
|------|-------------|---------|
| Deep recursion | Recursive factorial with large N | Time, stack depth |
| Tail recursion | Tail-recursive implementations | Time, stack usage |
| Mutual recursion | Two functions calling each other | Time, stack depth |
| Trampolining | Trampolined recursion | Time, memory |

### 7. I/O Operations
Tasks measuring file and network I/O performance.

| Task | Description | Metrics |
|------|-------------|---------|
| File read | Read large file sequentially | Time, throughput |
| File write | Write large data to file | Time, throughput |
| Random file access | Random reads from large file | Time, seek performance |
| Network requests | HTTP GET requests to local server | Time, requests/sec |

### 8. Real-world Algorithms
Complex, realistic algorithms that combine multiple aspects.

| Task | Description | Metrics |
|------|-------------|---------|
| Ray tracing | Render simple 3D scene | Time |
| N-body simulation | Simulate gravitational interactions | Time |
| K-means clustering | Cluster N points in D dimensions | Time, iterations |
| PageRank | Compute PageRank on web graph | Time, memory |
| SQL query processing | Parse and execute SQL-like queries | Time |

---

## Methodology

### Test Environment
- **Hardware**: Same machine for all tests (specify CPU, RAM, storage type)
- **OS**: Linux/macOS (specify version)
- **Compiler/Interpreter versions**: Document exact versions
- **Optimization flags**: -O3 for C, standard for others
- **Run multiple iterations**: At least 5-10 runs per test, discard outliers
- **Warm-up runs**: Perform warm-up iterations before measurement

### Measurement Tools
- **Time**: Use high-resolution timers (C: `clock_gettime`, Python: `time.perf_counter`, etc.)
- **Memory**: Use language-specific tools (C: `getrusage`, JVM: JMX, Python: `tracemalloc`)
- **CPU**: Track CPU usage percentage
- **External validation**: Use `/usr/bin/time -v` for cross-validation

### Test Harness Requirements
1. Each language has identical test harness structure
2. Input generation is consistent across languages
3. Output validation ensures correctness before benchmarking
4. Results logged in structured format (JSON/CSV)

---

## Implementation Plan

### Phase 1: Setup (Week 1)
- [x] Define exact versions of all language runtimes
- [x] Set up consistent build/environment configuration
- [x] Create template repository structure for each language
- [x] Implement input generation utilities (all categories: numerical, data_structures, string_processing, concurrency, memory, recursion)
- [x] Set up automated test runner (`scripts/run_all.sh` supports all 6 categories × 5 languages)

### Phase 2: Core Tasks (Weeks 2-4)
- [x] Implement numerical computation benchmarks (fibonacci, primes, matrix_multiply, monte_carlo_pi — C/Turmeric/Clojure/Racket/Python)
- [x] Implement data structure benchmarks (list_ops, hash_map, sort — C/Turmeric/Clojure/Racket/Python)
- [x] Implement string processing benchmarks (string_concat, text_search — C/Turmeric/Clojure/Racket/Python)
- [x] Implement concurrency benchmarks (thread_ring — C/Turmeric/Clojure/Racket/Python)
- [x] Implement memory/GC benchmarks (alloc_churn — C/Turmeric/Clojure/Racket/Python)
- [x] Implement recursion benchmarks (fib_recursive, factorial — C/Turmeric/Clojure/Racket/Python)

### Phase 3: I/O and Real-world (Weeks 5-6)
- [x] Implement I/O benchmarks (file_write, file_read, random_access — C/Turmeric/Clojure/Racket/Python)
- [x] Implement real-world algorithm benchmarks (nbody, ray_tracing — C/Turmeric/Clojure/Racket/Python)
- [x] Add micro-benchmarks for specific operations (int_arith, float_arith, function_call — C/Turmeric/Clojure/Racket/Python)

### Phase 4: Validation (Week 7)
- [ ] **Correctness Verification**: 
  - Implement cross-language validation tests for each benchmark
  - Verify identical outputs for same inputs across all languages
  - Create golden test files for reference outputs
  - (`scripts/validate_correctness.py` stub exists; no benchmarks implemented yet)

- [ ] **Reproducibility Testing**:
  - Run full benchmark suite 10+ times with identical environment
  - Calculate standard deviation and coefficient of variation
  - Document any observed non-determinism

- [ ] **Environment Validation**:
  - Verify consistent hardware/software configuration across runs
  - Check for background process interference
  - Validate compiler/interpreter versions match documentation

- [ ] **Benchmark Analysis**:
  - Identify outliers and investigate causes
  - Check for consistent performance characteristics
  - Validate measurement tools are working correctly

- [ ] **Issue Resolution**:
  - Document and categorize all discovered issues
  - Implement fixes for critical correctness problems
  - Note limitations and caveats for analysis phase

### Phase 5: Analysis and Documentation (Week 8)
- [ ] Aggregate and normalize results
- [ ] Create visualizations (charts, graphs)
- [ ] Write analysis and conclusions
- [ ] Document limitations and caveats

---

## Expected Outputs

### Data Files
- `results/numerical.json` - Raw numerical computation results
- `results/data_structures.json` - Raw data structure results
- `results/string_processing.json` - Raw string processing results
- `results/concurrency.json` - Raw concurrency results
- `results/memory.json` - Raw memory results
- `results/recursion.json` - Raw recursion results
- `results/io.json` - Raw I/O results
- `results/real_world.json` - Raw real-world algorithm results

### Analysis Files
- `analysis/comparison.md` - Detailed comparison with charts
- `analysis/by_category.md` - Breakdown by task category
- `analysis/by_language.md` - Breakdown by language
- `analysis/conclusions.md` - Key findings and recommendations

### Visualizations
- Bar charts comparing execution times
- Line graphs for scalability tests
- Heatmaps for multi-dimensional comparisons
- Box plots for result distributions

---

## File Structure

```
performance-comparison/
├── benchmarks/
│   ├── numerical/
│   │   ├── c/
│   │   ├── turmeric/
│   │   ├── clojure/
│   │   ├── racket/
│   │   └── python/
│   ├── data_structures/
│   │   ├── c/
│   │   ├── turmeric/
│   │   ├── clojure/
│   │   ├── racket/
│   │   └── python/
│   └── ... (other categories)
├── inputs/
│   ├── small/
│   ├── medium/
│   └── large/
├── results/
│   ├── raw/
│   └── processed/
├── scripts/
│   ├── run_all.sh
│   ├── analyze.py
│   └── visualize.py
├── docs/
│   └── methodology.md
└── README.md
```

---

## Success Criteria

1. **Reproducibility**: Any researcher can replicate results with provided code
2. **Fairness**: Each language uses idiomatic, optimized implementations
3. **Comprehensiveness**: Cover all major performance dimensions
4. **Transparency**: Full disclosure of methodology, environment, and caveats
5. **Statistical Rigor**: Sufficient iterations and proper statistical analysis

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Language expertise gaps | Consult community experts for idiomatic code |
| Environment differences | Use containerized environments (Docker) |
| Version differences | Pin exact versions, document everything |
| Optimization skew | Test both optimized and unoptimized builds |
| Result interpretation | Include multiple perspectives in analysis |
| Time constraints | Prioritize core tasks, defer nice-to-haves |

---

## Hypotheses

1. **C will be fastest** for low-level numerical computation and memory operations
2. **Turmeric will be competitive** with C on many tasks, possibly faster on functional patterns
3. **Clojure will have JVM warm-up overhead** but strong sustained performance
4. **Racket will show good performance** on functional tasks with JIT compilation
5. **Python will be slowest** on CPU-bound tasks but potentially fast for I/O and glue code
6. **Memory usage will vary significantly** based on GC strategies and object models

---

## Compiled Binary Size Comparison

For languages that can compile to a standalone binary (C, Turmeric, and Racket via `raco exe`), measure the output binary size as an additional dimension of comparison.

| Language | Compilation Method | Notes |
|----------|--------------------|-------|
| C | `clang -O3 -o binary source.c` | Baseline; typically very small |
| Turmeric | `tur build` | Primary subject |
| Racket | `raco exe` + `raco distribute` | Bundles runtime; larger output |

Clojure and Python are excluded as they do not produce self-contained native binaries (Clojure produces JARs requiring JVM; Python requires the interpreter).

### Metrics

- **Strip size**: Size of binary after `strip` (removes debug symbols)
- **Debug size**: Size of unstripped binary
- **Static vs. dynamic linking**: Note whether the binary links libc and other libs statically or dynamically
- **Compression ratio**: `gzip -9` size as a proxy for code density

### Tasks to Measure

Use a representative subset of benchmarks (e.g., Fibonacci, prime sieve, matrix multiplication) so binary size reflects realistic program complexity, not just a "hello world" baseline.

### Output

- `results/binary_size.json` - Raw size measurements per language and task
- Include binary size as a column in `analysis/comparison.md`

---

## Questions to Answer

1. How does Turmeric compare to C for systems programming tasks?
2. Where does Turmeric outperform more established languages?
3. What are the memory usage patterns for each language?
4. How do startup times compare for scripting vs. compiled languages?
5. Which language has the most predictable performance characteristics?
6. Are there tasks where dynamic typing provides performance benefits?
7. How does immutability impact performance in functional languages?
8. How do compiled binary sizes compare across C, Turmeric, and Racket?

---

## Next Steps

1. Review and refine this plan
2. Set up initial environment and templates
3. Begin with numerical computation benchmarks as proof of concept
4. Iterate on methodology based on early results
5. Expand to full benchmark suite
