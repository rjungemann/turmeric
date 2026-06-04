# Inline-C blocks hardcode mangled C identifiers of sigil-named defns

**Summary:** Several stdlib inline-C blocks call a sibling `defn` by spelling
out its *mangled* C identifier as a string literal in the C body (e.g.
`tur_int_carrier_eq_`, `httpd_resp_body_`). This silently couples stdlib source
to the exact name-mangling scheme: any change to how `?`/`!`/operator chars are
mangled breaks these references with no compiler-level warning until the C
compiler fails (`implicit declaration of function ...`).

**Severity:** Ergonomics gap / latent fragility (medium). Not a miscompile --
it surfaces as a hard `cc` error -- but it is invisible to the Turmeric type
checker and easy to miss because the references live inside opaque ` ```c `
fences. It also created a near-miss: the only signal that
`httpd-mw-cors-is-preflight` calls the *predicate* `httpd-req-header?` (and not
the value accessor `httpd-req-header`) was the trailing `_` in the hand-written
mangled name.

## Repro (pre-fix)

Change the mangling of `?` from `_` to `_qu` (plan A3). `stdlib/sym.tur:79`:

```turmeric
(mk-cmp [x] : int ```c return (int64_t)(intptr_t)tur_int_carrier_eq_; ```)
```

still names `tur_int_carrier_eq_`, but `tur-int-carrier-eq?` now emits
`tur_int_carrier_eq_qu`, so:

```
error: 'tur_int_carrier_eq_' undeclared; did you mean 'tur_int_carrier_eq_qu'?
```

Observed: build fails at the C stage. Expected: a reference from inline-C to a
Turmeric `defn` should not need to know the mangling, or should be checked.

## Affected sites (all updated in the A3 change)

- `stdlib/sym.tur:79` -- `tur_int_carrier_eq_`
- `stdlib/map.tur:341,346,353,360,367` -- `tur_int_carrier_eq_`,
  `tur_cstr_key_eq_`, `tur_f32_carrier_eq_`, `tur_f64_carrier_eq_`
- `stdlib/httpd.tur` (many) -- `httpd_req_header_`, `httpd_resp_status_`,
  `httpd_resp_body_`, `httpd_resp_header_`, `httpd_resp_header_add_`,
  `httpd_set_attr_`
- `stdlib/httpd-compress.tur:49,64,65,66` -- same `httpd_*` family

## Root cause

Inline-C is emitted verbatim; the compiler does not parse C identifiers inside
` ```c ` blocks, so a call to another defn must be written as that defn's final
C symbol. There is no surface syntax to say "the C name of `tur-int-carrier-eq?`"
from within inline-C, so authors hardcode the mangled spelling. The mangling is
an internal implementation detail (`src/compiler/mangle.c`), so the two drift
independently.

## Proposed fix directions

1. **Name-reference splice for inline-C.** Allow a templated form inside inline-C
   that expands to the mangled C name of a referenced binding, e.g.
   `@{c-name tur-int-carrier-eq?}`, lowered by the emitter through the same
   `tur_mangle_append` helper. Eliminates the hardcoding entirely.
2. **Stable `export-as` aliases.** Give these carrier/accessor helpers an
   explicit `(export-as "...")` C name (Phase M6) and reference that fixed alias
   from inline-C, decoupling them from the auto-mangler.
3. **Lint pass.** A check that scans inline-C bodies for tokens that look like
   mangled identifiers of known sigil-named defns and warns when they do not
   match the current mangling.

Direction (1) is the most general and is the natural home for the existing
`tur_mangle_append` helper.

## Validation

`bash tests/run.sh` (full fixture suite, leak-checked) must stay at zero `FAIL`
after any mangling change. A more targeted guard: a fixture importing `httpd`
and `map` that exercises a carrier comparator and a CORS/static middleware,
which is already covered by `tests/fixtures/httpd-mw-*` and the `eqmap-*`
fixtures -- those caught every stale reference during the A3 change.
