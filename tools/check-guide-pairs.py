#!/usr/bin/env python3
"""
tools/check-guide-pairs.py -- Verify toggle pairs in Turmeric guide markdown files.

Usage:
    python3 tools/check-guide-pairs.py docs/guides/          # all guides
    python3 tools/check-guide-pairs.py docs/guides/quickstart.md  # single file

For each adjacent turmeric+sweet-exp block pair found, the checker reports:
  - The guide file and approximate line number
  - Whether the pair is non-empty on both sides
  - (if 'tur' binary available) parse-equality: both blocks parse to the same AST

Exit code: 0 if all checks pass, 1 if any pair fails or no pairs are found
and --require-pairs is set.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


PAIR_RE = re.compile(
    r'(?m)^```turmeric\n(.*?)^```\n\s*^```sweet-exp\n(.*?)^```',
    re.DOTALL | re.MULTILINE,
)

NO_CHECK_RE = re.compile(r'(?m)^```turmeric no-check\n')


def find_pairs(text: str) -> list[tuple[str, str, int]]:
    """Return list of (turmeric_src, sweet_exp_src, line_number) for each pair."""
    pairs = []
    for m in PAIR_RE.finditer(text):
        line_no = text[:m.start()].count('\n') + 1
        tur_src = m.group(1)
        sweet_src = m.group(2)
        pairs.append((tur_src, sweet_src, line_no))
    return pairs


def check_nonempty(tur_src: str, sweet_src: str) -> list[str]:
    errors = []
    if not tur_src.strip():
        errors.append('turmeric block is empty')
    if not sweet_src.strip():
        errors.append('sweet-exp block is empty')
    return errors


def check_ascii(tur_src: str, sweet_src: str) -> list[str]:
    errors = []
    for i, ch in enumerate(tur_src):
        if ord(ch) > 127:
            errors.append(f'turmeric block contains non-ASCII character at offset {i}: {repr(ch)}')
            break
    for i, ch in enumerate(sweet_src):
        if ord(ch) > 127:
            errors.append(f'sweet-exp block contains non-ASCII character at offset {i}: {repr(ch)}')
            break
    return errors


def try_parse_check(tur_src: str, sweet_src: str, tur_bin: str) -> list[str]:
    """Use 'tur parse-check' to compare ASTs. Returns error strings."""
    errors = []
    with tempfile.NamedTemporaryFile(suffix='.tur', mode='w', delete=False) as tf:
        tf.write(tur_src)
        tur_file = tf.name
    with tempfile.NamedTemporaryFile(suffix='.sweet', mode='w', delete=False) as sf:
        sf.write(sweet_src)
        sweet_file = sf.name
    try:
        result = subprocess.run(
            [tur_bin, 'parse-check', tur_file, sweet_file],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode != 0:
            errors.append(f'parse-check failed: {result.stderr.strip() or result.stdout.strip()}')
    except FileNotFoundError:
        pass  # tur binary not available; skip parse check
    except subprocess.TimeoutExpired:
        errors.append('parse-check timed out')
    finally:
        Path(tur_file).unlink(missing_ok=True)
        Path(sweet_file).unlink(missing_ok=True)
    return errors


def check_file(path: Path, tur_bin: str | None, verbose: bool) -> tuple[int, int, int]:
    """Returns (pairs_found, pairs_ok, pairs_failed)."""
    text = path.read_text(encoding='utf-8')
    pairs = find_pairs(text)
    ok = failed = 0
    for tur_src, sweet_src, line_no in pairs:
        errors: list[str] = []
        errors += check_nonempty(tur_src, sweet_src)
        errors += check_ascii(tur_src, sweet_src)
        if tur_bin and not errors:
            errors += try_parse_check(tur_src, sweet_src, tur_bin)
        if errors:
            failed += 1
            print(f'FAIL  {path}:{line_no}')
            for e in errors:
                print(f'      {e}')
        else:
            ok += 1
            if verbose:
                print(f'ok    {path}:{line_no}')
    return len(pairs), ok, failed


def main() -> None:
    p = argparse.ArgumentParser(description='Check guide toggle pair equivalence.')
    p.add_argument('paths', nargs='+', help='Guide file(s) or directory of .md files')
    p.add_argument('--tur', default=None, help='Path to tur binary (default: auto-detect)')
    p.add_argument('--verbose', '-v', action='store_true')
    p.add_argument('--require-pairs', action='store_true',
                   help='Exit 1 if no pairs are found at all')
    args = p.parse_args()

    tur_bin = args.tur or shutil.which('tur')

    md_files: list[Path] = []
    for raw in args.paths:
        pt = Path(raw)
        if pt.is_dir():
            md_files += sorted(f for f in pt.glob('*.md') if f.stem != 'README')
        elif pt.suffix == '.md':
            md_files.append(pt)
        else:
            print(f'warning: skipping {pt} (not a .md file or directory)', file=sys.stderr)

    total_pairs = total_ok = total_failed = 0
    for f in md_files:
        n, ok, fail = check_file(f, tur_bin, args.verbose)
        total_pairs += n
        total_ok += ok
        total_failed += fail

    paired_guides = sum(
        1 for f in md_files
        if find_pairs(f.read_text(encoding='utf-8'))
    )

    print()
    print(f'Guides checked : {len(md_files)}')
    print(f'Guides with pairs: {paired_guides}')
    print(f'Pairs found    : {total_pairs}')
    print(f'Pairs ok       : {total_ok}')
    print(f'Pairs failed   : {total_failed}')
    if tur_bin:
        print(f'Checker        : {tur_bin} (parse-check)')
    else:
        print('Checker        : basic only (tur binary not found; skipped parse-check)')

    if total_failed > 0:
        sys.exit(1)
    if args.require_pairs and total_pairs == 0:
        print('error: no pairs found', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
