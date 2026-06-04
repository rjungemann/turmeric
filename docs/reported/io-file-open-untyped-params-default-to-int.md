# `io/file-open` (and friends) untyped params default to `:int`, rejecting `:cstr` paths

**Summary.** Several `stdlib/io.tur` functions declare their parameters with no
type annotation -- `(defn file-open [path mode] : FileHandle ...)`,
`(defn read-file [path] ...)`, `(defn write-file [path data len] ...)`,
`(defn file-exists? [path] ...)`. The checker resolves an unannotated parameter
to `:int`, so the documented string-path call fails to type-check:

```turmeric
(load "stdlib/io.tur")
(defn main [] : int
  (let [fh (file-open "/etc/hostname" "rb")]   ; TUR-E0001: arg 1 expected int, got cstr
    (file-close fh)))
```

**Severity.** Hard compile error that makes the public, linear-resource
`FileHandle` open path (`file-open`) unusable with a real `:cstr` path -- you
cannot open a file by name through the documented API. `read-file`,
`write-file`, and `file-exists?` have the same shape and are presumably equally
unusable with string paths. It is latent because no fixture loads
`stdlib/io.tur` and exercises these wrappers, so the suite stays green.

**Pre-existing.** Reproduces unchanged on `HEAD:stdlib/io.tur`
(`(defn file-open [path mode] : FileHandle ...)`); it is independent of the
opaque-handle-types work that surfaced it.

## Observed vs. expected

- Observed: `(file-open "/etc/hostname" "rb")` is a `TUR-E0001` -- arg 1
  `expected int, got cstr`.
- Expected: `path` is a `:cstr` and `mode` is a `:cstr`; the call type-checks
  and opens the file.

## Root cause

`stdlib/io.tur:314` declares `file-open` with bare parameters and an inline-C
body:

```turmeric
(defn file-open [path mode] : FileHandle
  ```c FileHandle fh;
  fh.ptr = (void*)fopen(path, mode);
  return fh;
  ```)
```

The inline-C body places no Turmeric-level constraint on `path`/`mode`, so they
are unconstrained type variables. Rather than being treated as fully
polymorphic, an unconstrained parameter is defaulted to `:int`, and the
`:cstr` argument then fails to unify. (By contrast, the freshly added
`file-stream-open [path : cstr mode : cstr]` annotates its parameters and works
with string paths.)

## Proposed fix

Annotate the parameter (and where useful, return) types on these inline-C
wrappers so they match their docstrings:

```turmeric
(defn file-open   [path : cstr mode : cstr] : FileHandle ...)
(defn read-file   [path : cstr] : ptr<void> ...)
(defn write-file  [path : cstr data : ptr<void> len : int] : int ...)
(defn file-exists? [path : cstr] : int ...)
```

(Confirm the intended `data` type for `write-file` against its callers before
settling on `ptr<void>` vs `cstr`.)

## How to validate a fix

1. The repro above must compile and run (open + close a real file by path).
2. Add a happy-path fixture under `tests/fixtures/` that loads `stdlib/io.tur`
   and round-trips a file via `file-open` / `file-read` / `file-close` (this
   would also have caught the original defect).
3. `bash tests/run.sh` stays green.

Until fixed, callers must use `file-stream-open` (the non-linear FileStream
path, which is properly `:cstr`-typed) or the `tur/fs` text helpers
(`fs/read-text` / `fs/write-text`), which take `:cstr` paths.
