# Benchmarking Methodology

## Environment
- **Hardware**: MacBook Pro (M3, 16GB RAM, 1TB SSD)
- **OS**: macOS 14.5
- **Compiler/Interpreter Versions**:
  - C: Apple clang 17.0.0
  - Turmeric: latest (local build)
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