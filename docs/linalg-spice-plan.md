# Spice Plan: tur-linalg

> **Status:** Draft Plan
> **Last Updated:** 2026-05-26
> **Type:** Spice Design

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-linalg` | `linalg-v0.1.0` | (none, pure Turmeric + inline-C) | Dense float linear algebra: matrices, vectors, decompositions |

`tur-linalg` is a self-contained spice providing `:float` dense matrices and
vectors with the standard operations needed across the Turmeric spice
ecosystem: arithmetic, decompositions (Cholesky, LU, QR), linear system
solvers, and small fixed-size graphics helpers (`vec2`/`vec3`/`vec4`,
`mat2`/`mat3`/`mat4`).

It is the shared numerical substrate that several other spices currently
work around in isolation:

- `tur-stats` embeds a Cholesky solver as inline-C in `stats/regress`. Once
  `tur-linalg` exists, that solver becomes a call to `linalg/decomp`.
- `tur-stats-multivariate` (planned) needs SVD for PCA and factor analysis.
- The OpenGL/graphics spices each carry their own ad-hoc `mat4` helpers;
  `tur-linalg` consolidates these into a single tested module.

`tur-linalg` has no cmake C dependency of its own. Inline-C is limited to:
- a thin flat-array allocator (reuses the same `malloc`/`free` pattern as
  `stdlib/sized-matrix.tur`),
- thin wrappers for `<math.h>` functions not already exposed by the stdlib
  (`sqrt`, `fabs`, `hypot` -- only if not provided by `tur-math`).

All numerical algorithms (matrix multiply, Cholesky, LU, QR, back-substitution,
Gram-Schmidt) are plain Turmeric operating on the flat buffer.

---

## Relationship to `stdlib/sized-matrix`

`stdlib/sized-matrix.tur` is **not replaced or modified**. It is:
- integer-only (`int64`)
- part of the sized-type (`#lang sized`) system
- aimed at compile-time shape assertions, not numerical computation

`tur-linalg` is orthogonal: it uses `:float` (64-bit), dynamic shapes, and
targets numerical workloads. The two types never need to interoperate.

---

## Scope

### In scope for v0.1.0

- **`linalg/mat`** -- dynamic `:float` matrix type (row-major), constructors,
  element access, arithmetic (add, sub, scale, mat-mul, transpose), norm,
  trace, identity, diagonal, copy, print.
- **`linalg/vec`** -- dynamic `:float` vector (column vector), dot product,
  outer product, norm, normalize, element-wise ops.
- **`linalg/decomp`** -- Cholesky (SPD systems), LU with partial pivoting
  (general square systems), QR via Householder reflections (least-squares /
  full-rank).
- **`linalg/solve`** -- forward/back substitution on triangular factors from
  decomp; `mat-solve A b` convenience wrapper; `mat-inv` via LU.
- **`linalg/small`** -- fixed-size concrete helpers: `vec2`, `vec3`, `vec4`,
  `mat2`, `mat3`, `mat4`; common operations (cross product, normalize,
  perspective, look-at, rotate-x/y/z, translate, scale transforms).

### Out of scope for v0.1.0

Tracked for follow-up; called out so the v0.1.0 API does not block them.

| Future work | What it adds |
|-------------|--------------|
| `tur-linalg` v0.2 | SVD, eigendecomposition (for PCA / `tur-stats-multivariate`) |
| `tur-linalg` v0.2 | Conjugate-gradient and iterative solvers for large sparse-ish systems |
| `tur-linalg` v0.2 | BLAS-level tiling / loop unrolling for large matrix multiply |
| `tur-linalg-sparse` | CSR / CSC sparse matrix type; sparse matrix-vector products |
| BLAS/LAPACK backend | Optional CMake dep; swap implementations behind the same API |

---

## Conventions

Standard spice layout:

```
spices/linalg/
  build.tur
  src/linalg/
    mat.tur       -- "linalg/mat"    dynamic float matrix type + arithmetic
    vec.tur       -- "linalg/vec"    dynamic float vector type + arithmetic
    decomp.tur    -- "linalg/decomp" Cholesky, LU, QR factorizations
    solve.tur     -- "linalg/solve"  triangular solvers, mat-solve, mat-inv
    small.tur     -- "linalg/small"  vec2/vec3/vec4, mat2/mat3/mat4 + transforms
    fmt.tur       -- "linalg/fmt"    pretty-printing for mat and vec
  tests/linalg/
    mat_test.tur
    vec_test.tur
    decomp_test.tur
    solve_test.tur
    small_test.tur
```

---

## Architecture

```
caller
  |
  v
linalg/mat       -- dynamic m x n float matrix; row-major flat buffer
linalg/vec       -- dynamic n float vector (column vector; thin wrapper on mat)
  |
  v
linalg/decomp    -- Cholesky / LU / QR; returns factor structs (not raw mats)
linalg/solve     -- back-sub / fwd-sub on factor structs; mat-solve convenience
  |
  v
linalg/small     -- fixed-size concrete helpers; wraps mat/vec for 2x2..4x4
linalg/fmt       -- print / str conversion for mat and vec
```

The `mat` type is the only heap object. All decompositions return factor
structs that hold references to their component matrices (L, U, R, Q, etc.);
freeing the factor frees the component matrices. `vec` is a 1-column `mat`
with a thin alias type so the API stays typed.

---

## Result types

```turmeric
;;; mat -- a heap-allocated row-major m x n float matrix.
(defstruct mat
  rows  :int      ;; number of rows
  cols  :int      ;; number of columns
  data  :int)     ;; :ptr to float64 array of rows*cols elements

;;; vec -- a 1-column mat; alias for column vectors.
(defstruct vec
  len   :int
  data  :int)     ;; :ptr to float64 array of len elements

;;; chol-factor -- result of a Cholesky decomposition A = L L'.
(defstruct chol-factor
  L     :int)     ;; lower-triangular mat

;;; lu-factor -- result of LU decomposition PA = LU.
(defstruct lu-factor
  L     :int      ;; lower-triangular mat (unit diagonal)
  U     :int      ;; upper-triangular mat
  piv   :int)     ;; permutation vector (cons list of :int)

;;; qr-factor -- result of QR decomposition A = QR.
(defstruct qr-factor
  Q     :int      ;; orthogonal mat (m x m)
  R     :int)     ;; upper-triangular mat (m x n)
```

---

## Modules and exports

### linalg/mat

```turmeric
;; Constructors.
(mat-new rows cols)                        ;; => mat  (uninitialized)
(mat-new-zeroed rows cols)                 ;; => mat  (zero-filled)
(mat-identity n)                           ;; => mat  (n x n identity)
(mat-diag v)                               ;; => mat  (diagonal from vec)
(mat-from-list rows cols data)             ;; => mat  (from cons list of :float, row-major)
(mat-copy m)                               ;; => mat  (deep copy)
(mat-free m)                               ;; => :void

;; Shape.
(mat-rows m)                               ;; => :int
(mat-cols m)                               ;; => :int
(mat-shape m)                              ;; => (cons rows cols)

;; Element access.
(mat-get m r c)                            ;; => :float  (bounds-checked)
(mat-set! m r c v)                         ;; => :void
(mat-row m r)                              ;; => vec   (copy of row r)
(mat-col m c)                              ;; => vec   (copy of col c)
(mat-submat m r0 c0 r1 c1)                 ;; => mat   (copy of submatrix)

;; Arithmetic (all return new mats; inputs unchanged).
(mat-add a b)                              ;; => mat   element-wise
(mat-sub a b)                              ;; => mat   element-wise
(mat-scale m s)                            ;; => mat   scalar multiply
(mat-mul a b)                              ;; => mat   matrix product
(mat-mul-vec m v)                          ;; => vec   matrix-vector product
(mat-transpose m)                          ;; => mat

;; Reductions.
(mat-trace m)                              ;; => :float  (sum of diagonal)
(mat-norm-fro m)                           ;; => :float  Frobenius norm
(mat-norm-max m)                           ;; => :float  max absolute element

;; Predicates.
(mat-square? m)                            ;; => :int  (1 if rows == cols)
(mat-approx-eq? a b tol)                   ;; => :int  (1 if max|a-b| < tol)
```

---

### linalg/vec

```turmeric
;; Constructors.
(vec-new n)                                ;; => vec  (uninitialized)
(vec-new-zeroed n)                         ;; => vec
(vec-from-list xs)                         ;; => vec  (from cons list of :float)
(vec-copy v)                               ;; => vec
(vec-free v)                               ;; => :void

;; Access.
(vec-get v i)                              ;; => :float
(vec-set! v i x)                           ;; => :void
(vec-len v)                                ;; => :int

;; Arithmetic.
(vec-add a b)                              ;; => vec
(vec-sub a b)                              ;; => vec
(vec-scale v s)                            ;; => vec
(vec-dot a b)                              ;; => :float
(vec-outer a b)                            ;; => mat  (outer product)

;; Norms.
(vec-norm v)                               ;; => :float  Euclidean (L2)
(vec-norm-1 v)                             ;; => :float  L1
(vec-norm-inf v)                           ;; => :float  L-infinity
(vec-normalize v)                          ;; => vec     (unit vector; error if zero)

;; Predicates.
(vec-approx-eq? a b tol)                   ;; => :int
```

---

### linalg/decomp

```turmeric
;; Cholesky -- A must be symmetric positive-definite.
;; Returns err if A is not SPD (detected by non-positive pivot).
(chol A)                                   ;; => result<chol-factor>
(chol-free f)                              ;; => :void

;; LU with partial (row) pivoting -- A must be square and non-singular.
;; Returns err if A is singular (zero pivot within tolerance).
(lu A)                                     ;; => result<lu-factor>
(lu-free f)                                ;; => :void

;; QR via Householder reflections -- A may be rectangular (m >= n).
(qr A)                                     ;; => qr-factor  (never fails)
(qr-free f)                                ;; => :void
```

All three decompositions operate on a copy of the input; the original `A` is
not modified.

---

### linalg/solve

```turmeric
;; Solve triangular systems.
(fwd-sub L b)                              ;; => vec   Lx = b  (L lower-triangular)
(back-sub U b)                             ;; => vec   Ux = b  (U upper-triangular)

;; Solve using a pre-computed factor (preferred when solving multiple rhs).
(chol-solve f b)                           ;; => vec   A x = b via L L'
(lu-solve f b)                             ;; => vec   A x = b via P L U
(qr-solve f b)                             ;; => vec   min ||Ax - b|| (least-squares)

;; Convenience -- factorize and solve in one call.
;; Chooses Cholesky if spd? = 1, LU otherwise.
(mat-solve A b spd?)                       ;; => result<vec>

;; Matrix inverse via LU (prefer mat-solve when possible; inverse is rarely needed).
(mat-inv A)                                ;; => result<mat>

;; Determinant via LU (product of U diagonal * pivot sign).
(mat-det A)                                ;; => result<:float>
```

---

### linalg/small

Fixed-size types for graphics and geometry. These are value-struct wrappers
backed by small stack-friendly float arrays -- not heap allocations.

```turmeric
;; vec2 / vec3 / vec4.
(vec2 x y)                                 ;; => vec2
(vec3 x y z)                               ;; => vec3
(vec4 x y z w)                             ;; => vec4
(vec3-dot a b)                             ;; => :float
(vec3-cross a b)                           ;; => vec3
(vec3-normalize v)                         ;; => vec3
(vec3-length v)                            ;; => :float

;; mat4 (column-major to match OpenGL convention).
(mat4-identity)                            ;; => mat4
(mat4-mul a b)                             ;; => mat4
(mat4-mul-vec4 m v)                        ;; => vec4
(mat4-transpose m)                         ;; => mat4
(mat4-inv m)                               ;; => mat4   (general 4x4 inverse)

;; Common transforms (return new mat4).
(mat4-translate v)                         ;; => mat4   v is vec3
(mat4-scale v)                             ;; => mat4   v is vec3
(mat4-rotate-x angle)                      ;; => mat4   angle in radians
(mat4-rotate-y angle)                      ;; => mat4
(mat4-rotate-z angle)                      ;; => mat4
(mat4-rotate axis angle)                   ;; => mat4   axis is vec3

;; Projection / view (OpenGL conventions: right-handed, NDC z in [-1,1]).
(mat4-perspective fovy aspect near far)    ;; => mat4
(mat4-ortho l r b t near far)              ;; => mat4
(mat4-look-at eye center up)               ;; => mat4   all vec3

;; Pointer helpers for passing to OpenGL uniform uploads.
(mat4-ptr m)                               ;; => :ptr   (stack pointer; valid for call duration)
(vec3-ptr v)                               ;; => :ptr
```

The `mat4` / `vec3` types here unify the ad-hoc definitions currently spread
across the OpenGL and GLSL spices.

---

### linalg/fmt

```turmeric
;; Print to stdout.
(mat-print m)                              ;; => :void  (aligned columns, 4 decimal places)
(vec-print v)                              ;; => :void

;; Convert to :cstr.
(mat->str m)                               ;; => :cstr
(vec->str v)                               ;; => :cstr

;; With precision control.
(mat-print-prec m prec)                    ;; => :void  prec = decimal places
(vec-print-prec v prec)                    ;; => :void
```

---

## Implementation phases

- [ ] **LA0** -- `build.tur`; spice scaffold; `linalg/mat` core: `mat-new`,
  `mat-new-zeroed`, `mat-identity`, `mat-from-list`, `mat-free`,
  `mat-get`, `mat-set!`, `mat-rows`, `mat-cols`; `linalg/vec` core:
  `vec-new`, `vec-from-list`, `vec-free`, `vec-get`, `vec-set!`, `vec-len`.

- [ ] **LA1** -- `linalg/mat` arithmetic: `mat-add`, `mat-sub`, `mat-scale`,
  `mat-mul`, `mat-mul-vec`, `mat-transpose`, `mat-trace`, `mat-norm-fro`,
  `mat-approx-eq?`; `linalg/vec` arithmetic: `vec-add`, `vec-sub`,
  `vec-scale`, `vec-dot`, `vec-outer`, `vec-norm`, `vec-normalize`.

- [ ] **LA2** -- `linalg/fmt`: `mat-print`, `vec-print`, `mat->str`, `vec->str`,
  with precision variants; all `mat_test.tur` and `vec_test.tur` pass.

- [ ] **LA3** -- `linalg/decomp`: `chol` (Cholesky-Banachiewicz algorithm);
  `linalg/solve`: `fwd-sub`, `back-sub`, `chol-solve`. Tests: solve a 3x3
  SPD system; verify `L * L' ~= A` to 12 significant figures.

- [ ] **LA4** -- `linalg/decomp`: `lu` with partial pivoting (Doolittle);
  `linalg/solve`: `lu-solve`, `mat-solve`, `mat-inv`, `mat-det`. Tests:
  solve a 4x4 general system; verify `P L U ~= A`; round-trip `A * inv(A)
  ~= I`.

- [ ] **LA5** -- `linalg/decomp`: `qr` via Householder reflections;
  `linalg/solve`: `qr-solve` (least-squares via R x = Q' b). Tests: full-rank
  overdetermined 6x3 system; verify `Q' Q ~= I`; verify `Q R ~= A`.

- [ ] **LA6** -- `linalg/small`: `vec2`, `vec3`, `vec4`, `mat4`; arithmetic,
  cross/dot/normalize; `mat4-translate/scale/rotate-x/y/z/rotate`; projection
  helpers (`mat4-perspective`, `mat4-ortho`, `mat4-look-at`); `mat4-ptr` /
  `vec3-ptr`. Tests reproduce the transform outputs of the OpenGL spice.

- [ ] **LA7** -- Migrate `tur-stats` `regress.tur` Cholesky inline-C to call
  `linalg/decomp chol` + `linalg/solve chol-solve`. Verify regression test
  suite still passes to the same 6 significant-figure tolerance.

- [ ] **LA8** -- Tests pass on all CI targets; README row in `turmeric-spices`;
  `docs/guides/linalg-guide.md`; `linalg-v0.1.0` tag.

---

## Design notes

### Why not wrap BLAS/LAPACK

BLAS/LAPACK are fast but heavy: they require a C/Fortran toolchain, a
system library or vendored source, and dramatically increase build complexity.
For the matrix sizes that appear in `tur-stats` (design matrices of tens to
hundreds of columns), a clean Turmeric implementation is fast enough and far
easier to audit. A future optional BLAS backend can slot in behind the same
API using a feature flag in `build.tur` for users who need throughput on
large matrices.

### Row-major vs column-major

`linalg/mat` uses row-major storage (consistent with `stdlib/sized-matrix`).
`linalg/small`'s `mat4` uses column-major to match OpenGL's uniform upload
convention; `mat4-ptr` returns a pointer ready to pass to `glUniformMatrix4fv`
with `transpose = GL_FALSE`. This distinction is documented on each type and
does not leak across module boundaries.

### Float precision

All `linalg/mat` and `linalg/vec` values are 64-bit floats (`double`).
`linalg/small` uses 32-bit floats for `vec2`/`vec3`/`vec4`/`mat4` because
GPU-side uniform uploads and standard graphics math are conventionally `float`.
The type tags make this explicit.

### Shared solver with tur-stats

Phase LA7 migrates the inline-C Cholesky from `stats/regress` to use
`linalg/decomp`. The migration is tested by running the existing `stats`
test suite unchanged -- the regression coefficients must match to the same
tolerance. This is the primary integration test for `tur-linalg`.

### No hidden allocation in small

`linalg/small` types are value structs on the stack. `mat4-ptr` returns a
raw pointer to the struct's array field -- it is valid for the duration of
the calling expression and must not be stored across a function boundary.
This matches the existing OpenGL spice convention.

---

## Risks and open questions

1. **Numerical stability of plain-Turmeric Cholesky/QR.** Textbook
   implementations in double precision are sufficient for the matrix sizes
   `tur-stats` encounters. For very ill-conditioned systems, the tolerance
   strategy (detect near-zero pivot, return `err`) is the safe fallback. We
   will document the condition number limitations.

2. **mat4-ptr lifetime.** Because `mat4` is stack-allocated, its `ptr` is
   only valid inside the expression that calls `mat4-ptr`. This is the same
   footgun that exists in the OpenGL spice today; document it clearly and
   add a test that exercises the expected usage pattern.

3. **Migration risk in tur-stats LA7.** The existing inline-C Cholesky in
   `stats/regress` has been tested against R's `lm()` to 6 significant
   figures. The `tur-linalg` Cholesky must reproduce the same results to that
   tolerance. LA7 is blocked on LA3 completing and is the primary integration
   gate before the v0.1.0 tag.

4. **Ownership model for factor structs.** `chol-factor`, `lu-factor`, and
   `qr-factor` own their component matrices. Callers must call `chol-free` /
   `lu-free` / `qr-free` to release memory. Document the rule and add
   contract assertions that the factor was not already freed (using a sentinel
   field).

---

## Shared work

### turmeric-spices README

Add row once v0.1.0 is tagged:

| Spice | Description | Tier | Depends on |
|-------|-------------|------|------------|
| `tur-linalg` | Dense float linear algebra: matrices, vectors, Cholesky/LU/QR solvers, mat4 graphics helpers | 1 -- pure Turmeric + inline-C | (none) |

### Guide

Deliver `docs/guides/linalg-guide.md` alongside the `linalg-v0.1.0` tag. Sections:

1. Creating and manipulating matrices (`mat-new`, `mat-from-list`, `mat-mul`)
2. Solving linear systems (`mat-solve`, when to use each decomposition)
3. Least-squares fitting with QR (`qr-solve`)
4. Graphics transforms (`mat4-perspective`, `mat4-look-at`, `mat4-rotate`)
5. Memory model (ownership of mats, factor structs, stack lifetime of `mat4`)
6. Numerical accuracy and limitations

### Integration notes

- **`tur-stats`**: replace inline-C Cholesky in `stats/regress` with
  `(import linalg/decomp :refer [chol])` + `(import linalg/solve :refer
  [chol-solve])`. QR path unlocked for future `tur-stats` v0.2 ridge solver.
- **`tur-stats-multivariate`** (planned): SVD and eigendecomposition added in
  `tur-linalg` v0.2 unblock PCA and factor analysis.
- **OpenGL spice**: deprecate per-spice `mat4` helpers; import from
  `linalg/small` instead. Migration is non-breaking (same function names,
  identical semantics).
