# `tur fmt` still deletes comments in `handle`/`case` arm gaps and in `^mut` binding pairs

**Severity: medium** -- the same silent-source-deletion defect as
[fmt-drops-comments-inside-bracket-vectors](../archive/fmt-drops-comments-inside-bracket-vectors.md),
in the printers that fix did not reach. Downgraded from HIGH because the
surviving cases are narrower: 8 files in a 2997-file sweep, versus 70 before
that fix.

## Repro

```sh
# a comment inside a handle arm
diff <(./build/tur fmt --stdout tests/fixtures/effect-reopen/input.tur) \
     tests/fixtures/effect-reopen/input.tur

# a `^mut` binding pair, plus a trailing comment on the last body form
diff <(./build/tur fmt --stdout tests/fixtures/borrow-deref/input.tur) \
     tests/fixtures/borrow-deref/input.tur
```

`effect-reopen` loses `;; Inner handler handles Log; its case body re-opens
Write.` -- a comment sitting between a `handle` form's scrutinee and its first
arm. `borrow-deref` loses `; deref &mut int -> 0` and additionally reformats

```turmeric
(let [^mut y 0] ...)
```

as

```turmeric
(let [^mut y
      0] ...)
```

splitting a binding name from its value, which the house style forbids
(CLAUDE.md, "Binding vectors").

## The full residue

A two-pass `tur fmt` over 2997 files (`stdlib/`, `tests/fixtures/`,
`examples/`, `tutorials/`) after the bracket-vector fix:

| File | symptom |
| --- | --- |
| `tests/fixtures/borrow-{deref,sugar,basic,mut-assign,reborrow,defer,through-deref}/input.tur` | `^mut` split + trailing comment lost; several non-idempotent |
| `tests/fixtures/errors/borrow-{invariance,conflict}/input.tur` | non-idempotent |
| `tests/fixtures/effect-reopen/input.tur` | `handle` arm-gap comment lost |
| `tests/fixtures/errors/ecs-defsystem-writes-unauthorized/input.tur` | one comment lost |
| `tests/fixtures/closure-field-boxed-all-shapes/input.tur` | one comment lost |
| `examples/guestbook/src/main.tur` | `; Allocate out-params for the next request.` lost |

Every one of these reproduced before the bracket-vector fix as well -- none is
a regression from it.

## Root cause

Identical in shape to the archived report, in printers that fix did not touch.
Two families:

**1. Header/arm loops that never consult the source gaps.** `fmt_body_forms`
(`src/compiler/fmt.c`) re-emits gap comments between the forms it lays out, and
after the bracket-vector fix so do `fmt_vec_broken`, `fmt_vec_params_broken`,
`fmt_vec_let_bindings_broken`, `fmt_map_broken`, `fmt_set_broken`, `fmt_call`'s
head/first-arg pair, and `fmt_cond`'s arm loop. The remaining fixed-index
header loops do not:

- `fmt_handle` -- items 0..1 (`handle` + scrutinee)
- `fmt_case` -- its arm loop
- `fmt_defclass` / `fmt_definstance` -- items 0..2
- `fmt_defn` / `fmt_fn` -- the signature items
- `fmt_if` / `fmt_when` / `fmt_do` -- their header items
- `fmt_defpackage` -- its keyword/value loop
- `fmt_map_block` -- its entry loop

Each walks `f->as.list.items[i]` with `fmt_form` and writes a literal `' '`
between them, so any comment in the gap has nowhere to go.

**2. `fmt_vec_let_bindings_broken` treats `^mut` as a binding name.** It walks
the vector two elements at a time assuming `[name value name value ...]`. A
leading modifier (`^mut`, `^fat`, `^linear`, `&`) is a third element, so the
pairing desynchronises: `[^mut y 0]` is read as name=`^mut`, value=`y`, then
name=`0`. `fmt_vec_params_broken` already has `param_is_leading_modifier` for
exactly this; the binding printer needs the same test, skipping the modifier
into the name slot rather than consuming a pair position.

Note this second one is a *layout* bug, not data loss -- but it is what makes
the borrow fixtures non-idempotent, and it is in the same function the archived
fix touched, so it is cheapest to fix alongside.

## Fix direction

The helpers the bracket-vector fix added are the whole toolkit -- this is
mechanical, not design work:

1. For each printer in family (1), replace the `fs_putc(s, ' ')` between header
   items with the pattern `fmt_call` now uses:

   ```c
   if (emit_gap_comments_before(s, prev_end, cur, body_col)) {
       fs_newline_indent(s, body_col);
   } else {
       fs_putc(s, ' ');
   }
   ```

   tracking `prev_end = cur->span.off_end` across the loop. `fmt_case`'s and
   `fmt_defpackage`'s pair loops take the two-slot version `fmt_cond` uses.

2. In `fmt_vec_let_bindings_broken`, consume a `param_is_leading_modifier`
   element into the *name* slot (emit it plus a space, then fall through to the
   name) instead of letting it take a pair position. Do the same in the
   `max_name` width pre-pass so the value column still lines up.

3. Extend the sweep in the archived report to a harness case, so the next
   printer added does not reintroduce this: a `tests/run-fmt.sh` case that
   two-pass formats a file with a comment in a `handle` arm gap and a `^mut`
   binding, asserting comment count is preserved and pass 2 == pass 1.

## Adjacent, not filed here

Two things noticed in the same sweep, both out of scope:

- The `borrow-*` fixtures contain non-ASCII characters (`&int -- immutable
  borrow`, `deref &int -> 42` are written with an em dash and an arrow glyph),
  which CLAUDE.md's "Fixture Files" rule forbids.
- `tur fmt` rewrites `@r` to `(deref r)`. That is a desugaring, not comment
  loss, but it means the formatter is not source-preserving for the `@` borrow
  sugar -- worth confirming it is intentional.

## Provenance

Found while fixing
[fmt-drops-comments-inside-bracket-vectors](../archive/fmt-drops-comments-inside-bracket-vectors.md),
by re-running that report's corpus sweep before and after (70 -> 8 files losing
comments, 9 -> 7 non-idempotent). This report is the 8.
