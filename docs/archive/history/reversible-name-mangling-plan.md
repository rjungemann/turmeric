---
title: Reversible (Injective) Name Mangling
category: Planning
description: Make Turmeric's Turmeric-name -> C-identifier mangling injective and self-delimiting by giving the structural separators `_`, `-`, `/` distinct mnemonic escapes (`_un`, `_hy`, plus `__` reserved as the only structural separator), instead of folding them all to a single `_`. Closes the latent `foo-bar` / `foo_bar` -> `foo_bar` collision, unifies the four divergent per-site manglers onto one scheme, and enables a real demangler. Builds on the A3 sigil-mnemonic work in `src/compiler/mangle.c`.
---

# Reversible (Injective) Name Mangling -- Plan

## Why

`src/compiler/mangle.c` already gives operator sigils distinct two-letter
mnemonics (`>` -> `_gt`, `<` -> `_lt`, `+` -> `_pl`, ...) so symmetric pairs
like `>>>` / `<<<` no longer collide. But it deliberately **folds** the three
structural separators to a single `_`:

```c
} else if (c == '-' || c == '/') {
    dst[k++] = '_';            /* legacy fold */
} else if (... alnum or '_' ...) {
    dst[k++] = (char)c;        /* '_' passes through unchanged */
}
```

This fold is **not injective**: distinct Turmeric names map to the same C
identifier. The collision is real and reachable today:

```turmeric
(defn foo-bar [] : int 1)
(defn foo_bar [] : int 2)   ;; both mangle to C `foo_bar`
```

`tur build` fails with `error: redefinition of 'foo_bar'` -- two legal,
distinct top-level names cannot coexist. The same hazard exists for any
kebab/snake pair (`a-b` vs `a_b`), and across module separators (`a/b` vs
`a__b`). It is a hard `cc` error in the lucky case and a silent shadow in the
unlucky one (e.g. when only one is referenced, or when the duplicate is a
forward decl the C compiler merges).

The header already documents the limitation and the reason it was punted:

> Because '-', '/', and a literal '_' all map to '_' (legacy folding, kept so
> the hundreds of existing kebab/namespaced names keep their C spelling), the
> encoding is not self-delimiting ... A sound inverse would require re-encoding
> '-'/'/'/'_' too, which the project deliberately avoids (see plan A3).

This plan does that re-encoding. It was the intended end state all along; A3
shipped the sigil half and left the separator half as a follow-up.

### Secondary payoffs

- **A real demangler becomes possible.** Today diagnostics dodge the problem by
  always reporting the *source* symbol (from the AST/span). An injective
  encoding additionally lets tooling (ABI traces, `nm` output, crash
  backtraces, the spice loader manifest) recover the original name from a C
  symbol -- useful for `--emit-abi-trace`, profilers, and debuggers.
- **One scheme, one source of truth.** Four separate per-site manglers
  currently re-implement the lossy fold by hand
  (`tur_mangle_append`, `mangle_field_name`, `mangle_dynvar_name`,
  `mangle_mod_basename`, plus the inline `isalnum` loops for type-suffix
  components). They drift. Unifying them removes a class of "this site mangles
  differently from that site" bugs.

## The scheme

Injective, self-delimiting, and backward-compatible for the common case of
pure-alphanumeric names.

| Source byte | Encoding | Notes |
|-------------|----------|-------|
| `[A-Za-z0-9]` | itself | unchanged |
| `_` (literal underscore) | `_un` | **was** passthrough |
| `-` (hyphen) | `_hy` | **was** `_` |
| `/` (namespace sep, if encoded as a byte) | `_sl` | **was** `_` |
| operator sigils (`>` `<` `+` `=` `?` `!` ...) | `_` + 2-letter mnemonic | unchanged from A3 |
| any other byte | `_xHH` | unchanged hex escape |
| **`__` (double underscore)** | **reserved structural separator** | module-path and prefix boundaries only |

Key invariants that make it invertible:

1. **A single `_` always introduces an escape.** After re-encoding literal `_`
   as `_un`, a lone `_` in the output is never data -- it is always the first
   byte of a `_xx` mnemonic or `_xHH` hex escape.
2. **`__` is exclusively structural.** Because a literal underscore is `_un`,
   two adjacent underscores can never arise from data, so `__` is free to mean
   "module/prefix boundary" (the existing `geom/vector` -> `geom__vector__`
   convention keeps working unchanged).
3. **Mnemonics are two lowercase letters, none starting with `x`.** `x` is
   reserved for the hex escape, so the demangler disambiguates `_xHH` from a
   mnemonic by a single lookahead. (`un`, `hy`, `sl` are added; verify they do
   not clash with the existing set `ex qu lt gt eq pl st pc am ba cr td dl at
   do cl qt hs cm sc` -- they do not.)

Demangling (new capability): scan left to right; copy alnum; on `__` emit the
structural separator; on `_` read the next byte -- `x` -> consume two hex
digits -> byte; otherwise consume two letters -> reverse-mnemonic lookup.

### Worked examples

| Turmeric name | Today (lossy) | This plan (injective) |
|---------------|---------------|------------------------|
| `foo-bar` | `foo_bar` | `foo_hybar` |
| `foo_bar` | `foo_bar` (collision!) | `foo_unbar` |
| `eq?` | `eq_` (pre-A3) / `eq_qu` (A3) | `eq_qu` (unchanged) |
| `>>>` | `___` (pre-A3) / `_gt_gt_gt` | `_gt_gt_gt` (unchanged) |
| `geom/vector` `add2` | `geom__vector__add2` | `geom__vector__add2` (unchanged) |
| `list->vec` | `list__vec` | `list_hy_gtvec` |

Note the last row: `list->vec` today mangles to `list__vec`, which **collides
with a hypothetical module-qualified `list/vec`**. The injective scheme
separates them.

## Scope

In scope:

- Rewrite `tur_mangle_append` in `src/compiler/mangle.c` to the injective
  scheme; add `tur_demangle` (the inverse) and unit tests.
- Unify the divergent manglers (`mangle_field_name`, `mangle_dynvar_name`, the
  type-suffix `isalnum` loops in `elab_typeclasses.c` / `emit_core.c`) onto the
  shared mangler, OR consciously document any that must stay different.
- Decide and implement the module/file-name mangling story
  (`mangle_mod_basename` in `main.c`, the `__` separator, header/impl
  basenames).
- Audit every hand-written C reference that assumes a specific mangled spelling
  (runtime, builtins, `wasm_glue.c`, `spice_loader.c`, preamble `__TUR_*`
  tokens, FFI/`extern-c` exports).
- Regenerate all codegen snapshots; full suite green.
- A collision-regression fixture (`foo-bar` and `foo_bar` coexist) and a
  demangler round-trip unit test.

Out of scope:

- Changing the *source* surface syntax of identifiers. This is purely the
  C-emission spelling.
- Reworking diagnostics to *prefer* demangled names -- they already report
  source spelling. The demangler is added as a capability; wiring it into ABI
  traces / tooling is a follow-up.
- The inline-C substitution tokens (`__TUR_VAL_N__`, `__TUR_CAP_N__`,
  `__TUR_TY_T__`) -- these are fixed compiler-internal placeholders, not
  mangled user names; they are unaffected.

## Tasks

### T1. Lock the scheme and the collision table

Write down the final byte -> encoding table (above), the `__`-as-separator
rule, and the demangle algorithm. Enumerate the reserved mnemonics and assert
no duplicates. Produce a fixture-style table of >=20 names with their
before/after spellings, including every separator and a few sigil mixes, to
serve as the unit-test oracle.

### T2. Implement in `src/compiler/mangle.c`

1. Re-encode `_` -> `_un`, `-` -> `_hy`, `/` -> `_sl` (decide whether `/`
   reaches `tur_mangle_append` at all, or is always pre-split into `__`
   structural separators by the module layer -- see T5).
2. Keep alnum passthrough and the existing sigil mnemonics + `_xHH` hex escape.
3. Update `tur_mangle_bound` if the worst case grows (it stays 4x: `_xHH`).
4. Add `size_t tur_demangle(const char *mangled, char *out, size_t cap)` and a
   reverse mnemonic table.
5. Add a standalone unit test (wire into the existing `tur_*_unit` ctest
   targets) asserting (a) the T1 table round-trips mangle->demangle, and
   (b) injectivity on the table (no two distinct inputs share an output).

### T3. Unify the divergent manglers

Route these onto `tur_mangle_ident` / `tur_mangle_append`:

- `mangle_field_name` (`emit_core.c:788`) -- struct field / ctor names.
- `mangle_dynvar_name` (`emit_core.c:766`) -- dynvar names (after stripping the
  `*earmuffs*`).
- The `isalnum`-fold loops building type-suffix components in
  `elab_typeclasses.c` (lines ~1929, 1951, 2575) and `emit_core.c` (~1719,
  1731). These spell instance *type* components (e.g. `_int`, `_arrow`, ctor
  names); fold them through the shared mangler so a struct/ADT named with a
  hyphen cannot collide.

For each, confirm the field-access *read* site uses the same helper (e.g.
`.field` access must mangle the same way as the field *declaration*). This is
the same struct-def-vs-call-site consistency the A3 work established for
typeclass dicts.

### T4. Audit hand-written C references (highest risk)

Grep the runtime and glue layers for any literal that bakes in a mangled
spelling and would break when the spelling changes:

- `src/runtime/*.c`, `src/compiler/builtins.c` -- runtime functions are mostly
  already valid C identifiers (`tur_hamt_new`) and are *not* produced by
  mangling, so they are safe; confirm none are referenced *by mangled name*.
- `src/wasm_glue.c` / `web/` exports (`turi_doc_lookup`, Emscripten
  `EXPORTED_FUNCTIONS`) -- these export by exact C name; verify none depend on a
  mangled stdlib spelling.
- `src/turi/spice_loader.c` -- parses `<mod>/<name> -> <mangled> :: ...` from a
  spice manifest. The `<mangled>` token must be produced by the *same* mangler
  the AOT build uses. If manifests are generated, regenerate; if hand-written,
  update. This is the most likely breakage point for `../turmeric-spices`.
- `extern-c` / FFI: any stdlib symbol deliberately exported under a stable C
  name. If such a name contained `-`/`_`, its spelling changes -- decide
  whether those need an explicit stable-name attribute (a future `#[c-name]`
  escape hatch) rather than silent re-spelling.

Deliverable: a checklist of every reference found and its disposition (safe /
updated / needs stable-name).

### T5. Module / file-name mangling decision

`mangle_mod_basename` (`main.c:3001`) maps `/` -> `__`, `-` -> `_`, other ->
`_` for header/impl **file** base names, and the binding C-name prefix uses the
same `module__name__binding` shape. Two coupled decisions:

1. **Binding prefix** (C symbol): keep `__` as the structural separator (it
   composes cleanly with the injective scheme, since data can never produce
   `__`). A module component containing `-` now encodes as `_hy` inside the
   component, so `my-mod/fn` -> `my_hymod__fn` instead of today's
   `my_mod__fn`. This is the desired de-collision.
2. **File base name** (on-disk `.c`/`.h`): these are filesystem names, not C
   identifiers; injectivity matters less and over-long `_hy`/`_xHH` names hurt
   readability. Option A: keep `mangle_mod_basename` as-is (lossy is fine for
   filenames, which are not linked-against). Option B: unify for consistency.
   Recommendation: **Option A** -- filenames stay legacy-lossy, only *symbol*
   names go injective. Document the split.

### T6. Regenerate snapshots + full suite

1. Rebuild `tur`.
2. Regenerate every `tests/fixtures/*/expected.c` per the CLAUDE.md loop. This
   is a large mechanical diff (every kebab/namespaced identifier re-spells:
   `list_concat` -> `list_hyconcat`, `geom__vector__add2` unchanged, etc.).
3. Update any `expected.stderr` / `expected.diag` that embeds a mangled name
   (e.g. `tests/fixtures/emit-abi-trace`).
4. `bash tests/run.sh` -- zero `FAIL`.

### T7. Regression fixtures

- `mangle-kebab-snake-coexist/` -- defines both `foo-bar` and `foo_bar` (and
  calls both); asserts distinct results. This must FAIL to compile on `main`
  today and PASS after.
- `mangle-arrow-name-vs-module/` -- `list->vec` and a `list/vec` module member
  coexist without collision.
- Demangler round-trip unit test from T2.

### T8. Docs

- Rewrite the scheme section of `src/compiler/mangle.h` (it currently explains
  *why no demangler exists*; replace with the injective scheme + demangler).
- Add `docs/guides/name-mangling-guide.md` (or a section in an existing
  compiler-internals guide) with the table and the `__`-separator rule.
- If `../turmeric-spices` manifests are affected, note the regeneration step in
  the spice developer guide.

## Blast radius

- **Every** `expected.c` snapshot changes (any identifier with `_`/`-`/`/`).
  Expect a diff comparable to the A3 regeneration but larger (separators are far
  more common than sigils). Mechanical and reviewable as a rename.
- **Spices** (`../turmeric-spices`) are the main external risk via the spice
  loader manifest (T4). Coordinate: the AOT mangler and the manifest generator
  must move together.
- Linker-visible symbol churn: anything that linked against a stdlib symbol by
  its old mangled name breaks. The audit (T4) must be exhaustive before merge.

## Migration & rollback

- Land behind no flag -- mangling is a pure compile-time spelling and the suite
  is the gate. (A `-Xlegacy-mangle` flag is possible but doubles the snapshot
  matrix; only add it if an external consumer needs a transition window.)
- Rollback is reverting the `mangle.c` rewrite + regenerating snapshots; the
  injective scheme touches no semantics, only spelling.

## Validation

- `bash tests/run.sh` -- zero `FAIL`.
- T2 unit test: round-trip + injectivity on the T1 table.
- T7 collision fixtures: compile and produce distinct results.
- `nm build/tur`-style spot check: no two distinct stdlib source names share a
  C symbol (script: mangle every stdlib top-level name, assert the set size
  equals the name count).
- Spice smoke test (if `../turmeric-spices` present): a spice with a
  kebab-named export still loads via the manifest.

## Acceptance checklist

- [ ] Scheme + collision table locked (T1); reserved mnemonics asserted unique.
- [ ] `tur_mangle_append` injective; `tur_demangle` added; unit test green.
- [ ] Field / dynvar / type-suffix manglers unified onto the shared scheme (T3).
- [ ] Hand-written C reference audit complete with dispositions (T4).
- [ ] Module prefix vs filename decision recorded and implemented (T5).
- [ ] All `expected.c` + affected `expected.stderr` regenerated; suite green.
- [ ] `foo-bar` / `foo_bar` coexistence fixture passes (failed on `main`).
- [ ] `mangle.h` doc + mangling guide rewritten.
- [ ] Spice manifest path verified (or explicitly N/A).

## Cross-references

- **Builds on** the A3 sigil-mnemonic work in `src/compiler/mangle.c` (see
  [stdlib-arrow-typeclass-reintroduction-plan](stdlib-arrow-typeclass-reintroduction-plan.md),
  "Complete A3 operator mangling" commit) -- this is the deferred separator half.
- **Supersedes** the "No general demangler is provided" note in
  `src/compiler/mangle.h`.
