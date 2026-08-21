# No stdlib cstr content-equality function

**Severity: low** -- guides and examples each hand-roll strcmp wrappers.
Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `stdlib/cstr.tur` exports `cstr-eq?` (and `cstr-free`).

## Repro

stdlib/cstr.tur exported only `cstr-len`/`cstr-nth`/`cstr-sub`;
examples/datalog/minimal.tur:157 and the cli-args guide defined local helpers.

## Correction to the original filing

The report said "`=` on cstr compares pointers". It does not -- `=` has **no
`cstr` overload at all**. `(= a b)` on two `cstr` values is a hard
`TUR-E0006` operator-lookup error, so there was never a silent
pointer-comparison footgun; there was simply no operator. Two guides repeated
the pointer-comparison claim in a code comment and have been corrected.

A second correction: `Eq[cstr]` already existed and was already a content
`strcmp` (stdlib/typeclass-eq.tur:35), and it is auto-loaded, so
`(eq? a b)` was a working content comparison the whole time. What was missing
was the direct, non-typeclass call sitting with the rest of the `cstr`
byte primitives.

## Resolution

`stdlib/cstr.tur` gained two exports:

- `cstr-eq? [a : cstr b : cstr] : bool` -- byte-wise content equality,
  NULL-safe on both sides (both NULL equal; NULL never equals non-NULL,
  including the empty string). Same comparison `Eq[cstr]` performs, reached
  without typeclass dispatch.
- `cstr-free [s : cstr] : void` -- found while writing the fixture.
  `cstr-sub` returns a malloc'd string and its docstring says "the caller
  frees the result", but `drop!` requires a `ref<T>` and rejects a bare
  `cstr`, so there was **no way to honor that contract from Turmeric** --
  every caller had to drop into inline C. Any fixture exercising `cstr-sub`
  under run.sh's LeakSanitizer therefore could not be written leak-clean.

## Tests

`tests/fixtures/cstr-eq-content` -- seven rows, each pairing a literal
against a freshly malloc'd `cstr-sub` result so the operands are always
distinct pointers: same-content, one-byte-diff, empty/empty, empty/non-empty,
proper prefix in both orders, and two literals. A pointer compare would report
every heap row false; a prefix compare would report abc/abcd true. Runs under
LSan, so the `cstr-free` calls are load-bearing.

The fixture carries `requires.compiled`: `stdlib/cstr.tur` is inline-C and the
tree-walker does not run inline-C. `run-turi.sh`'s `fixture_has_inline_c`
follows `(load "...")` but not `(import ...)`, so it would otherwise dispatch
the fixture to `--interpret`. Broadening that detector was considered and
rejected -- 7 fixtures import inline-C-carrying stdlib modules and interpret
correctly today because they never reach the inline-C, so following imports
would over-approximate and drop real coverage.

## Guides updated

- docs/guides/cli-args-guide.md -- both the s-expr and sweet-exp subcommand
  examples now call `cstr-eq?` instead of defining a local `cstr-same?`; the
  incorrect "`=` on cstr compares pointers" comment is gone, replaced by a
  note stating the actual `TUR-E0006` behavior and pointing at `eq?` as the
  no-import alternative.
- docs/guides/datalog-02-minimal-impl.md -- the tutorial's own `cstr-eq?`
  takes the erased `:int` storage handles, not `cstr`, so it now carries a
  callout that it is a different function from the stdlib export and that
  importing `cstr` into that file would collide.
- docs/guides/strings-guide.md -- the `cstr` tier note now states that `=`
  has no `cstr` overload, and names `eq?` / `cstr-eq?` as the two ways to
  compare content.

Regenerated `stdlib/docstrings.tur` and `docs/api/` via `tools/gendocs.py`.

## Not done here

The datalog examples were left untouched. Four of the five
(`blog`/`datalog`/`minimal`/`query`) already fail `tur check` on `main` for an
unrelated reason -- a `pred` callback typed as bare `:int` is then applied as
a function -- so renaming their local `cstr-eq?` inside already-broken files
could not be verified. Filed separately as
docs/reported/datalog-examples-do-not-compile.md.
