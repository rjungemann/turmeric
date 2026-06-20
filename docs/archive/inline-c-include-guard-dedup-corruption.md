---
title: file-scope inline-C dedup corrupts include guards (drops repeated `#endif`)
category: Codegen -- file-scope inline-C block deduplication
severity: Medium. Any module-prelude inline-C block carrying two or more
  preprocessor include guards (the documented "redeclare the carrier struct in
  every module that touches its fields" idiom) emits malformed C: the 2nd+
  identical `#endif` is silently dropped, so cc fails with
  `unterminated #ifndef`. Blocked the `stats` spice's `tur build` outright and
  is latent for every multi-guard prelude.
status: RESOLVED
---

## Symptom

`tur build <dir>` (and single-file `tur emit-c`/`tur run`) on a spice whose
inline-C prelude carries multiple include guards produced malformed C:

```
build/obj/stats__rng.c:257: error: unterminated #ifndef
build/obj/stats__cov.c:230: error: unterminated #ifndef
...
```

and, in the whole-program path, paired `#endif without #if` plus
`redefinition of pcg32_srandom_r` (two copies of a guarded block emitted with
only one `#ifndef`).

## Root cause

`inline_c_split_chunks` / `inline_c_emit_block_deduped` in
`src/compiler/emit_module.c` split a file-scope inline-C block into per-directive
and per-declaration "chunks", then dedup each chunk against a whitespace-
normalized set (so a struct shared across modules is emitted once per TU). Two
defects combined to corrupt include guards:

1. **Bare `#endif` deduped as a standalone chunk.** Each `#endif` line is its
   own chunk with the identical normalized key `#endif`. The first is recorded;
   every subsequent `#endif` collides and is silently dropped -- unbalancing the
   `#ifndef`/`#endif` pairing. A guard whose `#endif` carries a distinct comment
   (`#endif /* PCG32_H */`) survived; a bare `#endif` did not. So a prelude with
   `#ifndef A ... #endif` followed by `#ifndef B ... #endif` lost B's `#endif`.

2. **A guard opener preceded by a comment/decl on prior lines was swallowed.**
   The splitter only treated a `#` as a directive when it began a chunk. The
   common `/* license comment */\n#ifndef GUARD` idiom started the chunk at the
   comment; the scan then ran through `#ifndef GUARD`/`#define`/the typedef as
   ordinary text up to the first depth-0 `;`, hiding the conditional opener.
   That guard was then split per-line and its directives deduped away
   individually, dropping the `#ifndef` while keeping the body and `#endif`
   (`#endif without #if` + redefinition).

## Fix

`src/compiler/emit_module.c`:

- Added `inline_c_directive_class()` to classify a `#` directive as a
  conditional opener (`#if`/`#ifdef`/`#ifndef`), closer (`#endif`), or other.
- In `inline_c_split_chunks`, a conditional region is now consumed as a single
  atomic chunk (nesting-aware) from its opener through the matching `#endif`, so
  its interior directives are never deduped individually. An identical guarded
  block across modules still dedups as one unit; a non-identical one is emitted
  in full and the C preprocessor's own guard prevents redefinition.
- A non-directive chunk now ends at a newline that precedes a line-start `#`
  directive (at brace depth 0), so a leading comment/decl no longer swallows a
  following guard opener -- the opener begins a fresh chunk and the atomic path
  fires.

Un-guarded duplicate declarations still dedup as before.

## Verification

- `bash tests/run.sh` -> 1724 passed, 0 failed.
- `tur build .` on `stats` (was blocked here), `frame`, `watch` -> clean (the
  remaining `some?`-spec link error is a separate fix; see the
  separate-compilation report).
- Minimal repros: two/three include guards in a module prelude now keep every
  `#endif`; a duplicate non-guard declaration is still emitted once.
