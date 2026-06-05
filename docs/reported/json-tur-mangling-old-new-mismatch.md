---
title: stdlib/json.tur: inline-C calls use new injective mangling but defn emits old fold
severity: high (silent breakage of every fixture that loads stdlib/json.tur)
status: open
reported: 2026-06-05
related-pr: "#275 Make Turmeric->C name mangling injective and reversible"
related-commit: 77e73c9e
---

# `stdlib/json.tur` mangling mismatch -- inline-C calls vs. emitted defn names

## Summary

After PR #275 introduced an injective Turmeric-to-C mangling
(`-` -> `_hy`, `_` -> `_un`, `/` -> `_sl`), `stdlib/json.tur` was
hand-edited so its inline-C bodies call the *new* mangled names
(e.g. `json_hyenc_hyappend_hyc_hy`), but the compiler still emits the
corresponding `defn` definitions under the *old* fold
(e.g. `json_enc_append_c_`). Every fixture that transitively loads
`stdlib/json.tur` -- which includes `stdlib/httpd.tur` and therefore
every `httpd-*` and `httpd-async-*` fixture -- fails to compile with
`call to undeclared function 'json_hyenc_hyappend_hyc_hy'`.

## Observed vs. expected

**Observed** (cc errors against `/tmp/tur-build/tests_fixtures_httpd-mw-compress_input_tur.c`):

```
... error: call to undeclared function 'json_hyenc_hyappend_hyc_hy'; ...
 json_hyenc_hyappend_hyc_hy(b, (int64_t)'"');
```

with the actual definition in the same translation unit emitted as:

```c
static void json_enc_append_c_(void *, int64_t);   // line ~2732 (proto)
static void json_enc_append_c_(void * b, int64_t c) { ... }   // line ~4942
```

**Expected**: either all defn definitions emit under the injective scheme
(`json_hyenc_hyappend_hyc_hy`), making the inline-C call sites resolve,
or the stdlib inline-C bodies use the old fold names (`json_enc_append_c_`)
matching what the compiler emits. The two schemes must not coexist in one
translation unit.

## Minimal repro

```sh
./build/tur build tests/fixtures/httpd-mw-compress/input.tur -o /tmp/out
```

Any other httpd fixture reproduces it too, e.g.:

```sh
./build/tur build tests/fixtures/httpd-async-echo/input.tur -o /tmp/out
```

Both abort with `cc invocation failed` and a wall of
`call to undeclared function 'json_hyenc_hy...'` errors.

`bash tests/run.sh` shows the cascade -- dozens of unrelated fixtures
(reactor-*, httpd-*, etc.) report `build failed` because their transitive
import of `stdlib/json.tur` (via `stdlib/httpd.tur` etc.) drags in the
broken inline-C.

## Root-cause analysis

PR #275's release notes say:

> Globals are injective; module prefix preserved (`geom__vector__add2`).
> Compiler-synthesized `__`-prefixed pure-C-id names (`__fn_N`,
> `__inst_*`, internal stdlib helpers) emit verbatim; `extern-c` names
> use the legacy fold consistently (prototype, call sites, inline-C).

`json-enc-append-c-`, `json-enc-str-`, `json-enc-node-` etc. (`stdlib/json.tur`
lines 395, 404, 414, 422, 435 -- all trailing-hyphen names) are emitted as
`json_enc_append_c_` etc. in the .c file -- the **old fold**, not the new
injective form. Yet the *inline-C bodies* in the same file already use the
injective names (`json_hyenc_hyappend_hyc_hy`, ~28 occurrences in
`stdlib/json.tur`). The source-of-truth disagreement lives between
`src/mangle.c` (or whatever path emits these defns) and the manual edits
that landed in `stdlib/json.tur` as part of #275.

Possible causes worth checking:

1. The mangler path that emits `defn`-derived prototypes/definitions still
   hits the legacy fold for symbols matching `[a-z0-9-]+` with a trailing
   `-` (perhaps the "internal stdlib helper" carve-out is over-broad and
   captures these).
2. The hand-edits to `stdlib/json.tur` pre-computed mangled names
   incorrectly -- but those calls match the documented `-` -> `_hy`
   rule, so this seems less likely.
3. A second emission path (forward declarations vs. function bodies)
   uses a different mangler.

## Proposed fix directions

- **Option A (preferred)**: make the global-defn emission path use the
  same injective mangler as inline-C splices and `__TUR_CNAME_<name>__`,
  so `json-enc-append-c-` consistently becomes `json_hyenc_hyappend_hyc_hy`
  everywhere. Verify by grepping the emitted .c -- a single defn name
  must appear once as prototype and once as definition with no fold-form
  duplicate.
- **Option B (fallback)**: revert the inline-C bodies in `stdlib/json.tur`
  to use the legacy fold names, matching what the compiler currently
  emits. Less correct since it perpetuates the very collision PR #275
  set out to fix, but unblocks the test suite immediately.
- **Option C**: convert the inline-C call sites to the
  `__TUR_CNAME_json-enc-append-c-__` splice form so the mangler picks
  the right encoding regardless of which scheme is canonical at a given
  point. Robust against future mangling-scheme changes.

In any fix, the validation is the same: `bash tests/run.sh 2>&1 | grep "^FAIL"`
should be empty (currently dozens of `build failed` rows trace back to
this), and the dedicated httpd-mw-compress runner (currently absent --
the fixture carries `requires.dedicated-runner` but no matching ctest
target exists, a separate gap) should build.

## Sibling observation while triaging

`stdlib/httpd.tur:3228` references `(tnil? mws)`, but `tnil?` lives in
`stdlib/list.tur` which `httpd.tur` does not load. Fixed in this session
by replacing with `(= mws 0)` (matching the `(= mw 0)` style four lines
below). This is unrelated to the json mangling bug but was the *first*
error masking it; mentioned here only so future readers don't chase the
same red herring.

## Validation checklist for a fix

- [ ] `./build/tur build tests/fixtures/httpd-mw-compress/input.tur -o /tmp/out` succeeds
- [ ] `./build/tur build tests/fixtures/httpd-async-echo/input.tur -o /tmp/out` succeeds
- [ ] `bash tests/run.sh 2>&1 | grep "^FAIL"` is empty
- [ ] Grep the emitted .c: no symbol appears in both fold form
      (`json_enc_append_c_`) and injective form
      (`json_hyenc_hyappend_hyc_hy`) within the same translation unit
