#!/usr/bin/env python3
"""
tools/check-pack-spices.py -- assert a built docs pack carries spice pages.

`tools/genspices.py` treats a missing `../turmeric-spices/` checkout as a
warning and produces zero spice pages, which is right for CI (no checkout
there) and right for a contributor who does not have the sibling repo. It is
wrong immediately before a DEPLOY: the pack, the site's spice pages and the
search index all quietly lose every spice, and the recipe still exits 0. This
is the deploy-time counterpart to that leniency -- nothing else changes.

Usage:
    python3 tools/check-pack-spices.py web/public/docs-pack/
    python3 tools/check-pack-spices.py web/public/docs-pack/ --min 300
"""

import argparse
import json
import sys
from pathlib import Path


def check(pack_dir: Path, minimum: int) -> int:
    index_path = pack_dir / 'index.json'
    if not index_path.is_file():
        print(f'error: no pack manifest at {index_path} -- run the generators '
              f'with --emit-pack and tools/genpack.py first', file=sys.stderr)
        return 1

    try:
        index = json.loads(index_path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as exc:
        print(f'error: cannot read {index_path}: {exc}', file=sys.stderr)
        return 1

    n = len(index.get('spices') or [])
    if n >= minimum:
        print(f'  pack spices: {n} page(s)')
        return 0

    print(f'error: the docs pack carries {n} spice page(s), expected at least '
          f'{minimum}', file=sys.stderr)
    print('       Deploying this pack drops every spice from the site\'s spice',
          file=sys.stderr)
    print('       pages, the offline pack and the search index -- and',
          file=sys.stderr)
    print('       tools/genspices.py reports a missing checkout as a WARNING,',
          file=sys.stderr)
    print('       so the build that produced it exited 0.', file=sys.stderr)
    print('', file=sys.stderr)
    print('       Check that ../turmeric-spices/ resolves from the directory '
          'just runs in:', file=sys.stderr)
    print('         python3 -c "from pathlib import Path; '
          'print(Path(\'../turmeric-spices\').resolve())"', file=sys.stderr)
    print('', file=sys.stderr)
    print('       In a WORKTREE of a bare checkout the sibling is one level '
          'further out', file=sys.stderr)
    print('       than in a plain clone, so `../turmeric-spices` needs a '
          'bridge symlink', file=sys.stderr)
    print('       beside the worktrees. See the offline docs guide, "Notes for '
          'contributors".', file=sys.stderr)
    return 1


def main() -> None:
    p = argparse.ArgumentParser(
        description='Fail when a built docs pack carries no spice pages.')
    p.add_argument('pack_dir', help='Pack directory (e.g. web/public/docs-pack/)')
    p.add_argument('--min', type=int, default=1, dest='minimum',
                   help='Minimum spice pages required (default: 1)')
    args = p.parse_args()
    sys.exit(check(Path(args.pack_dir), args.minimum))


if __name__ == '__main__':
    main()
