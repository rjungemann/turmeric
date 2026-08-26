#!/usr/bin/env python3
"""
tools/packlib.py -- shared helpers for emitting the Turmeric docs pack.

The docs pack is a chrome-free rendering of the guides, the stdlib API pages,
and (when the sibling checkout is present) the spice pages:

    web/public/docs-pack/
      index.json            version, nav tree, search strings, precache manifest
      guides/<slug>.html    article body only -- no <head>, no nav, no CSS
      api/<module>.html     same, one per module
      spices/<spice>.html   same, one per spice

It is emitted by the same generators that build docs/html/, is rendered in
Try Turmeric's in-app docs pane, and is precached wholesale by the service
worker so the pane works with no network at all. This module carries the parts
all three generators need: fragment writing, search-string extraction, and the
per-generator manifest sidecars that `tools/genpack.py` merges into index.json.
"""

from __future__ import annotations

import html as _html
import json
import re
from pathlib import Path

# Every generator drops one of these into the pack root; genpack.py merges them
# into index.json and then deletes them. Keeping them out of index.json itself
# means a partial `just docs` (say, guides only) never produces a half-written
# contract file that the pane would happily believe.
SIDECAR_SUFFIX = '.pack-manifest.json'

# How much prose from a page body goes into its search string. Full bodies
# would put ~2.5 MB of text into index.json -- which the pane fetches on every
# open and the service worker precaches -- to serve a search that headings and
# descriptions already answer well. Headings are always kept in full; this cap
# applies only to the running text after them.
SEARCH_PROSE_CHARS = 800

_TAG_RE = re.compile(r'<[^>]+>')
_WS_RE = re.compile(r'\s+')


def strip_tags(html_text: str) -> str:
    """Return the visible text of an HTML fragment, whitespace-collapsed."""
    text = re.sub(r'(?is)<(script|style)\b.*?</\1>', ' ', html_text)
    text = _TAG_RE.sub(' ', text)
    text = _html.unescape(text)
    return _WS_RE.sub(' ', text).strip()


def search_string(*parts: str, prose: str = '') -> str:
    """Build the lowercase `words` blob the pane's client-side search matches on.

    `parts` are the high-signal fields (title, category, description, headings,
    symbol names) and are kept whole; `prose` is body text and is truncated to
    SEARCH_PROSE_CHARS.
    """
    kept = [p.strip() for p in parts if p and p.strip()]
    if prose:
        kept.append(prose[:SEARCH_PROSE_CHARS])
    return _WS_RE.sub(' ', ' '.join(kept)).strip().lower()


def heading_names(toc_tokens: list) -> list[str]:
    """Flatten a python-markdown toc_tokens tree into a list of heading names."""
    out = []
    for tok in toc_tokens:
        name = (tok.get('name') or '').strip()
        if name:
            out.append(name)
        out.extend(heading_names(tok.get('children', [])))
    return out


# ---------------------------------------------------------------------------
# Link rewriting
#
# Fragments are written with the links their source had -- `other-guide.md`,
# `../api/tur-vec.html`. Resolving them into the pack's own `#doc=` URL space
# needs to know what the whole pack contains, which no single generator does,
# so `genpack.py` runs `rewrite_pack_links` over every fragment once all of
# them have landed. One pass, one place, and the same pass reports the links it
# could not resolve.
# ---------------------------------------------------------------------------

# `[text](other-guide.md)` / `[text](other-guide.md#anchor)` after conversion.
_MD_HREF_RE = re.compile(r'href="([^"#]+)\.md(#[^"]*)?"')

# The site's own API pages, as guides reference them: `../api/tur-vec.html`
# from docs/guides/, or `../html/api/tur-vec.html` from elsewhere under docs/.
_SITE_API_HREF_RE = re.compile(
    r'href="(?:\.\./)+(?:html/)?api/([A-Za-z0-9_\-]+)\.html(#[^"]*)?"')

_EXTERNAL_PREFIXES = ('http://', 'https://', 'mailto:', '#', '/')


def rewrite_pack_links(body_html: str, guide_slugs: set[str],
                       api_slugs: set[str]) -> tuple[str, list[str]]:
    """Point a fragment's cross-links at other pack pages.

    In-pack targets become `#doc=guides/<slug>` / `#doc=api/<module>`, which the
    Try docs pane intercepts and resolves without ever navigating away from the
    REPL. A link whose target is not in the pack is left exactly as written --
    it still resolves against the website for an online reader -- and is
    returned in the second element so the emitter can report it instead of the
    pane silently swallowing a dead click.
    """
    unresolved: list[str] = []

    def rewrite_md(m: re.Match) -> str:
        href, frag = m.group(1), m.group(2) or ''
        if href.startswith(_EXTERNAL_PREFIXES):
            return m.group(0)
        slug = href.rsplit('/', 1)[-1]
        if slug not in guide_slugs:
            unresolved.append(f'{href}.md')
            return m.group(0)
        return f'href="#doc=guides/{slug}{frag}"'

    def rewrite_api(m: re.Match) -> str:
        page, frag = m.group(1), m.group(2) or ''
        if page not in api_slugs:
            unresolved.append(f'api/{page}.html')
            return m.group(0)
        return f'href="#doc=api/{page}{frag}"'

    body_html = _MD_HREF_RE.sub(rewrite_md, body_html)
    body_html = _SITE_API_HREF_RE.sub(rewrite_api, body_html)
    return body_html, unresolved


_IMG_SRC_RE = re.compile(r'<img\b[^>]*?\bsrc="([^"]+)"')


def local_image_srcs(body_html: str) -> list[str]:
    """Every non-external `<img src>` in a fragment, in document order.

    Guides carry no images today; the pack copies and budget-counts them anyway
    so the day one arrives it is a build-time number, not a silent PWA payload.
    """
    return [m.group(1) for m in _IMG_SRC_RE.finditer(body_html)
            if not m.group(1).startswith(_EXTERNAL_PREFIXES)
            and not m.group(1).startswith('data:')]


def write_fragment(pack_dir: Path, rel_path: str, body_html: str) -> int:
    """Write one chrome-free fragment into the pack. Returns its byte size."""
    out = Path(pack_dir) / rel_path
    out.parent.mkdir(parents=True, exist_ok=True)
    data = body_html.strip() + '\n'
    out.write_text(data, encoding='utf-8')
    return len(data.encode('utf-8'))


def write_sidecar(pack_dir: Path, section: str, entries: list[dict]) -> Path:
    """Write one generator's slice of the pack manifest for genpack.py to merge.

    Merges into an existing sidecar for the same section, replacing entries
    with the same slug: `genspices.py` contributes to the `spices` section once
    per spice, so the section is built up across calls rather than in one shot.
    """
    pack_dir = Path(pack_dir)
    pack_dir.mkdir(parents=True, exist_ok=True)
    out = pack_dir / f'{section}{SIDECAR_SUFFIX}'

    merged: dict[str, dict] = {}
    if out.is_file():
        try:
            prior = json.loads(out.read_text(encoding='utf-8'))
            for entry in prior.get('entries', []):
                merged[entry.get('slug', '')] = entry
        except (OSError, ValueError):
            pass
    for entry in entries:
        merged[entry.get('slug', '')] = entry

    out.write_text(json.dumps({'section': section,
                               'entries': list(merged.values())},
                              ensure_ascii=True, separators=(',', ':')),
                   encoding='utf-8')
    return out


def read_sidecars(pack_dir: Path) -> dict[str, list[dict]]:
    """Read and remove every sidecar in the pack; returns {section: entries}."""
    pack_dir = Path(pack_dir)
    sections: dict[str, list[dict]] = {}
    for path in sorted(pack_dir.glob(f'*{SIDECAR_SUFFIX}')):
        try:
            payload = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        section = payload.get('section') or path.name[:-len(SIDECAR_SUFFIX)]
        sections.setdefault(section, []).extend(payload.get('entries', []))
        path.unlink()
    return sections
