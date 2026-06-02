---
title: Parse-Check Subcommand Plan
category: Planning
description: Plan for implementing the `tur parse-check` subcommand that backs guide toggle-pair AST verification
---

# `tur parse-check` Subcommand -- Plan

## Goal

Implement the `tur parse-check` subcommand so that
`tools/check-guide-pairs.py` can actually verify that each paired
`turmeric` + `sweet-exp` example in the guides reads to the **same AST**.

Today the checker shells out to `tur parse-check <a> <b>` (see
`try_parse_check` in `tools/check-guide-pairs.py`), but that subcommand
does not exist in any current `tur` binary or on `origin/main`. As a
result:

- The parse-equality step **never passes** against a real binary -- every
  paired guide (including the already-accepted `quickstart.md`, 28/28)
  reports `FAIL ... parse-check failed: 'parse-check' is not a tur command`.
- The only checks that effectively run are the non-empty and ASCII-only
  passes. The headline promise of the toggle widget -- "both sides parse to
  the same thing" -- is unverified.

This plan closes that gap: ship the subcommand, then make
`check-guides` green and wire it into CI.

## Background -- how the checker calls it

From `tools/check-guide-pairs.py`:

```python
result = subprocess.run(
    [tur_bin, 'parse-check', tur_file, sweet_file],
    capture_output=True, text=True, timeout=10,
)
if result.returncode != 0:
    errors.append(f'parse-check failed: {result.stderr.strip() or result.stdout.strip()}')
```

Key facts the implementation must honour:

- **Two positional file arguments.** The first is the `turmeric` block, the
  second is the `sweet-exp` block.
- **Temp-file extensions are `.tur` and `.sweet`** -- *not* `.tur.sweet`.
  So extension-based dialect detection (`is_tursweet_path` /
  `.tur.sweet` suffix at `src/main.c:3810`) will **not** fire for the second
  file. The subcommand must force the sweet reader on argument 2 rather
  than relying on the extension.
- **Inline snippets omit `#lang sweet-exp`.** Only complete-program
  examples carry the directive. `parse-check` therefore cannot depend on
  `detect_lang` finding a directive; it must select the reader by argument
  position (arg1 = turmeric, arg2 = sweet) unless a `#lang` directive
  overrides.
- **Exit code is the contract.** `0` = ASTs equal; non-zero = mismatch or
  read error. A human-readable diff on stderr is a bonus, not required.

## Existing building blocks

The implementation can lean entirely on machinery that already exists:

| Piece | Location | Use |
|---|---|---|
| `ReaderType` enum (`READER_TURMERIC` ... `READER_SWEET`) | `src/compiler/diag.h:142` | select dialect per file |
| `SourceFile.reader_type` field | `src/compiler/diag.h:150` | force the reader for arg 2 |
| `detect_lang(...)` | `src/compiler/diag.h` | honour an explicit `#lang` directive when present |
| `read_all_with_registry(...)` | used by `cmd_format` (`src/main.c:3714`) | read a `SourceFile` into `Form **` |
| `form_print(Buf*, const Form*)` | `src/compiler/forms.h:134` | canonical s-expr serialization of a form tree |
| `read_entire_file(...)` | used throughout `src/main.c` | load file contents |

`cmd_format` (`src/main.c:3651`) is the closest existing template: it
builds a `SourceFile`, sets `reader_type`, registers it with diag, runs
`read_all_with_registry`, and walks the resulting `Form **`. `parse-check`
does the same thing twice and compares.

## Design

### Comparison strategy

Serialize both form vectors with `form_print` into two buffers and compare
the byte strings. `form_print` already emits a canonical s-expression
regardless of the surface dialect that produced the tree, so two
parse-equal inputs serialize identically. This avoids writing (and
maintaining) a structural `form_eq` and gives a printable diff for free.

Pseudocode:

```c
static int cmd_parse_check(const char *path_a, const char *path_b) {
    Buf sa = read_and_print(path_a, READER_TURMERIC);  // forced s-expr
    Buf sb = read_and_print(path_b, READER_SWEET);     // forced sweet
    if (diag_had_error()) return 2;                     // read/parse error
    if (sa.len == sb.len && memcmp(sa.data, sb.data, sa.len) == 0)
        return 0;                                       // equal
    // print both serializations to stderr for the diff
    return 1;                                           // mismatch
}
```

`read_and_print` mirrors the `cmd_format` setup: build `SourceFile`,
set `reader_type` (but let an explicit `#lang` directive win, via
`detect_lang`, so complete-program sweet blocks still work), preload
manifest reader-macros (optional -- guides do not use them, so this can be
skipped initially), call `read_all_with_registry`, then `form_print` each
form separated by `\n`.

### Reader selection precedence

1. If the file begins with a `#lang` directive, honour it (`detect_lang`).
2. Otherwise force the reader from the argument position: arg1 ->
   `READER_TURMERIC`, arg2 -> `READER_SWEET`.

This matches how the guides are authored (inline pairs omit the directive;
complete programs include it on the sweet side).

### CLI wiring

- Add `parse-check` to the subcommand dispatch in `main` (alongside
  `emit-c`, `check`, `format`, ... near `src/main.c:7427`).
- Add it to the `builtins[]` shadowing list (`src/main.c:6410`) so a spice
  recipe named `parse-check` does not shadow it.
- Add a one-line usage entry to the help text (`src/main.c` help blocks
  near 4010 / 6508 / 6708).

## Deliverables

1. `cmd_parse_check(const char *a, const char *b)` in `src/main.c`,
   dispatched from `main`.
2. Help-text and `builtins[]` entries.
3. A small fixture-style test: a pair that is equal (exit 0) and a pair
   that is deliberately different (exit 1), plus a malformed input
   (exit 2). A shell test under `tests/` driving the built `tur` is enough;
   no `expected.c` snapshot is involved.
4. `tools/check-guide-pairs.py` runs clean against the built binary:
   `python3 tools/check-guide-pairs.py docs/guides/` reports
   `Pairs failed : 0`.
5. Documentation: a line in the formatter/reader tooling docs (and the
   guides `README.md` "Coverage" note) stating that pairs are now
   machine-verified.

## Validation

```sh
cmake --build build -j --config Debug
# subcommand exists and behaves:
build/tur parse-check a.tur a.sweet ; echo $?    # 0 when equal
# whole guide tree is green:
PATH="$PWD/build:$PATH" python3 tools/check-guide-pairs.py docs/guides/
```

Expect `Pairs ok` to equal `Pairs found` and `Pairs failed : 0`. If any
real guide pair is genuinely *not* parse-equal, fix the guide (not the
checker) -- the whole point is to catch drift.

## Follow-up: wire into CI

Once `check-guides` can pass, add it to the docs CI gate (the `just
check-docs` target already exists) so future guide edits that break a pair
fail the build. Until then, `check-guides` is advisory only.

## Non-goals

- A general structural `form_eq` API -- string comparison of `form_print`
  output is sufficient and simpler.
- Normalising semantically-equivalent-but-textually-different trees (e.g.
  macro expansion). `parse-check` compares the **read** form tree only, not
  post-macro-expansion ASTs.
- Changing the guide authoring convention or the toggle-widget rendering in
  `genguides.py`.

## Open questions

- Should `parse-check` accept a `--lang A,B` override for the two files, for
  use outside the guide checker? Default: no -- positional precedence plus
  `#lang` detection covers the known caller. Add the flag only if a second
  consumer appears.
- Should reader-macro preloading be included from day one? Default: no --
  no guide pair uses manifest reader-macros; add it if that changes.
- Should mismatches print a structural diff or just the two serializations?
  Default: print both `form_print` outputs; a real diff can come later.

## Acceptance checklist

- [ ] `tur parse-check a b` exits 0 on equal, 1 on mismatch, 2 on read error.
- [ ] Forces sweet reader on arg 2 despite the `.sweet` (non-`.tur.sweet`)
      extension; honours an explicit `#lang` directive when present.
- [ ] `parse-check` appears in help text and `builtins[]`.
- [ ] `python3 tools/check-guide-pairs.py docs/guides/` reports zero failed
      pairs against the built binary.
- [ ] A test exercises the equal / mismatch / error exit codes.
- [ ] (Follow-up) `check-guides` added to the CI docs gate.
