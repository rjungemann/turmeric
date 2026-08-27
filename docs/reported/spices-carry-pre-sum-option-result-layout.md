# (spice repo) Spices still speak the pre-sum Option/Result layout

**Severity: high for the spice repo (46 files across 12 spices), invisible to
this repo's CI.**  Filed 2026-08-27 at the SR2b/SR3 landing, from a read-only
sweep of `rjungemann/turmeric-spices` at its current head.

## Summary

SR2b converted stdlib `Option`/`Result` from discriminated records to real
sums with the tagged layout `{ int tag; union { ... } as; }` (tag 0 =
None/Ok, payload at offset 8).  The in-tree stdlib, fixtures, preamble
helpers, and interpreter were migrated in lockstep -- but the spices repo was
not, and it carries both failure modes:

- **126 inline-C sites in 34 files hand-roll the retired layouts**
  (`struct { bool is_ok; int64_t ok_val; int64_t err_val; }` /
  `{ bool is_some; int64_t value; }`).  By file count: tourist 14,
  notebook 10, httpd 7, tidal 5, tourist-session-valkey 2, plutovg 2,
  http 2, scscm 1, regex 1, postgres 1, plot 1.  These are **silent wrong
  bytes**, not compile errors: the old `is_ok = true` byte reads back
  through the new layout as `tag == 1` -- which is **Err** -- and the old
  24-byte Result's `err_val` sits at offset 16 where the new 16-byte union
  puts nothing.  An ok result built the old way reads as an error carrying
  garbage.
- **35 record-accessor reads** (`(.is-some x)` / `(.value x)` /
  `(.ok-val r)` / `(.err-val r)`) in tourist-session (27), regex (4),
  json (3), tourist-session-valkey (1).  These fields do not exist on the
  sum, so they are **clean elaboration errors** -- loud, at least.

Only 7 files already go through the preamble helpers and need nothing.

## Why this repo's CI cannot see it

The `requires.spices` fixtures auto-skip when the sibling
`../turmeric-spices/` checkout is absent, which it is on the CI runners that
validated SR2b.  A green turmeric suite says nothing about the spices.

## Fix directions

The migration is mechanical and has a complete worked example: turmeric
commit `5acef8b0` ("sr2b: convert stdlib Option/Result to real defdata
sums") migrated the same two patterns across the stdlib and ~40 fixtures.

- Hand-rolled structs: replace with the preamble helpers (`tur_box_ok` /
  `tur_box_err` / `tur_is_ok` / `tur_ok_value` / `tur_err_value`,
  `tur_box_some` / `tur_is_some` / `tur_opt_value`, `TUR_NONE`), which carry
  the canonical layout and a `_Static_assert` pinning it.  Where a helper
  does not fit, the layout to spell is `{ int tag; int64_t payload }` with
  tag 0 = Ok/None -- see
  `docs/guides/inline-c-results-guide.md`.
- Accessor reads: replace with `match` on the constructors, or the stdlib
  accessors (`some?` / `unwrap` / `ok?` / `ok-val` / `err-val`).
- `(none)` note: since SR3 slice A the carrier None IS the null pointer
  again, so spice code that already treated 0 as none (the pre-SR2b
  convention) is correct on the read side without changes.

## Verification

Clone the spices next to this repo
(`git clone https://github.com/rjungemann/turmeric-spices ../turmeric-spices`)
and run `bash tests/run.sh` so the `requires.spices` fixtures stop skipping;
then each spice's own test suite.
