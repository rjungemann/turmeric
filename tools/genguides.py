#!/usr/bin/env python3
"""
tools/genguides.py -- Render Turmeric guide markdown files to HTML.

Usage:
    python3 tools/genguides.py docs/guides/ [--out docs/html/guides/]
"""

import argparse
import re
import sys
from pathlib import Path

import markdown as md_lib

STYLE_REL = '../api/style.css'

PAGE_HEADER = '''\
  <header class="site-header">
    <button class="hamburger" aria-label="Toggle navigation">
      <span></span><span></span><span></span>
    </button>
    <a class="brand" href="/">turmeric</a>
    <nav>
      <a href="/docs/html/guides/">Guides</a>
      <a href="/docs/html/api/">API Docs</a>
      <a href="/try">Try It</a>
    </nav>
  </header>'''

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
    document.querySelectorAll('pre code.language-turmeric').forEach(function(el){
      el.innerHTML=hl(el.textContent);
    });
  })();
  </script>'''

GUIDE_CSS = '''\
    .guide-content h1 { font-size:1.75rem; color:var(--gold); margin-bottom:1.5rem; padding-bottom:0.75rem; border-bottom:1px solid var(--border); }
    .guide-content h2 { font-size:1.2rem; color:var(--cream); margin:2rem 0 0.75rem; }
    .guide-content h3 { font-size:1rem; color:var(--gold); margin:1.5rem 0 0.5rem; }
    .guide-content p  { margin-bottom:1rem; }
    .guide-content ul, .guide-content ol { margin:0 0 1rem 1.5rem; }
    .guide-content li { margin:0.25rem 0; }
    .guide-content code { font-family:"JetBrains Mono","Fira Code",monospace; font-size:0.85em; background:var(--bg-code); border:1px solid var(--border); border-radius:3px; padding:0.1em 0.35em; }
    .guide-content pre { background:var(--bg-code); border:1px solid var(--border); border-radius:4px; padding:1rem; overflow-x:auto; margin-bottom:1rem; }
    .guide-content pre code { background:none; border:none; padding:0; font-size:0.85rem; }
    .guide-content blockquote { border-left:3px solid var(--gold); padding-left:1rem; color:var(--faint); margin:1rem 0; }
    .guide-content table { border-collapse:collapse; width:100%; margin-bottom:1rem; font-size:0.9rem; }
    .guide-content th { background:var(--bg-card); border:1px solid var(--border); padding:0.5rem 0.75rem; text-align:left; color:var(--gold); }
    .guide-content td { border:1px solid var(--border); padding:0.5rem 0.75rem; }
    .guide-content a { color:var(--gold); }
    .guide-content strong { color:var(--cream); }
    .hl-comment { color:#48433D; font-style:italic; }
    .hl-string  { color:#D9735A; }
    .hl-number  { color:#A8C98A; }
    .hl-keyword { color:#EFA030; font-weight:bold; }
    .hl-type    { color:#7AC4B8; }'''


def parse_readme(readme_path: Path) -> list[dict]:
    """
    Parse README.md into a list of categories.
    Each category: { 'name': str, 'guides': [{'stem', 'label', 'desc'}] }
    Stop at the first '---' separator (below the categories).
    """
    text = readme_path.read_text(encoding='utf-8')
    categories = []
    current: dict | None = None

    for line in text.splitlines():
        if line.strip() == '---':
            break
        m_cat = re.match(r'^## (.+)', line)
        if m_cat:
            current = {'name': m_cat.group(1), 'guides': []}
            categories.append(current)
            continue
        # Match: - **[label](href)** — desc  (with either -- or —)
        m_item = re.match(r'-\s+\*\*\[([^\]]+)\]\(([^)]+)\)\*\*\s+[--—]+\s+(.+)', line)
        if m_item and current is not None:
            label, href, desc = m_item.group(1), m_item.group(2), m_item.group(3)
            if not href.startswith('..') and href.endswith('.md'):
                stem = Path(href).stem
                current['guides'].append({'stem': stem, 'label': label.replace('.md', ''), 'desc': desc})

    return [c for c in categories if c['guides']]


def toc_tokens_to_sidebar(tokens: list) -> str:
    """Recursively render toc_tokens into sidebar <li> elements."""
    items = []
    for tok in tokens:
        anchor = tok.get('id', '')
        name = tok.get('name', '')
        level = tok.get('level', 2)
        indent = 'padding-left:0.75rem;' if level > 2 else ''
        color = 'color:var(--faint);' if level > 2 else ''
        items.append(
            f'<li style="{indent}"><a href="#{anchor}" style="{color}">{name}</a></li>'
        )
        children = tok.get('children', [])
        if children:
            items.append(toc_tokens_to_sidebar(children))
    return '\n'.join(items)


def render_guide(stem: str, src: Path, out: Path, all_stems: set[str]) -> None:
    text = src.read_text(encoding='utf-8')

    # Rewrite .md links to .html (only local, non-absolute links)
    def rewrite_md_link(m: re.Match) -> str:
        href = m.group(1)
        if href.startswith('http') or href.startswith('/') or href.startswith('..'):
            return m.group(0)
        return f'href="{Path(href).stem}.html"'

    text = re.sub(r'href="([^"]+\.md)"', rewrite_md_link, text)

    conv = md_lib.Markdown(extensions=['fenced_code', 'tables', 'toc'],
                            extension_configs={'toc': {'permalink': False}})
    body_html = conv.convert(text)
    toc_tokens = getattr(conv, 'toc_tokens', [])

    title_match = re.match(r'^#\s+(.+)', src.read_text(encoding='utf-8'), re.MULTILINE)
    title = title_match.group(1) if title_match else stem.replace('-', ' ').title()

    sidebar_items = toc_tokens_to_sidebar(toc_tokens)
    sidebar_html = f'''\
      <div style="margin-bottom:1.25rem">
        <a href="index.html" style="font-size:0.8rem;color:var(--faint)">← All Guides</a>
      </div>
      <h3>On this page</h3>
      <ul>{sidebar_items}</ul>'''

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title} | Turmeric Guides</title>
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
    Auto-generated by <code>tools/genguides.py</code> &mdash; source: <code>docs/guides/{stem}.md</code>
  </footer>
{TURMERIC_HIGHLIGHT_JS}
</body>
</html>
'''
    out.write_text(html, encoding='utf-8')
    print(f'  {stem}.html')


def render_index(categories: list[dict], all_stems: set[str], out_dir: Path) -> None:
    categorized_stems = {g['stem'] for c in categories for g in c['guides']}

    def guide_item(g: dict) -> str:
        if g['stem'] not in all_stems:
            return ''
        return (f'<li><a href="{g["stem"]}.html">{g["label"]}</a>'
                f'<span style="color:var(--faint)"> — {g["desc"]}</span></li>')

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

    sidebar_cats = '\n'.join(
        f'<li><a href="#{re.sub(r" +", "-", c["name"].lower())}">{c["name"]}</a></li>'
        for c in categories if any(g['stem'] in all_stems for g in c['guides'])
    )

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Guides | Turmeric</title>
  <link rel="stylesheet" href="{STYLE_REL}">
  <style>
    .index-card ul li {{ margin:0.3rem 0; font-size:0.875rem; }}
    .index-card ul li a {{ color:var(--cream); }}
    .index-card ul li a:hover {{ color:var(--gold); }}
  </style>
</head>
<body>
{PAGE_HEADER}
{SIDEBAR_TOGGLE_JS}
  <div class="page-layout">
    <div class="sidebar">
      <h3>Categories</h3>
      <ul>{sidebar_cats}</ul>
    </div>
    <div class="content">
      <div class="module-heading">
        <h1 style="font-family:system-ui;color:var(--gold)">Guides</h1>
        <div class="module-path">Tutorials, how-tos, and in-depth feature guides for Turmeric</div>
      </div>
      <div class="index-grid">
        {"".join(cards)}
      </div>
    </div>
  </div>
  <footer class="site-footer">
    Auto-generated by <code>tools/genguides.py</code>
  </footer>
{TURMERIC_HIGHLIGHT_JS}
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

    readme = guides_dir / 'README.md'
    categories = parse_readme(readme) if readme.exists() else []

    md_files = sorted(f for f in guides_dir.glob('*.md') if f.stem != 'README')
    all_stems = {f.stem for f in md_files}

    print('Generating guides:')
    for src in md_files:
        render_guide(src.stem, src, out_dir / f'{src.stem}.html', all_stems)
    render_index(categories, all_stems, out_dir)
    print(f'Done: {len(md_files)} guides + index → {out_dir}')


if __name__ == '__main__':
    main()
