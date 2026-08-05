#!/usr/bin/env python3
"""
tools/genguides.py -- Render Turmeric guide markdown files to HTML.

Usage:
    python3 tools/genguides.py docs/guides/ [--out docs/html/guides/]
"""

import argparse
import html as _html
import re
import subprocess
import sys
from pathlib import Path

import markdown as md_lib


def get_creation_date(path: Path, repo_root: Path) -> str | None:
    """Return the YYYY-MM-DD date the file was first added to git, or None."""
    try:
        rel = path.resolve().relative_to(repo_root.resolve())
    except ValueError:
        rel = path
    try:
        result = subprocess.run(
            ['git', 'log', '--diff-filter=A', '--follow',
             '--format=%ai', '--', str(rel)],
            cwd=repo_root, capture_output=True, text=True, check=False,
        )
    except FileNotFoundError:
        return None
    if result.returncode != 0:
        return None
    lines = [ln.strip() for ln in result.stdout.splitlines() if ln.strip()]
    if not lines:
        return None
    return lines[-1].split(' ', 1)[0]


def parse_front_matter(text: str) -> tuple[dict, str]:
    """Return (meta_dict, body) after stripping YAML front matter (--- blocks)."""
    if not text.startswith('---\n'):
        return {}, text
    end = text.find('\n---', 4)
    if end == -1:
        return {}, text
    fm_text = text[4:end]
    rest_start = text.find('\n', end + 1)
    body = text[rest_start + 1:] if rest_start != -1 else ''
    meta: dict = {}
    for line in fm_text.splitlines():
        if ':' in line:
            k, _, v = line.partition(':')
            k, v = k.strip(), v.strip()
            if k:
                meta[k] = v
    return meta, body


def build_categories_from_meta(meta_by_stem: dict, all_stems: set) -> list:
    """Build ordered category list from guide front matter."""
    buckets: dict[str, list] = {}
    for stem in sorted(all_stems):
        meta = meta_by_stem.get(stem, {})
        cat = meta.get('category', '').strip() or 'Other'
        title = meta.get('title', stem.replace('-', ' ').title()).strip()
        desc = meta.get('description', '').strip()
        buckets.setdefault(cat, []).append({'stem': stem, 'label': title, 'desc': desc})
    cat_names = sorted(k for k in buckets if k != 'Other')
    if 'Other' in buckets:
        cat_names.append('Other')
    return [{'name': name, 'guides': buckets[name]} for name in cat_names]


STYLE_REL = '../api/style.css'

SIDEBAR_GLOBALS = '''\
      <hr class="sidebar-divider">
      <h3>Language</h3>
      <ul>
        <li><a href="/tour">Tour</a></li>
        <li><a href="/try">Try It</a></li>
      </ul>
      <h3>Ecosystem</h3>
      <ul>
        <li><a href="/docs/html/guides/">Guides</a></li>
        <li><a href="/docs/html/api/">API Docs</a></li>
        <li><a href="https://spices.turmeric-lang.com">Spices</a></li>
      </ul>
      <h3>Community</h3>
      <ul>
        <li><a href="https://github.com/rjungemann/turmeric">GitHub</a></li>
      </ul>'''

PAGE_HEADER = '''\
  <header class="site-header">
    <button class="hamburger" aria-label="Toggle navigation">
      <span></span><span></span><span></span>
    </button>
    <a class="nav-logo" href="/">
      <img src="/logo-icon.svg" width="28" height="28" alt="">
      <img src="/logo.svg" width="101" height="28" alt="Turmeric">
    </a>
    <nav>
      <a href="/docs/html/guides/" class="active">Guides</a>
      <a href="/docs/html/api/">API Docs</a>
      <a href="https://spices.turmeric-lang.com">Spices</a>
      <a href="/try">Try It</a>
    </nav>
  </header>'''

INDEX_PAGE_HEADER = '''\
  <header class="site-header">
    <button class="hamburger" aria-label="Toggle navigation">
      <span></span><span></span><span></span>
    </button>
    <a class="nav-logo" href="/">
      <img src="/logo-icon.svg" width="28" height="28" alt="">
      <img src="/logo.svg" width="101" height="28" alt="Turmeric">
    </a>
    <nav>
      <a href="/docs/html/guides/" class="active">Guides</a>
      <a href="/docs/html/api/">API Docs</a>
      <a href="https://spices.turmeric-lang.com">Spices</a>
      <a href="/try">Try It</a>
    </nav>
    <div class="search-wrap">
      <input class="search-input" type="search" placeholder="Filter... (/)" aria-label="Filter guides">
    </div>
  </header>
  <p class="search-no-results">No matching guides.</p>'''

INDEX_FILTER_JS = '''\
  <script>
  document.addEventListener('DOMContentLoaded', function(){
    var input = document.querySelector('.search-input');
    if (!input) return;

    function filter() {
      var q = input.value.trim().toLowerCase();
      var visibleItems = 0;

      document.querySelectorAll('.index-card').forEach(function(card) {
        var items = card.querySelectorAll('ul li');
        var shown = 0;
        items.forEach(function(li) {
          var match = !q || li.textContent.toLowerCase().includes(q);
          li.style.display = match ? '' : 'none';
          if (match) shown++;
        });
        // A card with no matching guides drops out entirely.
        card.style.display = (!q || shown > 0) ? 'block' : 'none';
        visibleItems += shown;
      });

      // Sync sidebar category links with card visibility.
      document.querySelectorAll('.sidebar a[href^="#"]').forEach(function(link) {
        var target = document.getElementById(link.getAttribute('href').slice(1));
        link.parentElement.style.display =
          (!target || target.style.display !== 'none') ? '' : 'none';
      });

      var noResults = document.querySelector('.search-no-results');
      if (noResults) {
        noResults.style.display = (q && visibleItems === 0) ? 'block' : 'none';
      }
    }

    input.addEventListener('input', filter);

    // Clear on Escape
    input.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') { input.value = ''; filter(); input.blur(); }
    });

    // Focus the filter with '/' (when not already typing somewhere)
    document.addEventListener('keydown', function(e) {
      if (e.key === '/' && document.activeElement !== input &&
          document.activeElement.tagName !== 'INPUT' &&
          document.activeElement.tagName !== 'TEXTAREA') {
        e.preventDefault();
        input.focus();
      }
    });
  });
  </script>'''

SIDEBAR_TOGGLE_JS = '''\
  <div class="sidebar-overlay"></div>
  <script>
    document.addEventListener('DOMContentLoaded', function(){
      var btn = document.querySelector('.hamburger');
      var sidebar = document.querySelector('.sidebar');
      var overlay = document.querySelector('.sidebar-overlay');
      if (!btn || !sidebar) return;
      function open() { sidebar.classList.add('is-open'); overlay && overlay.classList.add('is-open'); }
      function close() { sidebar.classList.remove('is-open'); overlay && overlay.classList.remove('is-open'); }
      btn.addEventListener('click', function(){ sidebar.classList.contains('is-open') ? close() : open(); });
      overlay && overlay.addEventListener('click', close);
    });
  </script>'''

SYNTAX_TOGGLE_JS = '''\
  <script>
  (function(){
    function applyToggle(toggle, syntax) {
      var card = toggle.closest('.code-card');
      if (!card) return;
      toggle.querySelectorAll('.seg-btn').forEach(function(btn){
        var active = btn.dataset.syntax === syntax;
        btn.classList.toggle('active', active);
        btn.setAttribute('aria-selected', active ? 'true' : 'false');
      });
      card.querySelectorAll('.code-version').forEach(function(v){
        v.style.display = v.classList.contains(syntax + '-version') ? '' : 'none';
      });
    }

    // ST1.5: restore stored preference across all cards on load
    var stored = localStorage.getItem('guide-syntax');
    if (stored) {
      document.querySelectorAll('.code-syntax-toggle').forEach(function(t){ applyToggle(t, stored); });
    }

    document.querySelectorAll('.code-syntax-toggle').forEach(function(toggle){
      // Click handler
      toggle.addEventListener('click', function(e){
        if (!e.target.classList.contains('seg-btn')) return;
        var syntax = e.target.dataset.syntax;
        document.querySelectorAll('.code-syntax-toggle').forEach(function(t){ applyToggle(t, syntax); });
        localStorage.setItem('guide-syntax', syntax);
      });
      // ST5.2: arrow-key navigation within the tablist
      toggle.addEventListener('keydown', function(e){
        var btns = Array.from(toggle.querySelectorAll('.seg-btn'));
        var idx = btns.indexOf(document.activeElement);
        if (idx === -1) return;
        if (e.key === 'ArrowRight'){ btns[(idx+1)%btns.length].focus(); e.preventDefault(); }
        if (e.key === 'ArrowLeft') { btns[(idx-1+btns.length)%btns.length].focus(); e.preventDefault(); }
      });
    });
  })();
  </script>'''

TURMERIC_HIGHLIGHT_JS = '''\
  <script>
  (function(){
    var KW = new Set([
      'defn','defmacro','defstruct','definstance','defdata','defgadt','defclass','def','let','let*','letrec',
      'if','cond','when','unless','do','begin','and','or','not',
      'fn','lambda','async','await','match','case',
      'quote','quasiquote','unquote','for','while','loop','do-m',
      'set!','try','catch','finally','with','use',
      'import','export','module','require','provide',
      'cons','car','cdr','nil-value','some','none','ok','err',
      'map','filter','reduce','apply','return','yield','raise','throw',
      'coerce','cast','type-of','any',
    ]);
    function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
    function hl(code){
      var out='', i=0, n=code.length;
      while(i<n){
        var c=code[i];
        // Line comment
        if(c===';'){
          var e=code.indexOf('\\n',i); if(e===-1)e=n;
          out+='<span class="hl-comment">'+esc(code.slice(i,e))+'</span>'; i=e; continue;
        }
        // String
        if(c==='"'){
          var j=i+1;
          while(j<n){if(code[j]==='\\\\'){j+=2;continue;}if(code[j]==='"'){j++;break;}j++;}
          out+='<span class="hl-string">'+esc(code.slice(i,j))+'</span>'; i=j; continue;
        }
        // Type annotation :keyword
        if(c===':'&&i+1<n&&/[a-zA-Z_]/.test(code[i+1])){
          var j=i+1;
          while(j<n&&/[a-zA-Z0-9_\\-?!]/.test(code[j]))j++;
          out+='<span class="hl-type">'+esc(code.slice(i,j))+'</span>'; i=j; continue;
        }
        // Number (integer or float, possibly negative)
        if(/[0-9]/.test(c)||(c==='-'&&i+1<n&&/[0-9]/.test(code[i+1]))){
          var j=i; if(code[j]==='-')j++;
          while(j<n&&/[0-9a-fA-FxX_\\.]/.test(code[j]))j++;
          out+='<span class="hl-number">'+esc(code.slice(i,j))+'</span>'; i=j; continue;
        }
        // Symbol / identifier
        if(/[a-zA-Z_\\-!?*+<>=\\/&%^~#@]/.test(c)){
          var j=i;
          while(j<n&&/[a-zA-Z0-9_\\-!?*+<>=\\/&%^~#@\\.]/.test(code[j]))j++;
          var sym=code.slice(i,j);
          if(sym==='true'||sym==='false'||sym==='nil'){
            out+='<span class="hl-number">'+esc(sym)+'</span>';
          } else if(KW.has(sym)){
            out+='<span class="hl-keyword">'+esc(sym)+'</span>';
          } else {
            out+=esc(sym);
          }
          i=j; continue;
        }
        out+=esc(c); i++;
      }
      return out;
    }
    document.querySelectorAll('pre code.language-turmeric, pre code.language-sweet-exp').forEach(function(el){
      el.innerHTML=hl(el.textContent);
    });
  })();
  </script>'''

GUIDE_CSS = '''\
    .guide-content h1 { font-size:1.75rem; color:var(--gold-bright); margin-bottom:1.5rem; padding-bottom:0.75rem; border-bottom:1px solid var(--border); }
    .guide-content h2 { font-size:1.2rem; color:var(--text-primary); margin:2rem 0 0.75rem; }
    .guide-content h3 { font-size:1rem; color:var(--gold-bright); margin:1.5rem 0 0.5rem; }
    .guide-content p  { margin-bottom:1rem; }
    .guide-content ul, .guide-content ol { margin:0 0 1rem 1.5rem; }
    .guide-content li { margin:0.25rem 0; }
    .guide-content code { font-family:"Iosevka","Fira Code",monospace; font-size:0.85em; background:var(--bg-panel); border:1px solid var(--border); border-radius:3px; padding:0.1em 0.35em; }
    .guide-content pre { background:var(--bg-panel); border:1px solid var(--border); border-radius:4px; padding:1rem; overflow-x:auto; margin-bottom:1rem; }
    .guide-content pre code { background:none; border:none; padding:0; font-size:0.85rem; }
    .guide-content blockquote { border-left:3px solid var(--gold); padding-left:1rem; color:var(--text-sec); margin:1rem 0; }
    .guide-content table { border-collapse:collapse; width:100%; margin-bottom:1rem; font-size:0.9rem; }
    .guide-content th { background:var(--bg-surface); border:1px solid var(--border); padding:0.5rem 0.75rem; text-align:left; color:var(--gold-bright); }
    .guide-content td { border:1px solid var(--border); padding:0.5rem 0.75rem; }
    .guide-content a { color:var(--gold-bright); }
    .guide-content strong { color:var(--text-primary); }
    .guide-toc { border:1px solid var(--border); border-radius:6px; background:var(--bg-panel); padding:0.85rem 1.15rem 0.95rem; margin:0 0 2rem; }
    .guide-toc-title { font-family:system-ui; font-size:0.7rem; text-transform:uppercase; letter-spacing:0.09em; color:var(--text-sec); margin-bottom:0.5rem; }
    .guide-toc ul { margin:0 0 0 1.15rem; padding:0; }
    .guide-toc ul ul { margin-top:0.15rem; margin-bottom:0.15rem; }
    .guide-toc li { margin:0.2rem 0; font-size:0.875rem; }
    .guide-toc a { color:var(--gold-bright); }
    .guide-toc a:hover { color:var(--gold); }
    .hl-comment { color:#48433D; font-style:italic; }
    .hl-string  { color:#D9735A; }
    .hl-number  { color:#A8C98A; }
    .hl-keyword { color:#EFA030; font-weight:bold; }
    .hl-type    { color:#7AC4B8; }
    .code-toggle { border:1px solid var(--border); border-radius:4px; margin-bottom:1rem; overflow:hidden; }
    .code-card-bar { background:var(--bg-surface); border-bottom:1px solid var(--border); padding:0.35rem 0.75rem; display:flex; align-items:center; }
    .code-syntax-toggle { margin-left:auto; display:flex; border:1px solid var(--border); border-radius:4px; overflow:hidden; font-family:"Iosevka","Fira Code",monospace; font-size:11px; }
    .seg-btn { padding:3px 10px; background:transparent; color:var(--text-sec); border:none; cursor:pointer; transition:all 0.14s; }
    .seg-btn:hover { color:var(--text-primary); }
    .seg-btn.active { color:var(--gold-bright); background:var(--bg-hover); }
    .code-card-body { }
    .code-version { }
    .guide-content .code-toggle pre { border:none; border-radius:0; margin-bottom:0; }'''


def inject_syntax_toggles(body_html: str) -> str:
    """Wrap adjacent turmeric+sweet-exp block pairs in a syntax-toggle widget."""
    pattern = re.compile(
        r'(<pre><code class="language-turmeric">.*?</code></pre>)'
        r'(\s*)'
        r'(<pre><code class="language-sweet-exp">.*?</code></pre>)',
        re.DOTALL,
    )

    def wrap_pair(m: re.Match) -> str:
        tur_block = m.group(1)
        sweet_block = m.group(3)
        return (
            '<div class="code-card code-toggle">'
            '<div class="code-card-bar">'
            '<div class="code-syntax-toggle" role="tablist">'
            '<button class="seg-btn active" data-syntax="turmeric"'
            ' role="tab" aria-selected="true">turmeric</button>'
            '<button class="seg-btn" data-syntax="sweet-exp"'
            ' role="tab" aria-selected="false">sweet-exp</button>'
            '</div>'
            '</div>'
            '<div class="code-card-body">'
            f'<div class="code-version turmeric-version" role="tabpanel">{tur_block}</div>'
            f'<div class="code-version sweet-exp-version" role="tabpanel"'
            f' style="display:none">{sweet_block}</div>'
            '</div>'
            '</div>'
        )

    return pattern.sub(wrap_pair, body_html)


# A hand-written "## Table of Contents" (or "## Contents") heading plus the
# list that follows it, up to the next heading (or end of file). We strip these
# from the source before rendering so the auto-generated in-body Contents box is
# the single source of truth -- no stale, hand-maintained duplicate.
_MANUAL_TOC_RE = re.compile(
    r'^#{1,6}[ \t]+(?:table of contents|contents)[ \t]*\n'  # the TOC heading
    r'(?:(?!^#{1,6}[ \t]).*\n?)*',                          # non-heading lines
    re.IGNORECASE | re.MULTILINE,
)


def strip_manual_toc(text: str) -> str:
    """Remove a hand-written Table of Contents section from guide markdown."""
    return _MANUAL_TOC_RE.sub('', text, count=1)


def _count_toc_entries(tokens: list) -> int:
    return sum(1 + _count_toc_entries(t.get('children', [])) for t in tokens)


def toc_tokens_to_inline(tokens: list) -> str:
    """Render toc_tokens into a nested <ul> for the in-body Contents box."""
    items = []
    for tok in tokens:
        anchor = tok.get('id', '')
        name = tok.get('name', '')
        children = tok.get('children', [])
        sub = toc_tokens_to_inline(children) if children else ''
        items.append(f'<li><a href="#{anchor}">{name}</a>{sub}</li>')
    return '<ul>' + ''.join(items) + '</ul>'


def build_inline_toc(toc_tokens: list) -> str:
    """Build the in-body "Contents" navigation box from a doc's toc_tokens.

    The single top-level H1 (the page title) is elided -- its subsections
    become the roots. Returns '' when there are fewer than 3 entries, so short
    guides don't get a redundant one- or two-line box.
    """
    roots = toc_tokens
    if len(roots) == 1 and roots[0].get('level') == 1:
        roots = roots[0].get('children', [])
    if _count_toc_entries(roots) < 3:
        return ''
    return (
        '<nav class="guide-toc" aria-label="Table of contents">'
        '<div class="guide-toc-title">Contents</div>'
        f'{toc_tokens_to_inline(roots)}'
        '</nav>'
    )


def toc_tokens_to_sidebar(tokens: list) -> str:
    """Recursively render toc_tokens into sidebar <li> elements."""
    items = []
    for tok in tokens:
        anchor = tok.get('id', '')
        name = tok.get('name', '')
        level = tok.get('level', 2)
        indent = 'padding-left:0.75rem;' if level > 2 else ''
        color = 'color:var(--text-sec);' if level > 2 else ''
        items.append(
            f'<li style="{indent}"><a href="#{anchor}" style="{color}">{name}</a></li>'
        )
        children = tok.get('children', [])
        if children:
            items.append(toc_tokens_to_sidebar(children))
    return '\n'.join(items)


def render_guide(stem: str, src: Path, out: Path, all_stems: set, meta: dict | None = None) -> None:
    raw = src.read_text(encoding='utf-8')
    fm_meta, text = parse_front_matter(raw)
    if meta is None:
        meta = fm_meta

    text = re.sub(r'^(`{3,})(turmeric|sweet-exp)\s+no-check\b[^\n]*', r'\1\2', text,
                  flags=re.MULTILINE)
    text = strip_manual_toc(text)

    conv = md_lib.Markdown(extensions=['fenced_code', 'tables', 'toc'],
                            extension_configs={'toc': {'permalink': False}})
    body_html = conv.convert(text)
    body_html = inject_syntax_toggles(body_html)

    # Rewrite .md links to .html (only local, non-absolute links).
    # Must run AFTER markdown conversion -- the source uses `[text](file.md)`
    # syntax, which only becomes `href="file.md"` after the converter runs.
    def rewrite_md_link(m: re.Match) -> str:
        href = m.group(1)
        if href.startswith(('http://', 'https://', '/', '..')):
            return m.group(0)
        return f'href="{Path(href).stem}.html"'

    body_html = re.sub(r'href="([^"]+\.md)"', rewrite_md_link, body_html)
    toc_tokens = getattr(conv, 'toc_tokens', [])

    # In-body "Contents" box, inserted right after the page title (first <h1>),
    # or at the top of the content when the guide has no <h1>.
    inline_toc = build_inline_toc(toc_tokens)
    if inline_toc:
        h1_end = re.search(r'</h1>', body_html)
        if h1_end:
            i = h1_end.end()
            body_html = body_html[:i] + '\n' + inline_toc + body_html[i:]
        else:
            body_html = inline_toc + body_html

    fm_title = meta.get('title', '').strip() if meta else ''
    if fm_title:
        title = fm_title
    else:
        title_match = re.match(r'^#\s+(.+)', text, re.MULTILINE)
        title = title_match.group(1) if title_match else stem.replace('-', ' ').title()

    sidebar_items = toc_tokens_to_sidebar(toc_tokens)
    sidebar_html = f'''\
      <a class="sidebar-back" href="/">← Back to home</a>
      <div style="margin-bottom:1.25rem">
        <a href="index.html" style="font-size:0.8rem;color:var(--text-sec)">← All Guides</a>
      </div>
      <hr class="sidebar-divider">
      <h3>On this page</h3>
      <ul>{sidebar_items}</ul>
{SIDEBAR_GLOBALS}'''

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title} | Turmeric Guides</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.svg">
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link rel="preconnect" href="https://cdn.jsdelivr.net">
  <link rel="preload" as="style" href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500&display=swap" onload="this.rel='stylesheet'">
  <link rel="preload" as="style" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/400.css" onload="this.rel='stylesheet'">
  <link rel="preload" as="style" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/500.css" onload="this.rel='stylesheet'">
  <noscript>
  <link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500&display=swap">
  <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/400.css">
  <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/500.css">
  </noscript>
  <link rel="stylesheet" href="{STYLE_REL}">
  <style>
{GUIDE_CSS}
  </style>
</head>
<body>
{PAGE_HEADER}
{SIDEBAR_TOGGLE_JS}
  <div class="page-layout">
    <div class="sidebar">
      {sidebar_html}
    </div>
    <div class="content guide-content">
      {body_html}
    </div>
  </div>
  <footer class="site-footer">
    Auto-generated by <code>tools/genguides.py</code> &mdash; source: <a href="https://github.com/rjungemann/turmeric/blob/main/docs/guides/{stem}.md"><code>docs/guides/{stem}.md</code></a>
  </footer>
{TURMERIC_HIGHLIGHT_JS}
{SYNTAX_TOGGLE_JS}
</body>
</html>
'''
    out.write_text(html, encoding='utf-8')
    print(f'  {stem}.html')


def _fmt_inline(text: str) -> str:
    """HTML-escape text and convert backtick spans to <code> elements."""
    text = _html.escape(text)
    text = re.sub(r'`([^`]+)`', lambda m: f'<code>{m.group(1)}</code>', text)
    return text


def _fmt_desc(text: str) -> str:
    """Normalize and render a guide description for the index page.

    - Replaces em dashes with '--'
    - HTML-escapes special characters
    - Converts backtick spans to <code> elements
    """
    text = text.replace('—', '--')
    return _fmt_inline(text)


def render_index(categories: list[dict], all_stems: set[str], out_dir: Path,
                 recent: list[dict] | None = None) -> None:
    categorized_stems = {g['stem'] for c in categories for g in c['guides']}
    recent = recent or []

    def guide_item(g: dict) -> str:
        if g['stem'] not in all_stems:
            return ''
        return (f'<li><a href="{g["stem"]}.html">{_fmt_inline(g["label"])}</a>'
                f'<span style="color:var(--text-sec)"> -- {_fmt_desc(g["desc"])}</span></li>')

    cards = []
    for cat in categories:
        items = [s for g in cat['guides'] if (s := guide_item(g))]
        if not items:
            continue
        slug = re.sub(r'\s+', '-', cat['name'].lower())
        cards.append(f'''\
    <div class="index-card" style="display:block" id="{slug}">
      <h3 style="font-family:system-ui;font-size:0.9rem;margin-bottom:0.5rem">{cat['name']}</h3>
      <ul style="list-style:none;margin:0">
        {"".join(items)}
      </ul>
    </div>''')

    uncategorized = sorted(all_stems - categorized_stems)
    if uncategorized:
        items = [f'<li><a href="{s}.html">{s}</a></li>'
                 for s in uncategorized]
        cards.append(f'''\
    <div class="index-card" style="display:block">
      <h3 style="font-family:system-ui;font-size:0.9rem;margin-bottom:0.5rem">Other</h3>
      <ul style="list-style:none;margin:0">{"".join(items)}</ul>
    </div>''')

    sidebar_cats_list = [
        f'<li><a href="#{re.sub(r" +", "-", c["name"].lower())}">{c["name"]}</a></li>'
        for c in categories if any(g['stem'] in all_stems for g in c['guides'])
    ]
    if recent:
        sidebar_cats_list.insert(0, '<li><a href="#recently-added">Recently Added</a></li>')
    sidebar_cats = '\n'.join(sidebar_cats_list)

    recent_html = ''
    if recent:
        recent_items = ''.join(
            f'<li><a href="{r["stem"]}.html">{_fmt_inline(r["label"])}</a>'
            f'<span style="color:var(--text-sec)"> -- {r["date"]}</span></li>'
            for r in recent
        )
        recent_html = f'''\
      <div class="index-card" style="display:block;margin-bottom:1.5rem" id="recently-added">
        <h3 style="font-family:system-ui;font-size:0.9rem;margin-bottom:0.5rem">Recently Added</h3>
        <ul style="list-style:none;margin:0">{recent_items}</ul>
      </div>'''

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Guides | Turmeric</title>
  <link rel="icon" type="image/svg+xml" href="/favicon.svg">
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link rel="preconnect" href="https://cdn.jsdelivr.net">
  <link rel="preload" as="style" href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500&display=swap" onload="this.rel='stylesheet'">
  <link rel="preload" as="style" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/400.css" onload="this.rel='stylesheet'">
  <link rel="preload" as="style" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/500.css" onload="this.rel='stylesheet'">
  <noscript>
  <link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500&display=swap">
  <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/400.css">
  <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@fontsource/iosevka@5/500.css">
  </noscript>
  <link rel="stylesheet" href="{STYLE_REL}">
  <style>
    .index-card ul li {{ margin:0.3rem 0; font-size:0.875rem; }}
    .index-card ul li a {{ color:var(--text-primary); }}
    .index-card ul li a:hover {{ color:var(--gold); }}
  </style>
</head>
<body>
{INDEX_PAGE_HEADER}
{SIDEBAR_TOGGLE_JS}
  <div class="page-layout">
    <div class="sidebar">
      <a class="sidebar-back" href="/">← Back to home</a>
      <hr class="sidebar-divider">
      <h3>Categories</h3>
      <ul>{sidebar_cats}</ul>
{SIDEBAR_GLOBALS}
    </div>
    <div class="content">
      <div class="module-heading">
        <h1 style="font-family:system-ui;color:var(--gold)">Guides</h1>
        <div class="module-path">Tutorials, how-tos, and in-depth feature guides for Turmeric</div>
      </div>
{recent_html}
      <div class="index-grid">
        {"".join(cards)}
      </div>
    </div>
  </div>
  <footer class="site-footer">
    Auto-generated by <code>tools/genguides.py</code>
  </footer>
{TURMERIC_HIGHLIGHT_JS}
{INDEX_FILTER_JS}
</body>
</html>
'''
    (out_dir / 'index.html').write_text(html, encoding='utf-8')
    print('  index.html')


def main() -> None:
    p = argparse.ArgumentParser(description='Render Turmeric guide markdown to HTML.')
    p.add_argument('guides_dir', help='Path to docs/guides/ directory')
    p.add_argument('--out', default=None, help='Output directory (default: same as guides_dir)')
    args = p.parse_args()

    guides_dir = Path(args.guides_dir)
    out_dir = Path(args.out) if args.out else guides_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    md_files = sorted(f for f in guides_dir.glob('*.md') if f.stem != 'README')
    all_stems = {f.stem for f in md_files}

    meta_by_stem: dict = {}
    for src in md_files:
        fm, _ = parse_front_matter(src.read_text(encoding='utf-8'))
        meta_by_stem[src.stem] = fm

    categories = build_categories_from_meta(meta_by_stem, all_stems)

    # Find the git repo root by walking up from the guides dir.
    repo_root = guides_dir.resolve()
    while repo_root != repo_root.parent and not (repo_root / '.git').exists():
        repo_root = repo_root.parent

    dated: list[tuple[str, Path]] = []
    for src in md_files:
        d = get_creation_date(src, repo_root)
        if d:
            dated.append((d, src))
    dated.sort(key=lambda t: t[0], reverse=True)
    recent = []
    for date, src in dated[:10]:
        m = meta_by_stem.get(src.stem, {})
        label = m.get('title', src.stem.replace('-', ' ').title()).strip()
        recent.append({'stem': src.stem, 'label': label, 'date': date})

    print('Generating guides:')
    for src in md_files:
        render_guide(src.stem, src, out_dir / f'{src.stem}.html', all_stems,
                     meta_by_stem.get(src.stem, {}))
    render_index(categories, all_stems, out_dir, recent=recent)
    print(f'Done: {len(md_files)} guides + index → {out_dir}')


if __name__ == '__main__':
    main()
