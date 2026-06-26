# Curly-Infix in the Default Turmeric Dialect

## Goal

Make curly-infix arithmetic -- `{a + b}` reads as `(+ a b)`, SRFI-105
semantics -- available in plain s-expression Turmeric (no `#lang`
directive, no `.sweet` extension), so every file gets the same
visual-precedence sugar that sweet-exp files already have.

The blocker today is a hard either/or in the reader: bare `{...}` is
already the syntax for **contract type annotations** (`{var : T | pred}`),
and curly-infix is gated by `r->curly_infix_enabled`, which only flips
on in sweet-exp / `--lang curly-infix` / `--lang sweet-exp`. The two
syntaxes cannot coexist on the same delimiter.

This plan resolves that by **moving contract types to `#refine{...}`**,
freeing bare `{...}` for curly-infix in every dialect.

## Motivation

- Curly-infix is the highest-leverage piece of sweet-exp syntax for a
  Lisp -- it's the one bit of sugar that meaningfully improves
  arithmetic readability without changing program shape elsewhere.
- Today, an s-expr file that wants `{w * h}` has to opt into the full
  sweet-exp dialect, which also changes indentation semantics, neoteric
  call syntax, and `$` rest-of-line. That's a heavy switch for one
  feature.
- Contract types are a small, identifiable corner of the surface area
  (~30 sites in stdlib + fixtures + docs, roughly counted by
  `grep -rn '{[^{}]*|[^{}]*}'` filtered to the `var : T | pred`
  shape). Migrating them is a one-time cost; the syntactic dividend is
  permanent.
- `#refine{...}` fits the existing `#foo{...}` family alongside
  `#map{...}`, `#set{...}`, `#row{...}`, and `#r{...}`. It introduces
  no new dispatch character, no new lookahead, no content sniffing.

## Scope

In scope:

- Add a `#refine{var : T | pred}` reader path that produces the same
  `F_CONTRACT_TYPE` form that `read_contract_type` produces today.
- Make bare `{...}` always dispatch to `read_curly_infix` (delete the
  contract-type fallback at `src/compiler/reader.c:2816`).
- Set `curly_infix_enabled = true` in the default dialect (and keep it
  on in the sweet-exp / curly-infix / neoteric dialects).
- Migrate every existing contract-type site in the tree from
  `{var : T | pred}` to `#refine{var : T | pred}`.
- Update docs (`docs/guides/`, CLAUDE.md sweet-exp section, any
  contract-type reference pages).
- Update `tools/gendocs.py` if it pattern-matches on contract syntax
  for rendering.

Out of scope (deferred):

- Brace-neoteric `f{x}` -> `(f x)` in the default dialect. Separate plan;
  this one is strictly about infix.
- Paren-neoteric `f(x)` -> `(f x)`. Same.
- Curly-infix precedence tables. SRFI-105 says mixed-operator forms
  fall back to `(nfx ...)`; we adopt that as-is and don't try to
  introduce a precedence layer.
- Renaming `F_CONTRACT_TYPE` internally. Keep the form kind; only the
  reader entry point changes.

## Design

### Reader change

In `src/compiler/reader.c`:

1. Add a `read_refine_data_literal(Reader *r)` entry that:
   - Expects `#refine{` already peeked by the dispatch layer.
   - Consumes `#refine`, asserts `{`, and then runs the same body loop
     `read_contract_type` runs today.
   - Returns the same `F_CONTRACT_TYPE` form (via `form_contract_type`).
2. Wire `#refine{` into the existing `#`-prefix dispatch alongside
   `#map{`, `#set{`, `#row{`, `#r{` (the run at `reader.c:1066-1082`).
3. Delete the bare-`{` -> `read_contract_type` branch at
   `reader.c:2816-2819`. The function `read_contract_type` itself stays
   (called from `read_refine_data_literal`); only the dispatch path
   changes.
4. Flip `r->curly_infix_enabled = true` unconditionally in the default
   dialect initialization (`reader.c:3725` area).

### Diagnostics

- Bare `{...}` that looks contract-shaped (contains `:` and `|` at top
  level) but isn't `#refine{...}` should emit a hint:
  `TUR-W####: this looks like a contract type; did you mean #refine{...}?`
  Implementation: after `read_curly_infix` fails with a mixed-operator
  or unexpected-`|` error, run a one-pass scan of the original token
  stream for the `sym : type | expr` pattern and append the hint to
  the diagnostic. This is best-effort and only fires on the error
  path, so it doesn't slow valid curly-infix.
- `#refine{...}` with the wrong inner shape reuses the existing
  contract-type diagnostics from `elab_types.c`. No new error codes.

### Elaboration

Zero elaboration change. `F_CONTRACT_TYPE` is the same form kind it is
today; every downstream consumer (`elab_types.c`, `elab_fns.c`,
`elab_macros.c`, `elab_toplevel.c`, `interp.c`, `pkg.c`, `forms.c`)
keeps working unchanged.

### Migration of existing contract-type sites

Estimated ~30 sites across `stdlib/`, `tests/fixtures/`, and `docs/`,
based on `grep -rn '{[^{}]*|[^{}]*}'` filtered to the `var : T | pred`
shape. The exact count is small enough to migrate in one PR.

Migration is mechanical:

```
{var : T | pred}   ->   #refine{var : T | pred}
```

Do **not** use blind sed -- per `feedback_no_global_rename`, bulk
character rewrites are forbidden in this codebase. The migration
script must:

1. Parse each `.tur` file with the existing reader (in the legacy
   bare-`{` mode, via a temporary flag) and identify only the
   `F_CONTRACT_TYPE` spans.
2. Emit a span-by-span patch list (file + offset + replacement).
3. Apply with a review step -- human-readable diff, not a regex sweep.

Docs and markdown get a separate, smaller pass; those are easy to
eyeball because contract-type examples are localized to the
contract-type guide(s).

### Backwards compatibility

Strictly breaking for any program that uses bare `{var : T | pred}`.
There's no plausible transitional dialect that accepts both forms
without bringing back the content-sniff hazard we explicitly rejected
earlier. The migration ships atomically with the reader change in one
PR.

For external consumers (none today, per the project memory note that
turmeric has a single maintainer and no external consumers), the
migration script doubles as a codemod they could run against their own
tree. Ship it in `tools/` even though we don't expect callers.

## Implementation Steps

1. **Reader plumbing.** Add `read_refine_data_literal`. Wire
   `#refine{` into the `#`-prefix dispatch.
2. **Migration tool.** Write `tools/migrate-contract-types.py` (or
   `.tur`, if preferred) that walks a directory, parses each `.tur`
   file, finds `F_CONTRACT_TYPE` spans in bare-brace position, and
   emits a `diff` to stdout. Add `--apply` for in-place rewrite.
3. **Run the migration** against `stdlib/`, `tests/fixtures/`, and
   `docs/`. Review the diff. Commit the rewrites in their own commit
   (separate from the reader change) so the reader-flip commit is the
   one that gates the behavior.
4. **Flip the reader.** Delete the bare-`{` -> contract-type fallback.
   Set `curly_infix_enabled = true` by default. Add the
   "did you mean `#refine`?" hint.
5. **Snapshot regen.** `bash tests/run.sh` with `timeout: 600000` to
   surface fixture churn; regenerate snapshots per the
   "Fixture Snapshots" rule in CLAUDE.md. Commit alongside the reader
   change.
6. **Docs.** This is a first-class implementation task, not a
   follow-up. Every file below must be updated in the same PR as the
   reader flip, and every example block has to round-trip through the
   migrated reader before merge.

   Required updates:

   - `docs/guides/contract-types-guide.md` -- rewrite every example
     and the page intro to use `#refine{...}`. Add a one-paragraph
     "Why `#refine`?" section explaining that bare `{...}` is now
     curly-infix and contract types moved to a `#`-prefixed literal
     alongside `#map`, `#set`, `#row`, `#r`.
   - `docs/guides/advanced-type-system-rationale.md` -- replace every
     contract-type example.
   - `docs/guides/union-intersection-types-guide.md` -- replace any
     refined-type examples.
   - `docs/guides/type-annotations-guide.md` -- if it shows refined
     types, migrate them; add a cross-reference to `#refine{...}`.
   - Any other `docs/guides/*-types-guide.md` that shows a refined
     type. Sweep with
     `grep -rln '{[a-z_]\+ : [A-Za-z0-9<>]\+ |' docs/guides/`
     before declaring the sweep complete (the sweep returns empty
     today, but re-run it after migration to confirm no source has
     since added a new bare-brace example).
   - `docs/guides/reader-forms-guide.md` -- document `#refine{...}` in
     the data-literal section; document bare `{...}` as curly-infix in
     the expression-syntax section; cross-link them so a reader hitting
     one finds the other.
   - `docs/guides/sweet-exp-style.md` and CLAUDE.md's
     "Sweet Expression Style" section -- note that curly-infix is no
     longer sweet-only. Remove any claim that "`{a + b}` requires
     `#lang sweet-exp`."
   - `docs/guides/compiler-flags-guide.md` -- if `--lang curly-infix`
     is listed, mark it as a no-op alias for the default dialect (it
     remains accepted for backwards compatibility).
   - `README.md` -- if the language tour or feature list mentions
     contract types or sweet-exp curly-infix, update both sections.
   - `tools/gendocs.py` -- audit for any pattern-match on the bare
     contract-type shape; switch to recognizing `#refine{...}` if it
     renders contract types specially. Regenerate `docs/html/` via
     `tur run docs` and commit the regenerated HTML in the same PR.

   Acceptance gate for this step: a final
   `grep -rn '{[a-z_]\+ : [A-Za-z0-9<>]\+ |' docs/ README.md`
   returns zero hits, and every modified `docs/guides/*.md` page has at
   least one `#refine{...}` example.

Order matters: migration commit lands first, reader-flip commit lands
second. Bisects then point cleanly at the reader-flip if anything
regresses.

## Testing

New fixtures under `tests/fixtures/curly-infix-default/`:

- `basic-add/` -- `{1 + 2}` -> `3`.
- `mixed-vars/` -- `(let [w 10 h 20] {w * h})` -> `200`.
- `nested/` -- `{ {a + b} * c }` -> `(* (+ a b) c)`.
- `mixed-op-falls-to-nfx/` -- `{a + b * c}` lowers to `(nfx a + b * c)`
  per SRFI-105 (assuming `read_curly_infix` already does this; verify).
- `refine-basic/` -- `#refine{n : int | (> n 0)}` parses identically to
  the old `{n : int | (> n 0)}`. Compare `emit-c` output before and
  after the migration (snapshot pinned to the new form).
- `refine-in-defn/` -- a `defn` signature using `#refine` in arg and
  return position.
- `did-you-mean-hint/` -- bare `{n : int | (> n 0)}` triggers the
  warning suggesting `#refine`.

Regression: `bash tests/run.sh` with `timeout: 600000`. Expect snapshot
churn limited to fixtures that exercised contract types in bare form
(now migrated) -- the codegen output is identical, so the diff is
read-side only.

Targeted unit: a small reader test that confirms
`r->curly_infix_enabled` is `true` after default-dialect setup.

## Risks & Open Questions

- **Migration coverage.** The structural-parse migration tool is more
  work than a regex sweep, but is the only safe way per
  `feedback_no_global_rename`. Acceptable cost for a one-time PR.
- **The "did you mean" hint** is a UX courtesy, not a correctness gate.
  If it turns out to be noisy (false positives on legitimate
  curly-infix that happens to contain `|`-shaped operators), drop it.
- **`#r{...}` neighbor.** `#refine{` and `#r{` share a prefix; the
  existing dispatch already handles longest-match correctly (see
  `reader.c:1066-1082` where `#row`, `#map`, `#set`, `#r` are peeked
  in order). Confirm `#refine` is matched before `#r`.
- **Documentation discoverability.** Contract types are a less-known
  feature today; moving them to a `#`-prefixed form might make them
  *more* discoverable (it's grep-able by `#refine`) but it also shifts
  every existing tutorial example. Update the docs alongside.
- **No experiment flag.** This is a breaking syntax change, but it's
  reader-local, small, mechanically migratable, and the project has a
  single maintainer with no external consumers. Ship in one PR with
  the migration, snapshots, and docs together. No
  `--enable=curly-infix-default` gate needed.

## Rollout

Two commits, one PR:

1. **Migration commit.** Mechanical rewrite of every bare-brace
   contract-type to `#refine{...}` across `stdlib/`, `tests/fixtures/`,
   and `docs/`. Zero behavior change at this point -- both forms still
   parse, the new form is just being adopted first.

2. **Reader-flip commit.** Remove the bare-brace contract-type
   fallback, default `curly_infix_enabled = true`, regenerate
   snapshots, update docs. Bisect-friendly: any post-flip regression
   points cleanly at this commit.
