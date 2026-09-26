# `#lang r7rs`: `(features)` lists `ratios`, but `cond-expand` says it is absent

**Severity:** low. R7RS 4.2.1 says `cond-expand` tests the feature
identifiers that `(features)` returns. Here, the two lists have drifted apart:
`ratios` is in `(features)` but a `cond-expand` clause on `ratios` does not
hold. Found while planning SRFI support (docs/upcoming/r7rs-srfi-plan.md),
which adds `srfi-N` feature identifiers and would widen the drift if nothing
changed.

## Repro

Measured 2026-09-26 at fdd51fc9, Debug build; the same on both back ends.

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(cond-expand (ratios (display "ratios-yes")) (else (display "ratios-NO")))
(newline)
(display (if (memq 'ratios (features)) "in-features" "not-in-features"))
(newline)
```

```
ratios-NO
in-features
```

## Root cause

Two hand-maintained copies of one list, each with a comment asking to keep
them equal:

- `feature_holds`, src/compiler/scheme_lower.c:3970, accepts `r7rs`,
  `exact-closed` and `turmeric`.
- `r7rs-features`, stdlib/r7rs/prelude.tur:2424, returns `r7rs`,
  `exact-closed`, `ratios` and `turmeric`.

`ratios` went into the prelude with r7rs-lang-plan T2 (exact rationals), and
`feature_holds` was not updated.

## Fix directions

- The one-line fix is to add `ratios` to `feature_holds`.
- The durable fix is one source for both. r7rs-srfi-plan S1 generates the
  list from the table it adds (the lowering emits `(features)`'s list, or a
  sync check fails the build when the two differ), because that stage adds a
  `srfi-N` identifier per supported SRFI and a third hand-kept copy would
  drift the same way.
- Fixture: `r7rs-features-agree` asserts that every identifier `(features)`
  returns holds in `cond-expand`. For each element, generate a
  `cond-expand` on its literal name.
