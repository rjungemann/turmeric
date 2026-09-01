#!/usr/bin/env python3
"""
tools/genguides.py -- Render Turmeric guide markdown files to HTML.

Usage:
    python3 tools/genguides.py docs/guides/ [--out docs/html/guides/]
                                            [--emit-pack web/public/docs-pack/]

Two consumers, one rendering pass. `build_guide_body` converts a guide's
markdown into its article body exactly once; `render_guide` wraps that body in
site chrome for docs/html/guides/, and `emit_pack_fragment` writes the same
body -- chrome-free, links rewritten into the pack's `#doc=` URL space -- into
the docs pack that Try Turmeric renders in-app and precaches for offline use.
The two outputs cannot drift because there is only one renderer.
"""

import argparse
import html as _html
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import packlib  # noqa: E402  (sibling module, path fixed up just above)

try:
    import markdown as md_lib
except ImportError:  # pragma: no cover -- preflight, not a code path
    sys.exit(
        "error: tools/genguides.py needs the 'markdown' package\n"
        "       install it with:  python3 -m pip install -r tools/requirements.txt\n"
        "       (or:  python3 -m pip install markdown)"
    )


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

# Native `title` tooltips for the site chrome (topbar, sidebar, footer). Keyed
# by site-relative href -- absolute turmeric-lang.com URLs, which the spices
# site uses for cross-site links, are normalized to the same key so all three
# generators describe a page identically. Mirrors LINK_TITLES in web/site.js.
LINK_TITLES = {
    '/':                                      'Turmeric home',
    '/tour':                                  'A guided tour of the language in fourteen stops',
    '/try':                                   'Run Turmeric in your browser -- nothing to install',
    '/trowel':                                'Trowel -- the native Turmeric editor for macOS and Linux',
    '/docs/html/guides/':                     'Guides and tutorials, from quickstart to compiler internals',
    '/docs/html/api/':                        'Generated API reference for the standard library',
    '/docs/html/spices/':                     'Browse Spice packages -- the Turmeric package registry',
    '/roadmap':                               'Planned features, work in progress, and recent milestones',
    '/ci':                                    'Build and test metrics from continuous integration',
    'https://spices.turmeric-lang.com':       'Browse Spice packages -- the Turmeric package registry',
    'https://c.turmeric-lang.com':            'A C interpreter running in your browser',
    'https://github.com/rjungemann/turmeric': 'Turmeric source code on GitHub',
    'https://phasor.space':                   "Roger Jungemann's site",
}

# ---------------------------------------------------------------------------
# Canonical site chrome -- one list per region, four consumers
#
# The topbar and the sidebar name the same set of places on every page of the
# site, whether that page is hand-written (web/*.html, driven by web/site.js)
# or generated (guides here, API docs in gendocs.py, spices in genspices.py).
# Keeping the lists as data in one module is what stops the four surfaces from
# drifting into four different answers to "what is on this site". `web/site.js`
# mirrors NAV_LINKS/SIDEBAR_GROUPS verbatim -- change one, change the other.
# ---------------------------------------------------------------------------

# Topbar links, in order. `active` is matched by label.
NAV_LINKS = [
    ('/tour',                            'Tour'),
    ('/try',                             'Try It'),
    ('/docs/html/guides/',               'Guides'),
    ('/docs/html/api/',                  'API Docs'),
    ('https://spices.turmeric-lang.com', 'Spices'),
    ('/trowel',                          'Trowel'),
]

# Sidebar link groups, in order. The mobile drawer renders the same groups, so
# a phone sees the same site map a desktop sidebar shows.
SIDEBAR_GROUPS = [
    ('Language', [
        ('/tour',   'Tour'),
        ('/trowel', 'Trowel'),
        ('/try',    'Try It'),
    ]),
    ('Ecosystem', [
        ('/docs/html/guides/',               'Guides'),
        ('/docs/html/api/',                  'API Docs'),
        ('https://spices.turmeric-lang.com', 'Spices'),
        ('https://c.turmeric-lang.com',      'C Interpreter'),
    ]),
    ('Community', [
        ('https://github.com/rjungemann/turmeric', 'GitHub'),
        ('/ci',                                    'CI Metrics'),
    ]),
]

# The spices site lives on its own subdomain, so its root-relative hrefs have
# to be re-rooted at the main host. Everything already absolute is left alone.
MAIN_SITE = 'https://turmeric-lang.com'


def _href(href: str, base: str = '') -> str:
    """Re-root a site-relative href onto `base` (used by the spices subdomain)."""
    if not base or not href.startswith('/'):
        return href
    return base.rstrip('/') + href


def build_sidebar_globals(base: str = '', indent: str = '      ') -> str:
    """Render SIDEBAR_GROUPS -- the block every sidebar ends with.

    Emitted after the page's own back link and table of contents, separated
    from them by the one `sidebar-divider` on the page.
    """
    out = [f'{indent}<hr class="sidebar-divider">']
    for heading, links in SIDEBAR_GROUPS:
        out.append(f'{indent}<h3>{heading}</h3>')
        out.append(f'{indent}<ul>')
        for href, label in links:
            out.append(f'{indent}  <li><a href="{_href(href, base)}">{label}</a></li>')
        out.append(f'{indent}</ul>')
    return apply_link_titles('\n'.join(out))


def build_page_header(active: str = '', base: str = '', search: str = '',
                      indent: str = '  ') -> str:
    """Render the topbar -- identical on every page but for the `active` mark.

    `search` is the aria-label for the filter box on the pages that have one
    (the guides and API indexes); pages without a filter simply omit it. The
    Try It entry and the gold CTA both drop out on Try Turmeric itself, where
    every route to /try is a link to the page you are already looking at.
    """
    on_try = active == 'Try It'
    parts = []
    for href, label in NAV_LINKS:
        if on_try and href == '/try':
            continue
        cls = ' class="active"' if label == active else ''
        parts.append(f'<a href="{_href(href, base)}"{cls}>{label}</a>')
    links = ''.join(parts)
    search_html = (
        f'\n{indent}  <div class="search-wrap">'
        f'<input class="search-input" type="search" placeholder="Filter... (/)" '
        f'aria-label="{search}"></div>'
        if search else ''
    )
    cta = ('' if on_try else
           f'\n{indent}    <a href="{_href("/try", base)}" class="btn-gold">Try it</a>')
    github = 'https://github.com/rjungemann/turmeric'
    return apply_link_titles(f'''\
{indent}<header class="site-header">
{indent}  <button class="hamburger" aria-label="Toggle navigation" aria-expanded="false">
{indent}    <span></span><span></span><span></span>
{indent}  </button>
{indent}  <a class="nav-logo" href="{_href('/', base)}">
{indent}    <img src="/logo-icon.svg" width="28" height="28" alt="">
{indent}    <img src="/logo.svg" width="101" height="28" alt="Turmeric">
{indent}  </a>
{indent}  <nav class="nav-links">{links}</nav>{search_html}
{indent}  <div class="nav-right">
{indent}    <a href="{github}" class="btn-ghost">GitHub</a>{cta}
{indent}  </div>
{indent}</header>''')

_A_TAG_RE = re.compile(r'<a\s+([^>]*)href="([^"]+)"([^>]*)>')


def apply_link_titles(html: str, extra: dict | None = None) -> str:
    """Add a native `title` tooltip to every chrome link with a known href.

    Only used on the header/sidebar chrome, never on article bodies: a tooltip
    belongs on a navigation target, not on every prose link that happens to
    point at the same page. Links already carrying a title, and hrefs absent
    from the table, are left alone.
    """
    table = {**LINK_TITLES, **(extra or {})}

    def repl(m):
        before, href, after = m.group(1), m.group(2), m.group(3)
        if 'title=' in before or 'title=' in after:
            return m.group(0)
        key = href.replace('https://turmeric-lang.com', '') or '/'
        title = table.get(href) or table.get(key)
        if not title:
            return m.group(0)
        return f'<a {before}href="{href}"{after} title="{_html.escape(title, quote=True)}">'

    return _A_TAG_RE.sub(repl, html)


SIDEBAR_GLOBALS = build_sidebar_globals()


def build_sidebar(toc: str = '', uplinks: list | None = None, base: str = '',
                  extra_titles: dict | None = None) -> str:
    """Assemble a sidebar in the site's one shape.

    Home link, then the page's own "up" links (All Guides, All Spices, ...),
    then its table of contents, then the divider and the global groups. Every
    page in the site reads top-to-bottom in that order, so the global links sit
    in the same place whether you arrived at a guide, an API module, or a spice.
    """
    out = [f'      <a class="sidebar-back" href="{_href("/", base)}">Home</a>']
    if uplinks:
        items = ''.join(f'<a href="{href}">{label}</a>' for href, label in uplinks)
        out.append(f'      <div class="sidebar-uplinks">{items}</div>')
    if toc:
        out.append(toc.rstrip())
    out.append(build_sidebar_globals(base))
    return apply_link_titles('\n'.join(out), extra=extra_titles)

PAGE_HEADER = build_page_header(active='Guides')

INDEX_PAGE_HEADER = (build_page_header(active='Guides', search='Filter guides')
                     + '\n  <p class="search-no-results">No matching guides.</p>')

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

# The mobile drawer. One implementation, shared by every generated page and
# mirrored by `web/site.js` for the hand-written ones, so the hamburger does
# the same four things everywhere: toggle, close on overlay, close on Escape,
# close after following a link.
SIDEBAR_DRAWER_JS = '''\
  <div class="sidebar-overlay"></div>
  <script>
    document.addEventListener('DOMContentLoaded', function(){
      var btn = document.querySelector('.hamburger');
      var sidebar = document.querySelector('.sidebar');
      var overlay = document.querySelector('.sidebar-overlay');
      if (!btn || !sidebar) return;
      function setOpen(open) {
        sidebar.classList.toggle('is-open', open);
        overlay && overlay.classList.toggle('is-open', open);
        btn.setAttribute('aria-expanded', String(open));
      }
      btn.addEventListener('click', function(){
        setOpen(!sidebar.classList.contains('is-open'));
      });
      overlay && overlay.addEventListener('click', function(){ setOpen(false); });
      document.addEventListener('keydown', function(e){
        if (e.key === 'Escape') setOpen(false);
      });
      sidebar.addEventListener('click', function(e){
        if (e.target.closest('a')) setOpen(false);
      });
    });
  </script>'''

# Kept under the old name for the two importers that still spell it this way.
SIDEBAR_TOGGLE_JS = SIDEBAR_DRAWER_JS

# ---------------------------------------------------------------------------
# Guide runtime -- one source, three consumers
#
# A rendered guide body needs two behaviours to look right: Turmeric syntax
# highlighting on its code blocks, and the turmeric/sweet-exp segmented toggle
# on paired blocks. Those behaviours are needed by the site pages under
# docs/html/guides/, by the spice pages genspices.py renders, and by Try
# Turmeric's in-app docs pane, which renders the very same bodies out of the
# docs pack.
#
# So GUIDE_JS_CORE below is the only copy. The site pages inline it and call
# into it immediately (GUIDE_RUNTIME_JS); the docs pack ships it as guide.js
# and the pane calls the same two entry points against its own subtree after
# each render. Both entry points take a root element and are idempotent, which
# is what makes re-running them on a freshly rendered fragment safe.
# ---------------------------------------------------------------------------

GUIDE_JS_CORE = '''\
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

  // Idempotent: the data-hl stamp means a second pass over the same DOM (the
  // docs pane re-renders on every navigation) cannot double-escape the markup.
  function highlightGuideCode(root){
    var scope = root || document;
    scope.querySelectorAll('pre code.language-turmeric, pre code.language-sweet-exp')
      .forEach(function(el){
        if (el.dataset.hlDone) return;
        el.dataset.hlDone = '1';
        el.innerHTML = hl(el.textContent);
      });
  }

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

  function storedSyntax(){
    try { return localStorage.getItem('guide-syntax'); } catch (e) { return null; }
  }

  function initSyntaxToggles(root){
    var scope = root || document;
    var toggles = scope.querySelectorAll('.code-syntax-toggle');

    // ST1.5: restore the stored preference across every card on load
    var stored = storedSyntax();
    if (stored) toggles.forEach(function(t){ applyToggle(t, stored); });

    toggles.forEach(function(toggle){
      if (toggle.dataset.toggleWired) return;
      toggle.dataset.toggleWired = '1';
      // Click handler
      toggle.addEventListener('click', function(e){
        if (!e.target.classList.contains('seg-btn')) return;
        var syntax = e.target.dataset.syntax;
        document.querySelectorAll('.code-syntax-toggle').forEach(function(t){ applyToggle(t, syntax); });
        try { localStorage.setItem('guide-syntax', syntax); } catch (err) {}
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
  }

  var api = { highlightGuideCode: highlightGuideCode, initSyntaxToggles: initSyntaxToggles };
  if (typeof window !== 'undefined') window.turmericGuide = api;
})();'''

# What a rendered site page carries: the shared core, then the two calls that
# used to be the bodies of TURMERIC_HIGHLIGHT_JS and SYNTAX_TOGGLE_JS.
GUIDE_RUNTIME_JS = '''\
  <script>
''' + GUIDE_JS_CORE + '''
  window.turmericGuide.highlightGuideCode(document);
  window.turmericGuide.initSyntaxToggles(document);
  </script>'''

# Kept under their historical names so genspices.py (and any other caller)
# keeps working; both now expand to the shared runtime, and emitting both into
# one page is harmless because the core is idempotent and self-registering.
TURMERIC_HIGHLIGHT_JS = GUIDE_RUNTIME_JS
SYNTAX_TOGGLE_JS = ''

# Gold leads, green answers -- the same two-colour split the home page uses for
# headline and emphasis, carried into long-form prose so a guide, an API page
# and a spice page all rank their headings by the same pair.
GUIDE_CSS = '''\
    .guide-content h1 { font-size:1.75rem; color:var(--gold-bright); margin-bottom:1.5rem; padding-bottom:0.75rem; border-bottom:1px solid var(--border); }
    .guide-content h2 { font-size:1.2rem; color:var(--gold); margin:2rem 0 0.75rem; }
    .guide-content h3 { font-size:1rem; color:var(--green); margin:1.5rem 0 0.5rem; }
    .guide-content h4 { font-size:0.925rem; color:var(--text-primary); margin:1.25rem 0 0.5rem; }
    .guide-content em { color:var(--green); font-style:italic; }
    .guide-content p  { margin-bottom:1rem; }
    .guide-content ul, .guide-content ol { margin:0 0 1rem 1.5rem; }
    .guide-content li { margin:0.25rem 0; }
    .guide-content code { font-family:"Iosevka","Fira Code",monospace; font-size:0.85em; background:var(--bg-panel); border:1px solid var(--border); border-radius:3px; padding:0.1em 0.35em; }
    .guide-content pre { background:var(--bg-panel); border:1px solid var(--border); border-radius:4px; padding:1rem; overflow-x:auto; margin-bottom:1rem; }
    .guide-content pre code { background:none; border:none; padding:0; font-size:0.85rem; }
    .guide-content blockquote { border-left:3px solid var(--green); padding-left:1rem; color:var(--text-sec); margin:1rem 0; }
    .guide-content table { border-collapse:collapse; width:100%; margin-bottom:1rem; font-size:0.9rem; }
    .guide-content th { background:var(--bg-surface); border:1px solid var(--border); padding:0.5rem 0.75rem; text-align:left; color:var(--gold-bright); }
    .guide-content td { border:1px solid var(--border); padding:0.5rem 0.75rem; }
    .guide-content a { color:var(--gold-bright); }
    .guide-content strong { color:var(--text-primary); }
    .guide-toc { border:1px solid var(--border); border-radius:6px; background:var(--bg-panel); padding:0.85rem 1.15rem 0.95rem; margin:0 0 2rem; }
    .guide-toc { border-left:3px solid var(--green-line); }
    .guide-toc-title { font-family:system-ui; font-size:0.7rem; text-transform:uppercase; letter-spacing:0.09em; color:var(--green); margin-bottom:0.5rem; }
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


_LANG_FENCE_OPEN_RE = re.compile(r'(?m)^(`{3})(turmeric|sweet-exp)([^\n]*)\n')


def _read_fenced_block(text: str, pos: int) -> tuple[str, int]:
    """Read a markdown fenced block whose opening fence's newline is at `pos`.

    Returns (content, end) where `end` is just past the closing fence line.

    A markdown block closes at a column-0 bare ``` line. Turmeric inline-C
    blocks use ``` to toggle a C span (```c opens; ``` or ```) closes) and may
    be indented or written inline, so track the C span and only treat a bare
    ``` as the block close when not inside one. This is the same scan
    tools/check-guide-pairs.py uses to delimit blocks.
    """
    n = len(text)
    i = pos
    line_start = pos
    in_c = False
    while i < n:
        if text.startswith('```', i):
            at_col0 = (i == line_start)
            after = text[i + 3] if i + 3 < n else '\n'
            if in_c:
                in_c = False
                i += 3
                continue
            if after.isalnum():           # info string -> opens a (C) span
                in_c = True
                i += 3
                continue
            if at_col0:                   # bare ``` at column 0 -> block close
                j = text.find('\n', i)
                end = (j + 1) if j != -1 else n
                return text[pos:i], end
            i += 3
            continue
        if text[i] == '\n':
            line_start = i + 1
        i += 1
    return text[pos:], n


def widen_nested_fences(text: str) -> str:
    """Re-fence turmeric/sweet-exp blocks that contain inline-C fences.

    Turmeric's inline-C syntax puts ``` runs *inside* a code block. The project
    style closes an inline-C body with ```) on the same line, which markdown
    ignores -- but a module-level inline-C block legitimately closes with a bare
    ``` at column 0, and python-markdown's fenced_code reads that as the end of
    the *enclosing* turmeric block. Everything after it then renders as prose
    instead of code (docs/guides/thread-pool-guide.md is the case in the tree).

    Widening the enclosing fence to five backticks makes the inner
    three-backtick runs ordinary content, so the block survives intact. Only
    blocks that actually contain a nested run are touched.
    """
    out = []
    pos = 0
    for m in _LANG_FENCE_OPEN_RE.finditer(text):
        if m.start() < pos:
            continue  # inside a block already consumed
        content, end = _read_fenced_block(text, m.end())
        if '```' not in content:
            continue
        out.append(text[pos:m.start()])
        out.append(f'`````{m.group(2)}{m.group(3)}\n{content}`````\n')
        pos = end
    if not out:
        return text
    out.append(text[pos:])
    return ''.join(out)


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
        # `name` is already the heading text, so the tooltip says what the link
        # does (jump within this page) rather than repeating the label. Round
        # -trip through unescape so an entity in the heading is not re-escaped.
        plain = _html.unescape(re.sub(r'<[^>]+>', '', name)).strip()
        tip = _html.escape(f'Jump to {plain}', quote=True)
        items.append(
            f'<li style="{indent}"><a href="#{anchor}" style="{color}" '
            f'title="{tip}">{name}</a></li>'
        )
        children = tok.get('children', [])
        if children:
            items.append(toc_tokens_to_sidebar(children))
    return '\n'.join(items)


def build_guide_body(stem: str, src: Path, meta: dict | None = None) -> dict:
    """Render one guide's markdown to its article body -- the single rendering pass.

    The returned ``body`` still carries the source's raw ``href="other.md"``
    cross-links: each consumer rewrites them into its own URL space
    (``rewrite_links_site`` for docs/html/, ``rewrite_links_pack`` for the docs
    pack). Everything else about the body -- the syntax toggles, the in-body
    Contents box, the heading anchors -- is identical for both, which is what
    keeps the site and the pack from drifting.

    Returns a dict with: stem, title, body, toc_tokens, meta.
    """
    raw = src.read_text(encoding='utf-8')
    fm_meta, text = parse_front_matter(raw)
    if meta is None:
        meta = fm_meta

    text = re.sub(r'^(`{3,})(turmeric|sweet-exp)\s+no-check\b[^\n]*', r'\1\2', text,
                  flags=re.MULTILINE)
    text = strip_manual_toc(text)
    text = widen_nested_fences(text)

    conv = md_lib.Markdown(extensions=['fenced_code', 'tables', 'toc'],
                            extension_configs={'toc': {'permalink': False}})
    body_html = conv.convert(text)
    body_html = inject_syntax_toggles(body_html)
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

    return {
        'stem': stem,
        'title': title,
        'body': body_html,
        'toc_tokens': toc_tokens,
        'meta': meta,
    }


# `[text](other-guide.md)` and `[text](other-guide.md#anchor)`, as they appear
# in the converted HTML. The optional fragment group is what lets a
# deep-linking cross-reference survive the rewrite instead of being left as a
# dead `.md` href.
_MD_HREF_RE = re.compile(r'href="([^"#]+)\.md(#[^"]*)?"')


def rewrite_links_site(body_html: str) -> str:
    """Rewrite `.md` cross-links to the sibling `.html` pages of docs/html/guides/.

    Must run AFTER markdown conversion -- the source uses `[text](file.md)`
    syntax, which only becomes `href="file.md"` once the converter has run.
    """
    def rewrite(m: re.Match) -> str:
        href, frag = m.group(1), m.group(2) or ''
        if href.startswith(('http://', 'https://', '/', '..')):
            return m.group(0)
        return f'href="{Path(href).name}.html{frag}"'

    return _MD_HREF_RE.sub(rewrite, body_html)


def render_guide(stem: str, src: Path, out: Path, all_stems: set,
                 meta: dict | None = None, doc: dict | None = None) -> None:
    if doc is None:
        doc = build_guide_body(stem, src, meta)
    title = doc['title']
    toc_tokens = doc['toc_tokens']
    body_html = rewrite_links_site(doc['body'])

    sidebar_items = toc_tokens_to_sidebar(toc_tokens)
    sidebar_html = build_sidebar(
        toc=f'      <h3>On this page</h3>\n      <ul>{sidebar_items}</ul>',
        uplinks=[('index.html', 'All Guides')],
        extra_titles={'index.html': 'Every guide, grouped by category'},
    )

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
        f'<li><a href="#{re.sub(r" +", "-", c["name"].lower())}" '
        f'title="Jump to {_html.escape(c["name"], quote=True)}">{c["name"]}</a></li>'
        for c in categories if any(g['stem'] in all_stems for g in c['guides'])
    ]
    if recent:
        sidebar_cats_list.insert(
            0, '<li><a href="#recently-added" title="Jump to Recently Added">'
               'Recently Added</a></li>')
    sidebar_cats = '\n'.join(sidebar_cats_list)
    sidebar_html = build_sidebar(
        toc=f'      <h3>Categories</h3>\n      <ul>{sidebar_cats}</ul>')

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
{sidebar_html}
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


def emit_pack_guides(docs: list[dict], guides_dir: Path, pack_dir: Path) -> None:
    """Write the chrome-free guide fragments and the guides slice of index.json.

    Fragments keep their source links; `tools/genpack.py` rewrites them into the
    pack's `#doc=` URL space once every generator has contributed, because only
    it knows what the finished pack contains.
    """
    pack_dir = Path(pack_dir)

    # The pane owns typography, so the pack ships the same guide stylesheet the
    # site pages inline and the same runtime that highlights their code blocks
    # and drives the turmeric/sweet-exp toggles -- emitted from the very
    # constants those pages are built from, so a guide looks the same in Try as
    # it does on turmeric-lang.com without the rules being written twice.
    packlib.write_fragment(pack_dir, 'guide.css', GUIDE_CSS)
    packlib.write_fragment(pack_dir, 'guide.js', GUIDE_JS_CORE)

    entries = []
    for doc in docs:
        stem = doc['stem']
        rel = f'guides/{stem}.html'
        size = packlib.write_fragment(pack_dir, rel, doc['body'])
        meta = doc['meta'] or {}
        headings = packlib.heading_names(doc['toc_tokens'])
        description = (meta.get('description', '') or '').replace('—', '--').strip()
        entries.append({
            'slug': stem,
            'path': rel,
            'title': doc['title'],
            'category': (meta.get('category', '') or '').strip() or 'Other',
            'description': description,
            'bytes': size,
            'words': packlib.search_string(
                stem.replace('-', ' '), doc['title'], description,
                *headings, prose=packlib.strip_tags(doc['body'])),
        })

        # Copy any local images the guide references, so the pack is
        # self-contained and the budget check counts what it ships.
        for src_rel in packlib.local_image_srcs(doc['body']):
            src_path = (guides_dir / src_rel).resolve()
            if not src_path.is_file():
                print(f'  warning: {stem}.md references missing image {src_rel}',
                      file=sys.stderr)
                continue
            dst = pack_dir / 'guides' / src_rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(src_path.read_bytes())

    packlib.write_sidecar(pack_dir, 'guides', entries)
    print(f'  pack: {len(entries)} guide fragments -> {pack_dir}/guides/')


def main() -> None:
    p = argparse.ArgumentParser(description='Render Turmeric guide markdown to HTML.')
    p.add_argument('guides_dir', help='Path to docs/guides/ directory')
    p.add_argument('--out', default=None, help='Output directory (default: same as guides_dir)')
    p.add_argument('--emit-pack', metavar='DIR', default=None,
                   help='Also write chrome-free guide fragments into the docs '
                        'pack at DIR (see tools/genpack.py)')
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
    docs = []
    for src in md_files:
        doc = build_guide_body(src.stem, src, meta_by_stem.get(src.stem, {}))
        render_guide(src.stem, src, out_dir / f'{src.stem}.html', all_stems,
                     doc=doc)
        docs.append(doc)
    render_index(categories, all_stems, out_dir, recent=recent)
    print(f'Done: {len(md_files)} guides + index → {out_dir}')

    if args.emit_pack:
        emit_pack_guides(docs, guides_dir, Path(args.emit_pack))


if __name__ == '__main__':
    main()
