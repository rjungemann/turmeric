---
title: CONV-S6 -- Struct/ADT Convergence Diagnostic Wording Pass
category: Planning
description: One-pass rewording of struct construction, field access, pattern match, and `with` diagnostics so that "struct" and "variant" wording line up with the unified struct-as-single-variant-ADT model. Best done after StructDef retirement settles.
---

# CONV-S6 -- Diagnostic Wording Pass

## Status

**RESOLVED 2026-07-02.** Executed against the post-StructDef-retirement
codebase. By that point the "two physical emission sites per code" the plan
anticipated had already collapsed to a single unified path (`make-struct`
rewrites to the record-ADT constructor call), so S6-2's consolidation reduced
to introducing one shared surface-classifier helper
(`conv_surface_is_struct` / `conv_surface_phrase` in `elab_structs.c`,
declared in `elab_internal.h`) that both the constructor-call path
(`elab_call.c`) and the `with` path (`elab_structs.c`) call. `AdtDef` already
carried the origin flag (`from_struct_lowering`), so S6-1 needed no new field.
All wording (S6-3), surface-coverage fixtures (S6-4), and `tur explain`
entries for TUR-E0292/E0293/E0294/E0296/E0297/E0298/E0299 (S6-5) landed. Full
suite green. TUR-E0298 left unchanged per the open question.

Split out from the parent `struct-adt-convergence-plan.md` (archived
2026-06-28). Deferred until [`structdef-retirement-plan.md`](structdef-retirement-plan.md)
lands -- doing the wording pass before the retirement means doing it twice
(once against the dual-path internals, once against the unified ones).

## Problem

After CONV-S1's lowering and CONV-S0's record-style variants landed,
diagnostics on product-shaped data come from a mix of code paths and the
wording has drifted:

| Code | Where it lives | Today's wording | Issue |
|---|---|---|---|
| TUR-E0292 | [`elab_call.c:2133`](../../src/compiler/elab_call.c), [`elab_structs.c:4655`](../../src/compiler/elab_structs.c) | "missing field" (variant ctor) / "make-struct missing field" | two surfaces for one error |
| TUR-E0293 | both files | "duplicate field" (variant) / "make-struct duplicate field" | same |
| TUR-E0294 | both files | "unknown field" (variant) / "make-struct unknown field" | same |
| TUR-E0296 | [`elab_structs.c:4840,4988`](../../src/compiler/elab_structs.c) | "with requires a :copy type -- '%s' is move-only" (ADT) / "with requires a :copy struct" (struct) | same code, different wording |
| TUR-E0297 | both | "with unknown field '%s' for variant '%s'" / "for struct '%s'" | same |
| TUR-E0298 | both | "with duplicate override field '%s'" / same | identical -- safe |
| TUR-E0299 | [`elab_call.c:2077,2103`](../../src/compiler/elab_call.c), [`elab_structs.c:4623,4667`](../../src/compiler/elab_structs.c) | "cannot mix positional and keyword" (4 sites, 2 wordings) | same |

For a `defstruct`-originated lowered ADT, the message says "variant" -- which
is technically correct but reads oddly when the user wrote `defstruct`. For
a hand-written `defadt`, the message saying "struct" is wrong outright.

## Design principle

**Mention the surface the user actually wrote.** When the type was declared
`defstruct Foo`, say "struct Foo". When it was declared `defadt Foo (Foo
[...])`, say "variant Foo of type Foo". The compiler already records which
keyword introduced the type (it must, to print the right `make-struct` vs
constructor-call ergonomics in other diagnostics); thread it through to the
field-error site.

For the lowered-from-struct case the unified internal representation does
not lose this information -- it is a single `bool defstruct_origin` (or
equivalent) on the `AdtDef`. Verify the field exists on `AdtDef` in
[`types.h:271`](../../src/compiler/types.h); if not, add it as part of
S6-1 below.

## Tasks

### S6-1 -- Audit and tag the origin

- Confirm `AdtDef` carries (or gain a) `bool from_defstruct` field. Set it
  in the elaboration boundary that lowers `defstruct` to `defadt`. Verify
  no existing call site assumes it is unset.
- Audit every emission of TUR-E0290-TUR-E0299 in
  [`src/compiler/elab_structs.c`](../../src/compiler/elab_structs.c),
  [`src/compiler/elab_call.c`](../../src/compiler/elab_call.c), and
  [`src/compiler/elab_types.c`](../../src/compiler/elab_types.c). For each
  site, decide: keep "struct" wording (single-variant from-defstruct),
  use "variant" wording (everywhere else), or branch on `from_defstruct`.

### S6-2 -- Consolidate the duplicated emission sites

Each of TUR-E0292 / E0293 / E0294 / E0296 / E0297 / E0299 has two physical
emission sites (the struct path and the ADT path). Replace both with a
single helper, e.g.

```c
static void emit_field_error(DiagCode code, Span sp,
                             const AdtDef *def, const CtorDef *ctor,
                             const char *fname);
```

that picks "struct %s" vs "variant %s of type %s" based on
`def->from_defstruct && def->n_ctors == 1`. The helper lives in
`elab_structs.c` and is called from both the make-struct path and the
constructor-call path. **No new diagnostic codes** -- this is a wording
pass, not a vocabulary expansion.

### S6-3 -- Reword

For each code, the unified text:

- **TUR-E0292** -- "missing field '%s' in {struct|variant} '%s'%s"
  (trailing `%s` is " of type X" for the variant case, empty for struct).
- **TUR-E0293** -- "duplicate field '%s' in {struct|variant} '%s'%s
  construction".
- **TUR-E0294** -- "unknown field '%s' on {struct|variant} '%s'%s".
- **TUR-E0296** -- "with requires a :copy {struct|variant} -- '%s' is
  move-only; declare it `(def{struct|adt} %s :copy ...)` to use with."
- **TUR-E0297** -- "with: unknown field '%s' on {struct|variant} '%s'%s".
- **TUR-E0299** -- "{struct|variant} '%s' construction: cannot mix
  positional and keyword arguments".

The two `with` codes (E0296, E0297) keep the actionable hint -- removing
"declare it :copy" would regress the messages.

### S6-4 -- Fixture coverage

Diagnostics live or die by their snapshots. The error-fixture directory
already covers most happy/sad paths; add or update:

- `tests/fixtures/errors/conv-field-error-on-struct/` -- a `defstruct`
  triggers TUR-E0292/E0294. Expects "struct Foo" wording.
- `tests/fixtures/errors/conv-field-error-on-adt-variant/` -- a record
  variant of a multi-variant `defadt` triggers the same codes. Expects
  "variant Foo of type Shape" wording.
- `tests/fixtures/errors/conv-field-error-on-single-variant-adt/` --
  hand-written `(defadt Foo (Foo [...]))` (not lowered from defstruct).
  Expects "variant Foo of type Foo" wording (no special-case to "struct"
  for this case; the user picked `defadt` for a reason).
- `tests/fixtures/errors/conv-with-copy-error-on-struct/` and
  `.../conv-with-copy-error-on-adt/` -- TUR-E0296 wording in both
  surfaces.
- `tests/fixtures/errors/conv-mixed-args-on-struct/` and
  `.../conv-mixed-args-on-adt-variant/` -- TUR-E0299 in both surfaces.

`stderr.expected` files are the source of truth here. When the wording is
finalised, regenerate snapshots in the same commit -- per the project rule
("Fixture churn is not a deferral reason").

### S6-5 -- Cross-link in the diagnostic explanation pages

[`src/compiler/diag.c`](../../src/compiler/diag.c) carries multi-line
explanations for many codes. Update the explanations for E0292-E0299 to
reflect the unified vocabulary, with one example block per surface
(`defstruct` and `defadt`). Existing examples should not be removed --
add the second surface alongside.

## Non-tasks

- **No new diagnostic codes.** A wording pass that adds new codes is no
  longer a wording pass.
- **No change to TUR-E0290 / E0291** -- those are about typed-row literal
  syntax, not product-shape construction. They live in `elab_types.c` and
  are out of scope.
- **No translation/i18n.** Diagnostics remain English-only.

## Order of work

1. **S6-1** -- audit + add `from_defstruct` if missing. (One commit.)
2. **S6-2** -- introduce `emit_field_error` helper, route both sites
   through it. Snapshots should not move yet -- the helper produces the
   pre-existing wording per site. (One commit; no fixture churn expected.)
3. **S6-3** -- swap to the unified wording. (One commit; expect fixture
   snapshot churn; regenerate in the same commit.)
4. **S6-4** -- add the surface-coverage fixtures listed above.
5. **S6-5** -- diag.c explanation updates. (One commit; no test impact.)

Gates on `bash tests/run.sh` with a 10-minute timeout, per the project
rule.

## Risks

- **Snapshot churn surprise.** Even the helper-extraction step (S6-2) can
  shift snapshots if the previous duplicated wordings differed by a comma
  or word. The expected scope is small; verify before claiming "no churn"
  for that step.
- **`from_defstruct` lifetime.** If StructDef retirement is mid-flight
  when this lands, the field may move; coordinate timing so the audit
  happens against the *post-retirement* AdtDef shape.
- **Third-party tooling consuming TUR-Exxxx strings.** None known.
  Anything that grep-matches the wording will break; the codes are the
  stable contract, the wording is not.

## Open questions

- **Should TUR-E0298 be reworded for symmetry?** Today it is identical in
  both surfaces ("with duplicate override field"). The "variant" wording
  is fine on a struct because override fields are already field-named. Lean
  no -- leave it alone unless S6-2's audit turns up a reason.
- **Hint wording on E0294 (unknown field).** Suggest the closest-match
  field name (Levenshtein-1)? Out of scope here; mention as a follow-up
  if the audit shows the wording is short on signal.
