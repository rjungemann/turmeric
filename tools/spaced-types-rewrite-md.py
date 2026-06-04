#!/usr/bin/env python3
"""
spaced-types-rewrite-md.py -- apply spaced-types-rewrite.py to Turmeric
code blocks inside markdown files.

Walks ```turmeric / ```tur / ```scheme / ```clojure fences and pipes
each block through tools/spaced-types-rewrite.py.  Other content
(prose, non-Turmeric code) is left untouched.

Usage:
  tools/spaced-types-rewrite-md.py [opts] PATH [PATH...]

Same mode flags as the underlying tool: --write (default), --dry-run,
--check.
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import sys

# Reuse the rewriter as a library.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
import importlib.util
_spec = importlib.util.spec_from_file_location(
    "spaced_types_rewrite",
    os.path.join(SCRIPT_DIR, "spaced-types-rewrite.py"),
)
_mod = importlib.util.module_from_spec(_spec)
sys.modules["spaced_types_rewrite"] = _mod
_spec.loader.exec_module(_mod)
rewrite_source = _mod.rewrite_source

TURMERIC_LANGS = {"turmeric", "tur", "scheme", "clojure", "lisp"}

FENCE_RE = re.compile(
    r"(^|\n)(```)([A-Za-z0-9_+-]*)(\n)(.*?)(\n```)",
    re.DOTALL,
)


def rewrite_markdown(src: str) -> tuple[str, int]:
    """Rewrite Turmeric code blocks inside markdown source."""
    out = []
    last = 0
    total = 0
    for m in FENCE_RE.finditer(src):
        lead, fence_open, lang, sep, body, fence_close = m.groups()
        out.append(src[last:m.start()])
        out.append(lead)
        out.append(fence_open)
        out.append(lang)
        out.append(sep)
        if lang.lower() in TURMERIC_LANGS:
            try:
                new_body, n = rewrite_source(body, rewrite_quoted=False)
            except Exception:
                # On any parse error, leave the block alone.
                new_body, n = body, 0
            out.append(new_body)
            total += n
        else:
            out.append(body)
        out.append(fence_close)
        last = m.end()
    out.append(src[last:])
    return "".join(out), total


def iter_md_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    if f.endswith(".md"):
                        yield os.path.join(root, f)
        else:
            yield p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--check", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if not (args.dry_run or args.check):
        args.write = True

    any_changed = False
    total_rewrites = 0
    n_files_changed = 0

    for path in iter_md_files(args.paths):
        try:
            with open(path, "r", encoding="utf-8") as fh:
                src = fh.read()
        except (OSError, UnicodeDecodeError) as e:
            print(f"{path}: read error: {e}", file=sys.stderr)
            return 2
        new_src, n = rewrite_markdown(src)
        if new_src != src:
            any_changed = True
            n_files_changed += 1
            total_rewrites += n
            if args.check:
                print(f"{path}: {n} fused annotation(s) in turmeric blocks",
                      file=sys.stderr)
            elif args.dry_run:
                sys.stdout.writelines(difflib.unified_diff(
                    src.splitlines(keepends=True),
                    new_src.splitlines(keepends=True),
                    fromfile=f"a/{path}", tofile=f"b/{path}",
                ))
            else:
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(new_src)
                if args.verbose:
                    print(f"rewrote {path} ({n} rewrites)", file=sys.stderr)
        elif args.verbose:
            print(f"clean {path}", file=sys.stderr)

    if args.check and any_changed:
        print(f"{n_files_changed} file(s) need rewrite, {total_rewrites} fused annotation(s)",
              file=sys.stderr)
        return 1
    if any_changed and not args.check:
        print(f"{n_files_changed} file(s) {'would change' if args.dry_run else 'rewritten'}, "
              f"{total_rewrites} rewrite(s) total", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
