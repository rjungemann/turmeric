#!/usr/bin/env python3
"""
tools/genpack.py -- assemble the Turmeric docs pack and check it.

`genguides.py --emit-pack`, `gendocs.py --emit-pack`, and
`genspices.py --emit-pack` each drop their chrome-free fragments plus a
manifest sidecar into the pack directory. This script is the pass that runs
after all of them, when -- and only when -- the whole pack is on disk:

  1. merge the sidecars into `index.json`, the pack's one contract;
  2. rewrite cross-links into the pack's `#doc=` URL space (only genpack knows
     the whole pack, so only genpack can tell an in-pack link from a website
     one) and report the links it could not resolve;
  3. check every fragment is well-formed and every manifest path exists;
  4. sweep files the manifest does not claim, so a deleted guide's fragment
     cannot linger in the pack and get precached forever;
  5. enforce the size budget.

The budget is the mechanism that keeps unconditional precaching honest: Try
Turmeric's service worker precaches the whole pack on install, with no toggle
and no opt-in, so a pack that outgrows the budget is a build error here rather
than silent PWA bloat someone discovers on a phone. If it trips, shrink the
pack -- compress images, drop a category, tier rarely-read pages -- never make
precaching conditional.

Usage:
    python3 tools/genpack.py web/public/docs-pack/ --version 0.38.0
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import sys
from html.parser import HTMLParser
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import packlib  # noqa: E402  (sibling module, path fixed up just above)

DEFAULT_MAX_BYTES = 16 * 1024 * 1024

# Fragments are article bodies, so they legitimately contain unclosed void
# elements; anything else must nest properly or the pane's innerHTML render
# silently swallows content.
VOID_TAGS = {
    'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input',
    'link', 'meta', 'param', 'source', 'track', 'wbr',
}

# A fragment is a body, not a page: these belong to the wrapper the site
# supplies, and their presence means a generator leaked chrome into the pack.
FORBIDDEN_TAGS = {'html', 'head', 'body', 'title', 'meta', 'link'}


class FragmentChecker(HTMLParser):
    """Verify a pack fragment is a well-formed, chrome-free HTML body."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.stack: list[str] = []
        self.errors: list[str] = []

    def handle_starttag(self, tag, attrs):
        if tag in FORBIDDEN_TAGS:
            self.errors.append(f'line {self.getpos()[0]}: page-level <{tag}> in a fragment')
            return
        if tag not in VOID_TAGS:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag in VOID_TAGS:
            return
        if tag not in self.stack:
            self.errors.append(f'line {self.getpos()[0]}: stray </{tag}>')
            return
        while self.stack:
            open_tag = self.stack.pop()
            if open_tag == tag:
                break
            self.errors.append(
                f'line {self.getpos()[0]}: <{open_tag}> not closed before </{tag}>')

    def finish(self) -> list[str]:
        for tag in reversed(self.stack):
            self.errors.append(f'<{tag}> never closed')
        return self.errors


def check_fragment(path: Path) -> list[str]:
    checker = FragmentChecker()
    checker.feed(path.read_text(encoding='utf-8'))
    checker.close()
    return checker.finish()


def generated_stamp() -> str:
    """UTC timestamp for index.json, honouring SOURCE_DATE_EPOCH when set."""
    epoch = os.environ.get('SOURCE_DATE_EPOCH')
    if epoch and epoch.isdigit():
        when = _dt.datetime.fromtimestamp(int(epoch), _dt.timezone.utc)
    else:
        when = _dt.datetime.now(_dt.timezone.utc)
    return when.replace(microsecond=0).isoformat().replace('+00:00', 'Z')


def human(n: int) -> str:
    return f'{n / (1024 * 1024):.2f} MB' if n >= 1024 * 1024 else f'{n / 1024:.1f} KB'


def build(pack_dir: Path, version: str, max_bytes: int,
          strict_links: bool = False) -> int:
    pack_dir = Path(pack_dir)
    if not pack_dir.is_dir():
        print(f'error: no docs pack at {pack_dir} -- run the generators with '
              f'--emit-pack first', file=sys.stderr)
        return 1

    sections = packlib.read_sidecars(pack_dir)
    guides = sorted(sections.get('guides', []), key=lambda e: e['slug'])
    api = sorted(sections.get('api', []), key=lambda e: e['slug'])
    spices = sorted(sections.get('spices', []), key=lambda e: e['slug'])

    if not guides and not api:
        print(f'error: {pack_dir} has no guide or API fragments -- did the '
              f'generators run with --emit-pack?', file=sys.stderr)
        return 1

    # --- 2. link rewriting, now that the whole pack is known -----------------
    guide_slugs = {e['slug'] for e in guides}
    api_slugs = {e['slug'] for e in api}
    unresolved: dict[str, list[str]] = {}
    for entry in guides + api + spices:
        frag = pack_dir / entry['path']
        if not frag.is_file():
            print(f'error: {entry["path"]} is in the manifest but not on disk',
                  file=sys.stderr)
            return 1
        before = frag.read_text(encoding='utf-8')
        after, missing = packlib.rewrite_pack_links(before, guide_slugs, api_slugs)
        if after != before:
            frag.write_text(after, encoding='utf-8')
            entry['bytes'] = len(after.encode('utf-8'))
        if missing:
            unresolved[entry['path']] = sorted(set(missing))

    # --- 3. fragment well-formedness ----------------------------------------
    bad = 0
    for entry in guides + api + spices:
        errors = check_fragment(pack_dir / entry['path'])
        for err in errors[:5]:
            print(f'error: {entry["path"]}: {err}', file=sys.stderr)
        bad += len(errors)
    if bad:
        print(f'error: {bad} malformed-fragment problem(s); the docs pane '
              f'renders fragments with innerHTML, so these lose content',
              file=sys.stderr)
        return 1

    # --- 1. the manifest -----------------------------------------------------
    files = [e['path'] for e in guides + api + spices]
    # Images the guides reference were copied in alongside their fragments;
    # they are part of the pack and part of the budget.
    known = set(files)
    images = sorted(
        str(p.relative_to(pack_dir))
        for p in pack_dir.rglob('*')
        if p.is_file() and str(p.relative_to(pack_dir)) not in known
        and p.name != 'index.json' and p.suffix.lower() != '.html'
    )
    files.extend(images)

    index = {
        'version': version,
        'generated': generated_stamp(),
        'guides': guides,
        'api': api,
        'spices': spices,
        # `files` doubles as the service worker's precache manifest: one list,
        # so a fragment can never be in the pack but missing from precache.
        'files': files,
    }
    index_path = pack_dir / 'index.json'
    index_path.write_text(json.dumps(index, ensure_ascii=True,
                                     separators=(',', ':')),
                          encoding='utf-8')

    # --- 4. sweep files the manifest does not claim --------------------------
    keep = set(files) | {'index.json'}
    swept = 0
    for path in sorted(pack_dir.rglob('*'), reverse=True):
        rel = str(path.relative_to(pack_dir))
        if path.is_file():
            if rel not in keep:
                path.unlink()
                swept += 1
        elif path.is_dir() and not any(path.iterdir()):
            path.rmdir()
    if swept:
        print(f'  swept {swept} stale file(s) from the pack')

    # --- 5. the size budget --------------------------------------------------
    total = sum((pack_dir / f).stat().st_size for f in files)
    total += index_path.stat().st_size

    print(f'  index.json: {len(guides)} guides, {len(api)} API modules, '
          f'{len(spices)} spice pages ({human(index_path.stat().st_size)})')
    print(f'  pack size:  {human(total)} of {human(max_bytes)} budget')

    if unresolved:
        n = sum(len(v) for v in unresolved.values())
        print(f'  note: {n} cross-link(s) in {len(unresolved)} page(s) point '
              f'outside the pack and were left pointing at the website:',
              file=sys.stderr)
        for path, links in sorted(unresolved.items())[:10]:
            print(f'        {path}: {", ".join(links[:4])}', file=sys.stderr)
        if strict_links:
            print('error: --strict-links is set and the pack has unresolved '
                  'cross-links', file=sys.stderr)
            return 1

    if total > max_bytes:
        print(f'\nerror: docs pack is {human(total)}, over the '
              f'{human(max_bytes)} budget.\n'
              f'       Try Turmeric precaches this pack unconditionally -- there '
              f'is no offline toggle --\n'
              f'       so the fix is a smaller pack, never conditional '
              f'precaching. In order of preference:\n'
              f'         1. compress or drop oversized images;\n'
              f'         2. exclude a doc category from the pack;\n'
              f'         3. tier rarely-read reference pages behind a lazy '
              f'fetch.\n'
              f'       See docs/upcoming/offline-docs-plan.md (OD1).',
              file=sys.stderr)
        return 1

    print(f'Done: docs pack at {pack_dir}/ (version {version})')
    return 0


def main() -> None:
    p = argparse.ArgumentParser(
        description='Merge, check, and budget the Turmeric docs pack.')
    p.add_argument('pack_dir', help='Pack directory (e.g. web/public/docs-pack/)')
    p.add_argument('--version', default=None,
                   help='Version stamped into index.json (default: read VERSION)')
    p.add_argument('--max-bytes', type=int, default=DEFAULT_MAX_BYTES,
                   help=f'Size budget in bytes (default: {DEFAULT_MAX_BYTES})')
    p.add_argument('--strict-links', action='store_true',
                   help='Fail when a cross-link does not resolve inside the pack '
                        '(default: report and continue, since guides legitimately '
                        'link to pages the pack does not carry)')
    args = p.parse_args()

    version = args.version
    if not version:
        version_file = Path(__file__).resolve().parent.parent / 'VERSION'
        version = version_file.read_text(encoding='utf-8').strip() \
            if version_file.is_file() else '0.0.0'

    sys.exit(build(Path(args.pack_dir), version, args.max_bytes,
                   strict_links=args.strict_links))


if __name__ == '__main__':
    main()
