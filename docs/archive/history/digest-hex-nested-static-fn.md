# digest/*-hex inline-C uses `static` nested functions -- won't compile

**Status:** RESOLVED 2026-07-20. The per-block SHA-256 / MD5 compression
functions are hoisted to file-scope Turmeric defns (`digest/sha256-transform!`,
`digest/md5-transform!`) and called from the four digest bodies via
`__TUR_CNAME_...__`, so the emitted C carries no nested functions and compiles on
a standard/clang toolchain. Verified against the FIPS/RFC test vectors
(SHA-256/MD5 of `""` and `"abc"`). Regression fixtures: `tests/fixtures/digest-hex`
and `tests/fixtures/digest-string`. This unblocked the `digest/*-string` owned
siblings (`stdlib/digest-string.tur`).

**Severity:** medium (the `digest/sha256-hex` / `digest/md5-hex` hex-string API
is uncompilable on a standard/GNU C compiler; no fixture covers it, so it went
unnoticed).

## Summary

`stdlib/digest.tur`'s inline-C bodies for the hex-digest functions declare their
SHA-256 / MD5 transform helpers as **`static` nested functions** (a function
defined inside another function body, with `static` storage class). GNU C allows
nested functions as an extension but does **not** allow the `static` qualifier on
them; ISO C forbids nested functions outright. `cc` rejects them with:

```
error: invalid storage class for function 'sha256_transform'
error: invalid storage class for function 'sha256_transform_h'
error: invalid storage class for function 'md5_transform'
error: invalid storage class for function 'md5_transform_h'
```

## Repro

```turmeric
(load "stdlib/digest.tur")
(load "stdlib/io.tur")
(defn demo [] : int (do (println (digest/sha256-hex 0 0)) 0))
(demo)
```

`tur run` (or `tur build`) fails at the `cc` stage with the errors above. No
fixture under `tests/fixtures/` exercises `sha256-hex` / `md5-hex`, which is why
the suite is green despite this.

## Root cause

`stdlib/digest.tur`:
- `digest/sha256` (~line 26): `static void sha256_transform(...)` nested in the body.
- `digest/sha256-hex` (line 94): `static void sha256_transform_h(...)` nested.
- `digest/md5` and `digest/md5-hex` (~lines 160 / 236): `static void md5_transform*(...)` nested.

Each is a function-local (nested) definition carrying `static` -- the illegal
combination.

## Fix directions

Hoist the transform helpers to **file scope** so they are ordinary `static`
translation-unit functions, not nested ones. In Turmeric inline-C, a helper can
be lifted out of the `defn` body via a file-scope `extern-c`-backed helper or by
emitting the transform once at module scope (e.g. a dedicated
`digest/sha256-transform` defn whose body is the transform, called from both
`digest/sha256` and `digest/sha256-hex`). Removing the `static` keyword alone is
not enough under ISO C (nested functions are still non-standard); the definitions
need to move out of the enclosing function body.

## Impact on other work

Blocks the `digest/sha256-string` / `digest/md5-string` owned-String siblings
planned in `docs/upcoming/v2/string-adoption-stdlib-plan.md` (item 4): those wrap
`digest/*-hex`, which must compile first. Once this is fixed, the digest sibling
module is a trivial two-function `string/adopt-cstr` wrapper.
