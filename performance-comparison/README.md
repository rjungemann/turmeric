# Performance Comparison: C, Turmeric, Clojure, Racket, Python

## Setup

### Language Versions
```
| Language  | Version       | Implementation       |
|-----------|---------------|---------------------|
| C         | C17/C23       | Apple clang 17.0.0   |
| Turmeric  | latest        | Custom               |
| Clojure   | 1.12.5        | JVM (Clojure CLI)    |
| Racket    | 9.1           | Chez Scheme          |
| Python    | 3.13.1        | CPython (pyenv)      |
```

### Build Commands
```
| Language  | Build/Run Command                     |
|-----------|---------------------------------------|
| C         | clang -O3 -o binary source.c          |
| Turmeric  | just release && build-rel/tur file.tur |
| Clojure   | clojure -M script.clj                 |
| Racket    | racket script.rkt                     |
| Python    | python3 script.py                     |
```

## Structure
```
performance-comparison/
├── benchmarks/
│   ├── numerical/
│   ├── data_structures/
│   ├── ...
├── inputs/
├── results/
├── scripts/
└── docs/
```

## Phase 1: Setup (Complete)
- [x] Define language versions
- [x] Create directory structure
- [x] Document build commands
- [x] Set up environment configuration

## Next Steps
1. Implement input generation utilities
2. Create automated test runner
3. Begin numerical computation benchmarks