# Dictionary Passing Benchmark Results
Generated: Wed May 13 03:44:13 PDT 2026

## Results

- **bind-option**: 215ms
- **fmap-vec**: 228ms
  - Baseline: 224ms, Overhead ratio: 1.01x

## Capture/restore cost curve (SX0(a))

The per-path capture and restore curves live in their own file rather than
being appended here: the sweep is ~145 rows across three capture paths and
four axes, and folding that into this page would bury the dictionary-passing
numbers it exists for.

- Report: [capture-curve-results.md](capture-curve-results.md)
- Raw CSV: [capture-curve.csv](capture-curve.csv)
- Regenerate: `bash benchmarks/run-capture-curve.sh`

## Solver cap sweep (SX0(b))

- Report: [cap-sweep-results.md](cap-sweep-results.md)
- Regenerate: `bash benchmarks/run-cap-sweep.sh`
