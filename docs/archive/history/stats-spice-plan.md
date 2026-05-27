# Spice Plan: tur-stats

> **Status:** Draft Plan
> **Last Updated:** 2026-05-24
> **Type:** Spice Design

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-stats` | `stats-v0.1.0` | `tur-frame`, `tur-math` | Common statistical analysis on dataframes |

`tur-stats` is a pure-Turmeric spice that operates on `tur-frame` columns and
frames. It covers the analysis surface that an undergraduate statistics or
intro-data-science workflow needs: descriptive summaries, common probability
distributions, hypothesis tests, and ordinary-least-squares regression, plus a
small resampling toolkit (bootstrap, permutation, train/test split).

`tur-stats` has no cmake C dependency of its own. Inline-C is limited to:
- a vendored **PCG32** PRNG (one `.h` worth of code, MIT) so RNG-driven
  routines are seedable and deterministic without relying on `rand()`,
- thin wrappers for `<math.h>` functions the stdlib does not already expose
  (`erf`, `erfc`, `lgamma`, `tgamma`, `expm1`, `log1p`).

Everything else -- column reductions, regression solver, p-value calculation,
test statistics -- is plain Turmeric.

---

## Scope

### In scope for v0.1.0

- **Descriptive stats**: mean, median, mode, variance, sd, quantile, IQR,
  range, skewness, kurtosis, covariance, correlation (Pearson, Spearman).
- **Distributions**: normal, t, chi-squared, F, uniform, binomial, poisson,
  exponential, beta, gamma. Each exposes `pdf` / `cdf` / `quantile` / `random`.
- **Tests**: one-sample and two-sample t (Welch and pooled), paired t,
  one-way ANOVA, chi-squared goodness-of-fit, chi-squared contingency,
  Mann-Whitney U (rank-sum), Wilcoxon signed-rank, Kolmogorov-Smirnov
  (one- and two-sample), F-test for variances, Pearson correlation test.
- **Linear regression**: OLS for one or more predictors, returns coefficients
  + standard errors + t / p values + R^2 / adjusted R^2 + residuals + fitted.
- **Resampling**: bootstrap (percentile + BCa), permutation tests,
  stratified train/test split, k-fold cross-validation index generator.
- **Random number generation**: seedable PCG32 with stream support; helpers
  for shuffling, sampling without replacement, sampling with replacement.

### Out of scope for v0.1.0

Tracked for follow-up spices; called out so the API does not block them.

| Future spice | What it adds |
|--------------|--------------|
| `tur-stats-glm` | Logistic, Poisson, gamma regression; link functions |
| `tur-stats-ts` | Time-series (ACF/PACF, ARIMA, exponential smoothing) |
| `tur-stats-bayes` | MCMC samplers, posterior summaries, conjugate updates |
| `tur-stats-survival` | Kaplan-Meier, Cox proportional hazards |
| `tur-stats-multivariate` | PCA, factor analysis, k-means (already partially in `tur-frame`?) |
| `tur-stats-formula` | Wilkinson-style formula DSL (`y ~ x1 * x2 + I(x3^2)`): lexer, Pratt parser, term-set algebra, factor dummy-coding, intercept control; returns column lists compatible with `ols`/`ols-frame` |

---

## Conventions

Standard spice layout:

```
spices/stats/
  build.tur
  src/stats/
    pcg32.h         -- vendored PCG32 PRNG header (no .tur file)
    math_ext.tur    -- "stats/mathx"  erf/erfc/lgamma/tgamma/expm1/log1p
    rng.tur         -- "stats/rng"    PCG32 wrapper, shuffle, sample
    summary.tur     -- "stats/summary"  mean/median/sd/quantile/skew/kurt
    cov.tur         -- "stats/cov"    covariance + correlation (Pearson/Spearman)
    dist.tur        -- "stats/dist"   probability distributions (pdf/cdf/quantile/random)
    test.tur        -- "stats/test"   hypothesis tests, returning test-result
    regress.tur     -- "stats/regress"  OLS linear regression
    sample.tur      -- "stats/sample"  bootstrap, permutation, split, cv-folds
    fmt.tur         -- "stats/fmt"    pretty-print for test-result / lm-fit
  tests/stats/
    summary_test.tur
    dist_test.tur
    test_test.tur
    regress_test.tur
    sample_test.tur
    rng_test.tur
```

---

## Architecture

```
caller (tur-frame columns or :float vectors)
  |
  v
stats/summary   -- column / vector reductions
stats/cov       -- pairwise reductions on multiple columns
stats/dist      -- analytic pdf/cdf/quantile + RNG-backed random()
stats/test      -- builds on summary + dist, returns test-result struct
stats/regress   -- normal-equation OLS via Cholesky on (X'X), returns lm-fit
stats/sample    -- index generators for bootstrap / permutation / k-fold
  ^
  |
stats/rng       -- PCG32 stream(s); seeded once, shared by sample + dist.random
stats/mathx     -- math.h functions not already in stdlib
```

All input data is one of:
1. a `tur-frame` column (preferred -- typed, null-aware),
2. a raw cons list of `:float` (for ad-hoc and test code),
3. a `tur-frame` frame plus column-name arguments (for regression and tests
   that take multiple columns).

All output is one of:
1. a scalar `:float` (most descriptive stats),
2. a `test-result` struct (every hypothesis test),
3. an `lm-fit` struct (regression),
4. a `tur-frame` frame (resampling indices, k-fold tables, `summary` reports).

This keeps the spice's surface uniform with `tur-frame` -- aggregations and
results round-trip through frames so they compose with `select`, `arrange`,
`write-csv`, etc.

---

## Result types

```turmeric
;;; test-result -- output of every test in stats/test.
(defstruct test-result
  name       :cstr   ;; "Welch Two-Sample t-test" etc.
  statistic  :float  ;; the test statistic (t, F, U, chi^2, ...)
  df1        :float  ;; primary df (NaN if not applicable)
  df2        :float  ;; secondary df (NaN if not applicable; e.g. F-test denom)
  p-value    :float  ;; two-sided unless alt-tag says otherwise
  alt-tag    :int    ;; 0=two-sided 1=less 2=greater
  estimate   :float  ;; point estimate (mean diff, correlation, ...; NaN if N/A)
  ci-low     :float  ;; confidence-interval low (NaN if not computed)
  ci-high    :float  ;; confidence-interval high (NaN if not computed)
  conf-level :float) ;; e.g. 0.95

;;; lm-fit -- output of stats/regress ols.
(defstruct lm-fit
  coefs        :int    ;; tur-frame column<:float>, length p+1 (intercept first)
  se           :int    ;; tur-frame column<:float>, standard errors
  t-values     :int    ;; tur-frame column<:float>
  p-values     :int    ;; tur-frame column<:float>
  fitted       :int    ;; tur-frame column<:float>, length n
  residuals    :int    ;; tur-frame column<:float>, length n
  r-squared    :float
  adj-r-squared :float
  sigma        :float  ;; residual standard error
  df-residual  :int    ;; n - p - 1
  df-model     :int    ;; p
  n-obs        :int    ;; n
  names        :int)   ;; cons list of :cstr, length p+1 (intercept first)
```

---

## Modules and exports

### stats/rng

```turmeric
;; Construct a seeded PCG32 stream.  Two streams seeded the same way produce
;; identical sequences across runs.  seed = 0 means "use a system entropy seed".
(rng-make seed)                            ;; => rng :int
(rng-clone r)                              ;; => rng :int  (independent copy at same state)
(rng-free r)                               ;; => :void

;; Basic primitive draws.
(rng-uint32 r)                             ;; => :int   (in [0, 2^32))
(rng-uint64 r)                             ;; => :int
(rng-uniform r)                            ;; => :float (in [0,1))
(rng-uniform-range r lo hi)                ;; => :float (in [lo, hi))
(rng-int-range r lo hi)                    ;; => :int   (in [lo, hi))

;; Shuffles and samples.
(rng-shuffle! r indices)                   ;; => :void  (in-place Fisher-Yates on int-list)
(rng-shuffled r indices)                   ;; => list<:int>  (immutable variant)
(rng-sample r col n with-replacement?)     ;; => column   (sample n rows from a column)
(rng-sample-indices r n k with-replacement?)
                                           ;; => list<:int>  (k indices from [0,n))
```

PCG32 is a single ~80-line C header. The `rng :int` is a pointer to a
heap-allocated `{ uint64_t state; uint64_t inc; }` struct.

---

### stats/summary

Every reduction skips nulls and returns NaN on an empty / all-null input.
Frame variants are convenience wrappers that look up the column by name and
delegate to the column variant.

```turmeric
;; Column reductions.  col = tur-frame column of int* or f*.
(col-count col)                            ;; => :int   non-null count
(col-sum col)                              ;; => :float
(col-mean col)                             ;; => :float
(col-median col)                           ;; => :float
(col-mode col)                             ;; => :float
(col-var col)                              ;; => :float  sample variance (n-1)
(col-sd col)                               ;; => :float  sample sd
(col-min col)                              ;; => :float
(col-max col)                              ;; => :float
(col-range col)                            ;; => :float  max - min
(col-quantile col q)                       ;; => :float  q in [0,1], type-7 interp
(col-iqr col)                              ;; => :float
(col-skewness col)                         ;; => :float  Fisher-Pearson
(col-kurtosis col)                         ;; => :float  excess kurtosis

;; Frame variants -- look up column by name, error if not numeric.
(frame-mean f name)                        ;; => result<:float>
(frame-median f name)
;; ... one wrapper per column reduction above ...

;; Whole-frame report -- pandas-style describe; returns a frame.
(describe f)                               ;; => frame
  ;; Columns: stat (utf8), then one column per numeric input column.
  ;; Rows: count, mean, sd, min, 25%, 50%, 75%, max.
```

`describe` overlaps with `tur-frame`'s `frame/print describe`; the
implementations are aligned (one of them delegates to the other -- decided in
FR7 / ST1 cross-coordination).

---

### stats/cov

```turmeric
;; Pairwise covariance and correlation between two columns.
(cov a b)                                  ;; => :float  sample covariance
(cor a b)                                  ;; => :float  Pearson
(cor-spearman a b)                         ;; => :float  Spearman rank correlation

;; Covariance / correlation matrix over a list of columns.
;; Returns an N-by-N frame with row+column "name" header columns.
(cov-matrix cols names)                    ;; => frame
(cor-matrix cols names)                    ;; => frame
(cor-matrix-spearman cols names)           ;; => frame

;; Frame convenience.
(frame-cor f names)                        ;; => result<frame>
(frame-cor-spearman f names)               ;; => result<frame>
```

---

### stats/dist

Every distribution exposes the same four operations. Distribution objects are
small structs (parameters + a type tag); we expose them so callers can pass a
distribution around as a single value.

```turmeric
;; Constructors.
(dist-normal mu sigma)                     ;; => dist :int
(dist-t df)                                ;; => dist :int
(dist-chi2 df)                             ;; => dist :int
(dist-f df1 df2)                           ;; => dist :int
(dist-uniform a b)                         ;; => dist :int
(dist-binomial n p)                        ;; => dist :int
(dist-poisson lambda)                      ;; => dist :int
(dist-exponential rate)                    ;; => dist :int
(dist-beta alpha beta)                     ;; => dist :int
(dist-gamma shape rate)                    ;; => dist :int

;; Operations (uniform across distributions).
(pdf d x)                                  ;; => :float  density / pmf at x
(cdf d x)                                  ;; => :float  P(X <= x)
(quantile d p)                             ;; => :float  inverse CDF; p in [0,1]
(random d rng)                             ;; => :float  one draw
(random-n d rng n)                         ;; => column<:float>  n draws

;; Convenience top-level helpers (no constructor needed).
(dnorm x mu sigma)                         ;; => :float  pdf
(pnorm x mu sigma)                         ;; => :float  cdf
(qnorm p mu sigma)                         ;; => :float  quantile
(rnorm rng n mu sigma)                     ;; => column<:float>
;; ... same shape for dt/pt/qt/rt, dchi2/..., df/..., etc.
```

Quantile functions use closed-form when available (normal via Beasley-Springer-Moro,
exponential, uniform) and bisection on the CDF when not. CDFs use:
- normal: `erf` (from `stats/mathx`)
- t / F / chi2 / gamma / beta: regularized incomplete beta and gamma functions
  (implemented via continued-fraction expansions in `stats/dist`)
- binomial / poisson: direct summation up to `x`, switching to normal
  approximation for very large parameters.

---

### stats/test

Every test returns a `test-result` struct. `alt-tag` is one of
`(alt-two-sided)`, `(alt-less)`, `(alt-greater)`.

```turmeric
;; Constants for the alt-tag field.
(alt-two-sided) ;; => :int 0
(alt-less)      ;; => :int 1
(alt-greater)   ;; => :int 2

;; One-sample t-test: is mean(col) == mu?
(t-test-1samp col mu alt-tag conf-level)   ;; => test-result

;; Two-sample t-test.  pooled? = 0 uses Welch (recommended default).
(t-test-2samp a b pooled? alt-tag conf-level)
                                           ;; => test-result

;; Paired t-test: a and b are paired observations of equal length.
(t-test-paired a b alt-tag conf-level)     ;; => test-result

;; One-way ANOVA on a list of group columns.
(anova-oneway cols)                        ;; => test-result   (F statistic)

;; Chi-squared goodness-of-fit.
;; observed = column of counts; expected = column of expected counts (same length).
(chi2-gof observed expected)               ;; => test-result

;; Chi-squared test of independence on a contingency table (a frame).
(chi2-contingency f)                       ;; => test-result

;; F-test for equality of variances.
(var-test a b alt-tag conf-level)          ;; => test-result

;; Pearson correlation test.
(cor-test a b alt-tag conf-level)          ;; => test-result   (estimate = r)

;; Nonparametric tests.
(mann-whitney a b alt-tag)                 ;; => test-result   (statistic = U)
(wilcoxon-signed-rank a b alt-tag)         ;; => test-result   (statistic = W)

;; Kolmogorov-Smirnov.  one-sample: compare col to a continuous distribution.
(ks-test-1samp col d alt-tag)              ;; => test-result   (statistic = D)
(ks-test-2samp a b alt-tag)                ;; => test-result   (statistic = D)
```

---

### stats/regress

```turmeric
;; Ordinary least squares.
;; y = response column.
;; xs = cons list of predictor columns (any number).
;; names = cons list of :cstr, one per predictor (used for output naming).
;; intercept? = 0 to fit through origin, 1 to include intercept.
(ols y xs names intercept?)                ;; => result<lm-fit>

;; Frame variant: predictor and response columns come from a single frame.
;; formula is the trivial "y ~ x1 + x2 + ..." encoded as a list of column names;
;; no Wilkinson notation parsing in v0.1.0.
(ols-frame f response-name predictor-names intercept?)
                                           ;; => result<lm-fit>

;; Inspection.
(predict fit new-xs)                       ;; => column<:float>
(predict-frame fit new-frame)              ;; => result<column<:float>>

;; Diagnostics frame (one row per observation; columns: fitted, residual,
;; standardized-residual, leverage, cooks-d).
(diagnostics fit)                          ;; => frame
```

Implementation: normal equations `(X'X) beta = X'y` solved by Cholesky
factorization of `X'X`. Predictors must be linearly independent in v0.1.0; we
detect singularity by a tolerance on the Cholesky pivots and return an
`err "rank-deficient design matrix; remove a collinear predictor"`.

(Future work: QR-decomposition based solver for better numerical stability;
ridge / penalized variants. Tracked under `tur-stats` post-v0.)

---

### stats/sample

Resampling utilities. None of these mutate their inputs.

```turmeric
;; Bootstrap.  stat-fn : (fn [col :int] :float).
;; method = 0 (percentile) or 1 (BCa).
;; Returns a test-result with estimate = original-stat, ci-low/ci-high set.
(bootstrap col stat-fn n-reps conf-level method rng)
                                           ;; => test-result

;; Two-sample bootstrap difference (e.g. of means).
;; stat-fn : (fn [a :int b :int] :float).
(bootstrap-2samp a b stat-fn n-reps conf-level method rng)
                                           ;; => test-result

;; Permutation test: stat-fn returns a real-valued test statistic; we shuffle
;; group labels n-reps times and compute a p-value against the observed value.
(permutation-test a b stat-fn n-reps alt-tag rng)
                                           ;; => test-result

;; Train/test split on a frame.  Returns (cons train-frame test-frame).
;; stratify-col = nil for simple random split; else stratify within that column.
(train-test-split f test-frac stratify-col rng)
                                           ;; => result<list<frame>>

;; K-fold CV.  Returns a list of (cons train-indices test-indices) pairs;
;; each pair's lists together cover [0, nrows(f)).
(cv-folds n k shuffle? rng)                ;; => list<(cons list<:int> list<:int>)>
(cv-folds-stratified col k shuffle? rng)   ;; => list<(cons list<:int> list<:int>)>
```

---

### stats/fmt

```turmeric
;; Pretty-print a test-result, R/Python-style.
(print-test r)                             ;; => :void
(test->str r)                              ;; => :cstr

;; Pretty-print an lm-fit summary (coefs table + R^2 + sigma).
(print-fit f)                              ;; => :void
(fit->str f)                               ;; => :cstr

;; Coefficient table as a frame (name, estimate, se, t, p).  Useful for
;; downstream formatting via tur-frame's own pretty-printer.
(fit-coefs-frame f)                        ;; => frame
```

---

## Implementation phases

- [x] **ST0** -- `build.tur`; spice deps on `tur-frame`, `tur-math`; vendor
  `pcg32.h`; `stats/mathx` (erf/erfc/lgamma/tgamma/expm1/log1p);
  `stats/rng` (rng-make, rng-uniform, rng-uint32, rng-int-range, rng-shuffle!).

- [x] **ST1** -- `stats/summary`: col-* reductions through col-kurtosis; frame
  wrappers; `describe`; alignment with `tur-frame`'s describe (decide who owns).

- [x] **ST2** -- `stats/cov`: cov, cor, cor-spearman; cov-matrix, cor-matrix.

- [x] **ST3** -- `stats/dist`: dist-normal + dist-t + dist-chi2 + dist-f with
  pdf/cdf/quantile/random; continued-fraction implementations of the regularized
  incomplete beta and gamma functions; round-trip tests
  (quantile(cdf(x)) ~= x; rng samples converge to expected moments).

- [x] **ST4** -- `stats/dist` remainder: dist-uniform, dist-binomial,
  dist-poisson, dist-exponential, dist-beta, dist-gamma; top-level
  convenience wrappers (dnorm / pnorm / qnorm / rnorm and the rest).

- [x] **ST5** -- `stats/test`: t-tests (1-sample, 2-sample pooled/Welch,
  paired), var-test, cor-test; test-result struct; `stats/fmt` print-test;
  reproduce R's t.test output on Fisher's iris on at least three pairings.

- [x] **ST6** -- `stats/test` remainder: anova-oneway, chi2-gof,
  chi2-contingency, mann-whitney, wilcoxon-signed-rank, ks-test-1samp,
  ks-test-2samp.

- [x] **ST7** -- `stats/regress`: ols via Cholesky on X'X; predict;
  diagnostics; lm-fit struct + `stats/fmt` print-fit; reproduce R's lm()
  coefficients on Fisher's iris (Sepal.Length ~ Sepal.Width + Petal.Length)
  to at least 6 significant figures.

- [x] **ST8** -- `stats/sample`: bootstrap (percentile + BCa),
  bootstrap-2samp, permutation-test; train-test-split (simple + stratified);
  cv-folds + cv-folds-stratified.

- [x] **ST9** -- Tests pass on all CI targets; README in `turmeric-spices`;
  `docs/guides/stats-guide.md`; `stats-v0.1.0` tag.

---

## Design notes

### Why a separate spice and not part of tur-frame

`tur-frame` is about the *container* (storage, layout, dataflow). `tur-stats`
is about what you *do* with one. Splitting them keeps `tur-frame` small and
lets stats grow independently: a future `tur-stats-glm` or `tur-stats-ts` can
ship without bloating the dataframe core. It also lets non-stats spices
(`tur-plot`, `tur-sqlite` writers, `tur-frame-parquet`) depend on `tur-frame`
without dragging in distribution code.

### Why PCG32 over Mersenne Twister

PCG32 is ~80 lines, has known-good statistical quality, and supports cheap
multiple streams via the `inc` parameter (handy for parallel bootstrap). MT
needs 624-word state, is slower per draw, and has worse equidistribution.
PCG32 outputs 32 bits per draw; we combine two for 64-bit uses.

### Determinism

Every randomized routine (`random-n`, `bootstrap`, `permutation-test`,
`train-test-split`, `cv-folds*`, `rng-sample*`) takes an explicit `rng`
argument. There is no hidden global RNG. This makes tests reproducible and
parallel work safe: clone the rng with `rng-clone` for each worker.

### Null handling

Column reductions skip nulls by walking the validity bitmap. Tests and
regression operate on the *intersection* of non-null rows across all input
columns (R's `complete.cases` semantics) and report the resulting N in the
test-result / lm-fit struct so callers can audit drop rates.

### Numerical accuracy targets

For v0.1.0 we aim to match R / SciPy outputs to 6 significant figures on
representative datasets (iris, mtcars, a small AB-test simulation). Tests
encode this as a tolerance, and we explicitly call out any place we
deliberately differ (e.g. ANOVA Type-III sums of squares are not implemented;
Type-I only).

### Why no formula parser

A Wilkinson-style formula DSL ("y ~ x1 * x2 + I(x3^2)") is a substantial
parser and term-expansion algorithm. v0.1.0 takes predictor lists directly.
A future `tur-stats-formula` could grow the DSL on top, returning the same
column lists.

---

## Risks and open questions

1. **lm-fit owns columns by handle.** If a caller frees the source frame, the
   `fitted` / `residuals` columns in `lm-fit` remain valid (they are
   freshly-allocated columns), but `coefs` etc. need similar treatment.
   Document the ownership rule: every column on `lm-fit` is owned by the fit
   and freed with it.

2. **Continued-fraction stability.** The incomplete beta / gamma functions
   underpinning t / F / chi2 / beta / gamma CDFs are notoriously fiddly at
   extreme arguments. ST3 includes the standard cutover heuristics (use
   `1 - I_x(a,b)` when `x > (a+1)/(a+b+2)` etc.) and tests them against R.

3. **Bootstrap perf.** Naive bootstrap allocates `n_reps` columns of length
   `n`. For `n_reps = 10_000` and `n = 100_000` this is 4 GB at f32.
   v0.1.0 uses an in-place statistic loop (one allocation reused across
   replicates) and documents the memory characteristics.

4. **Connection to tur-frame `agg`.** Several reductions overlap with
   `tur-frame`'s `agg-mean`, `agg-sum`, etc. Resolution: the agg machinery
   stays in `tur-frame` (it's plumbing for group-by); `tur-stats` provides
   the *named, documented, null-aware* analytical entry points. Both call the
   same column-buffer kernels under the hood (kernels living in `tur-frame`).

---

## Shared work

### turmeric-spices README

Add row once v0.1.0 is tagged:

| Spice | Description | Tier | Depends on |
|-------|-------------|------|------------|
| `tur-stats` | Statistical analysis on dataframes (summary, distributions, tests, OLS) | 1 -- pure Turmeric | `tur-frame`, `tur-math` |

### Guide

Deliver `docs/guides/stats-guide.md` alongside the `v0.1.0` tag. Sections:

1. Summarizing a column (mean, sd, quantile, describe)
2. Comparing two groups (t-test, Mann-Whitney, bootstrap CI)
3. Correlation matrices and visualization (pair with `tur-plot`)
4. Fitting a linear model (`ols`, diagnostics, prediction)
5. Cross-validation patterns (`cv-folds`, `train-test-split`)
6. Seeding and reproducibility (`rng-make`, `rng-clone`)

### Integration notes

- Pair with `tur-frame` for I/O: read CSV -> `ols-frame` -> write predictions
  back with `with-col` -> `write-csv`.
- Pair with `tur-plot` for diagnostic plots: feed `diagnostics fit` columns
  into `points` / `function` for residual-vs-fitted and Q-Q plots.
- Pair with `tur-notebook` (planned) for reproducible analyses: the
  `rng-make seed` pattern makes a notebook's stats outputs byte-stable across
  re-evaluation.
