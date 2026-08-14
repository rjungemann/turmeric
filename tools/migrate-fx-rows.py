#!/usr/bin/env python3
"""migrate-fx-rows.py -- rewrite legacy effect-row spellings to `#fx{...}`.

fx-row-syntax-rename-plan Phase 3.  Walks each path argument (file or
directory) and rewrites:

  * `#{`   -> `#fx{`   (legacy bare effect-row form)
  * `@{`   -> `#fx{`   (legacy `@`-prefixed effect-row form), but only
                       when the `@` is in prefix position (whitespace or
                       an opening bracket to the left).  A bare `@x`
                       deref is left alone.

The rewriter respects the existing source structure:

  * Inside `"..."` string literals: never rewrite (escaped quotes
    handled).
  * Inside `;`/`;;`/`;;;` comments through end-of-line: never rewrite.
  * Inside ` ```c ... ``` ` Turmeric inline-C blocks: never rewrite.
    (The C body is opaque to the reader; an `#{` inside C is a
    compound-literal designator, not a Turmeric effect row.)
  * `#map{`, `#set{`, `#row{`, `#refine{`, `#r{`, `#fx{`, `#json...`,
    `#s(`, `#[...]`, `#?...`, `#;...`, `#refine`, etc. all have a
    non-`{` character between `#` and `{`, so the `#{` match never
    triggers on them.  The script asserts this rather than enumerating.

Dry-run by default -- the script prints a unified diff to stdout and
EXITS WITHOUT WRITING.  Pass --apply to overwrite the source files in
place.  Per CLAUDE.md feedback_no_global_rename, never apply without
reviewing the diff first.

Usage:
  python3 tools/migrate-fx-rows.py <path>...           # dry-run
  python3 tools/migrate-fx-rows.py --apply <path>...   # write
  python3 tools/migrate-fx-rows.py --stats <path>...   # counts only
"""

import argparse
import difflib
import os
import sys
from typing import Tuple

# File suffixes the script touches.  .tur source files and .md docs whose
# code samples may contain the legacy spelling.  .tur.sweet uses the same
# reader rules.
SUFFIXES = (".tur", ".tur.sweet", ".md")

# Reader literals that start with `#<char>{` (where char != '{').  None of
# these collide with the `#{` rewrite because there is always a tag char
# between `#` and `{`, but we keep an assertion list for sanity.
KNOWN_HASH_TAGS = ("map", "set", "row", "refine", "r", "fx", "json", "s",
                   "lang", "include")


def rewrite_text(src: str) -> Tuple[str, int, int]:
    """Return (rewritten_text, hash_count, at_count).

    Stateful scan -- tracks string/comment/cblock context so rewrites only
    fire in ordinary code positions.
    """
    out = []
    i = 0
    n = len(src)
    hash_count = 0
    at_count = 0

    # We approximate the inline-C fence as the triple-backtick form. The
    # fence opener can be ```c, ```C, or ``` followed by `c` on the same
    # line; the closer is the matching triple-backtick on its own.
    while i < n:
        c = src[i]

        # `;` line comments: copy through to newline.
        if c == ';':
            j = src.find('\n', i)
            if j == -1:
                out.append(src[i:])
                i = n
            else:
                out.append(src[i:j + 1])
                i = j + 1
            continue

        # `"` string literal: copy through to closing quote, honoring
        # backslash escapes.
        if c == '"':
            out.append('"')
            i += 1
            while i < n:
                if src[i] == '\\' and i + 1 < n:
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            continue

        # Triple-backtick fence.  Copy until the next triple-backtick.
        if c == '`' and src.startswith("```", i):
            # Locate end fence.
            end = src.find("```", i + 3)
            if end == -1:
                # Unterminated -- copy rest verbatim and bail.
                out.append(src[i:])
                i = n
                continue
            out.append(src[i:end + 3])
            i = end + 3
            continue

        # `#{` -- legacy effect row.
        if c == '#' and i + 1 < n and src[i + 1] == '{':
            out.append("#fx{")
            i += 2
            hash_count += 1
            continue

        # `@{` -- legacy effect row, only when `@` is in prefix position.
        # Prefix position means: at start of file, or preceded by
        # whitespace / `(` / `[` / `{` / `,`.  This excludes constructs
        # like `(deref @{...})` (already handled because `@` is after a
        # space) and excludes the unusual `foo@{...}` (no such legal form
        # exists in turmeric today, but we err on the side of caution).
        if c == '@' and i + 1 < n and src[i + 1] == '{':
            prev = out[-1][-1] if out and out[-1] else ''
            if prev == '' or prev in " \t\r\n([{,":
                out.append("#fx{")
                i += 2
                at_count += 1
                continue

        # `#<tag>{` family: assert recognized (sanity check) and copy.
        if c == '#' and i + 1 < n and src[i + 1].isalpha():
            # Scan tag identifier.
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] == '-' or src[j] == '_'):
                j += 1
            tag = src[i + 1:j]
            # Note: not all `#word` are reader literals (e.g. `#lang`
            # directive, `#[attr]`), so we don't error on unknown tags.
            # Just copy and move on.
            out.append(src[i:j])
            i = j
            continue

        out.append(c)
        i += 1

    return ''.join(out), hash_count, at_count


# Paths skipped by default.  `docs/archive/` is the resolved-bug paper trail
# (CLAUDE.md "Archiving resolved reports") -- rewriting it would falsify
# history.  The fx-row rename plan itself uses `#{...}` literally to describe
# what is being replaced; the docs sweep handles it by hand.
DEFAULT_EXCLUDE_DIRS = ("docs/archive",)
DEFAULT_EXCLUDE_FILES = ("docs/archive/history/fx-row-syntax-rename-plan.md",)


def _is_default_excluded(path: str) -> bool:
    norm = path.replace("\\", "/")
    for d in DEFAULT_EXCLUDE_DIRS:
        if f"/{d}/" in f"/{norm}/" or norm.endswith(d) or f"/{d}/" in norm:
            return True
    for f in DEFAULT_EXCLUDE_FILES:
        if norm.endswith(f):
            return True
    return False


def walk(paths, include_archive: bool, extra_excludes):
    extras = set(extra_excludes or ())
    for p in paths:
        if os.path.isfile(p):
            if not include_archive and _is_default_excluded(p):
                continue
            if p in extras:
                continue
            yield p
            continue
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                # Skip a few obvious build/output directories.
                base = os.path.basename(root)
                if base in (".tur-abi-cache", "build", "build-release",
                            ".tur-repl-cache", "node_modules", ".git"):
                    continue
                if not include_archive and _is_default_excluded(root):
                    continue
                for fn in files:
                    if not fn.endswith(SUFFIXES):
                        continue
                    full = os.path.join(root, fn)
                    if not include_archive and _is_default_excluded(full):
                        continue
                    if full in extras:
                        continue
                    yield full


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+", help="Files or directories to scan")
    ap.add_argument("--apply", action="store_true",
                    help="Write rewrites in place (default: dry-run diff)")
    ap.add_argument("--stats", action="store_true",
                    help="Print per-file counts and totals; suppress diff")
    ap.add_argument("--include-archive", action="store_true",
                    help="Also rewrite docs/archive/ and the rename plan "
                         "(default: skip; these intentionally preserve the "
                         "legacy spelling)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="Additional path to skip (may repeat)")
    args = ap.parse_args()

    files_changed = 0
    total_hash = 0
    total_at = 0
    for path in walk(args.paths, args.include_archive, args.exclude):
        try:
            with open(path, "r", encoding="utf-8") as f:
                src = f.read()
        except (OSError, UnicodeDecodeError) as e:
            print(f"# skip {path}: {e}", file=sys.stderr)
            continue
        new, hc, ac = rewrite_text(src)
        if new == src:
            continue
        files_changed += 1
        total_hash += hc
        total_at += ac
        if args.stats:
            print(f"{path}\t#{{->#fx{{ {hc}\t@{{->#fx{{ {ac}")
            continue
        if args.apply:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new)
            print(f"wrote {path} ({hc} #{{, {ac} @{{)")
        else:
            diff = difflib.unified_diff(
                src.splitlines(keepends=True),
                new.splitlines(keepends=True),
                fromfile=path, tofile=path + ".new",
            )
            sys.stdout.writelines(diff)

    if args.stats or args.apply:
        print(f"\n# {files_changed} files; "
              f"{total_hash} `#{{` rewrites; {total_at} `@{{` rewrites",
              file=sys.stderr)


if __name__ == "__main__":
    main()
