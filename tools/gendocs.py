#!/usr/bin/env python3
"""
tools/gendocs.py -- Generate HTML API documentation from Turmeric stdlib files.

Usage:
    # Generate all docs
    python3 tools/gendocs.py stdlib/ --out docs/html/api/

    # Generate single module
    python3 tools/gendocs.py stdlib/list.tur --out docs/html/api/

    # Also emit docstrings.tur (for runtime (doc name) lookup)
    python3 tools/gendocs.py stdlib/ --out docs/html/api/ --emit-tur stdlib/docstrings.tur
"""

import argparse
import os
import re
import sys
import html as html_module
from pathlib import Path


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

def _parse_params(bracket_content):
    """
    Parse a parameter list like '[value :int next :int]' or '[& clauses]'.
    Returns a list of (name, type_ann) tuples.  Type annotations are optional.

    UT3: ^-annotations (^unique, ^linear, ^mut) that precede a parameter
    are captured and prepended to the parameter name so they appear in the
    rendered signature, e.g. '^unique v' instead of 'v'.
    """
    tokens = bracket_content.strip().lstrip('[').split(']')[0].split()
    params = []
    i = 0
    pending_anns = []  # UT3: accumulated ^-annotations for the next param
    while i < len(tokens):
        tok = tokens[i]
        if tok == '&':
            i += 1
            if i < len(tokens):
                name = tokens[i]
                if pending_anns:
                    name = ' '.join(pending_anns) + ' ' + name
                    pending_anns = []
                params.append((name, '...'))
            i += 1
            continue
        if tok.startswith('^'):
            # UT3: ^-annotation precedes the parameter it qualifies
            pending_anns.append(tok)
            i += 1
            continue
        if tok.startswith(':') or tok.startswith('ptr<'):
            # This is a type annotation for the previous param.
            # Note: bare 'ptr' without '<' is a valid field/param NAME in Turmeric,
            # so we only treat 'ptr<...' (with the angle bracket) as a type token.
            if params:
                name, _ = params[-1]
                params[-1] = (name, tok)
            i += 1
            continue
        # Regular parameter name -- apply any pending annotations
        name = tok
        if pending_anns:
            name = ' '.join(pending_anns) + ' ' + name
            pending_anns = []
        params.append((name, None))
        i += 1
    return params


def _extract_return_type(line):
    """
    Extract return type from a defn signature line.
    Looks for a ':type' or 'ptr<...>' token after the closing bracket.
    """
    # After the closing ] there may be a return type token
    bracket_end = line.rfind(']')
    if bracket_end == -1:
        return None
    rest = line[bracket_end + 1:].strip()
    # First token of rest is the return type (if it starts with : or ptr)
    parts = rest.split()
    if parts and (parts[0].startswith(':') or parts[0].startswith('ptr')):
        return parts[0]
    return None


def _parse_docstring(lines):
    """
    Parse a list of ';;; ...' lines into structured sections.
    Returns a dict:
      {
        'summary': str,
        'params': [(name, desc), ...],
        'returns': str,
        'example': str,
        'since': str,
        'raw': str,   # full text for --emit-tur
      }
    """
    if not lines:
        return None

    # Strip ';;; ' prefix
    stripped = []
    for line in lines:
        line = line.strip()
        if line == ';;;':
            stripped.append('')
        elif line.startswith(';;; '):
            stripped.append(line[4:])
        else:
            stripped.append('')

    summary = stripped[0] if stripped else ''
    # Remove leading 'name -- ' from summary for display
    # but keep the full text for raw

    sections = {'summary': summary, 'params': [], 'returns': '',
                 'example': '', 'since': '', 'raw': '\n'.join(stripped)}

    current_section = None
    buf = []

    def flush():
        nonlocal buf
        text = '\n'.join(buf).strip()
        buf = []
        return text

    def _flush_section():
        if current_section == 'returns':
            sections['returns'] = flush()
        elif current_section == 'example':
            sections['example'] = flush()

    for line in stripped[1:]:
        if line.startswith('Parameters:'):
            _flush_section()
            current_section = 'params'
            continue
        elif line.startswith('Returns:'):
            _flush_section()
            current_section = 'returns'
            continue
        elif line.startswith('Example:'):
            _flush_section()
            current_section = 'example'
            continue
        elif line.startswith('Since:'):
            _flush_section()
            sections['since'] = line[len('Since:'):].strip()
            current_section = None
            continue

        if current_section == 'params':
            stripped_line = line.strip()
            if stripped_line:
                # Format: '  name -- description'
                m = re.match(r'^(\S+)\s+--\s+(.*)', stripped_line)
                if m:
                    sections['params'].append((m.group(1), m.group(2)))
                else:
                    sections['params'].append(('', stripped_line))
        elif current_section == 'returns':
            buf.append(line)
        elif current_section == 'example':
            buf.append(line)

    _flush_section()

    return sections


def parse_tur_file(path):
    """
    Parse a .tur file and return a module description dict:
    {
        'name': 'tur/list',   # or None
        'file_stem': 'list',
        'file_path': path,
        'exports': set(),
        'definitions': [
            {
                'kind': 'defn' | 'defmacro' | 'defstruct' | 'definstance',
                'name': str,
                'params': [(name, type), ...],
                'return_type': str | None,
                'exported': bool,
                'docstring': { ... } | None,
                'line': int,
            },
            ...
        ],
    }
    """
    path = Path(path)
    with open(path, encoding='utf-8', errors='replace') as f:
        raw = f.read()

    lines = raw.splitlines()
    file_stem = path.stem  # e.g. 'list' from 'list.tur'

    module = {
        'name': None,
        'file_stem': file_stem,
        'file_path': str(path),
        'exports': set(),
        'definitions': [],
    }

    # ------------------------------------------------------------------
    # Step 1: Find (defmodule tur/name ...)
    # ------------------------------------------------------------------
    module_re = re.compile(r'\(\s*defmodule\s+([\w/\-]+)')
    for line in lines:
        m = module_re.search(line)
        if m:
            module['name'] = m.group(1)
            break

    if module['name'] is None:
        # Derive pseudo-name from filename
        module['name'] = 'tur/' + file_stem

    # ------------------------------------------------------------------
    # Step 2: Find (export ...) possibly spanning multiple lines
    # ------------------------------------------------------------------
    export_re = re.compile(r'\(\s*export\s+')
    in_export = False
    paren_depth = 0
    export_text = ''

    for line in lines:
        if not in_export:
            if export_re.search(line):
                in_export = True
                # Start from the 'export' keyword
                idx = export_re.search(line).start()
                export_text = line[idx:]
                paren_depth = export_text.count('(') - export_text.count(')')
                if paren_depth <= 0:
                    in_export = False
                    # Done
                    _extract_exports(export_text, module)
        else:
            export_text += ' ' + line
            paren_depth += line.count('(') - line.count(')')
            if paren_depth <= 0:
                in_export = False
                _extract_exports(export_text, module)

    # ------------------------------------------------------------------
    # Step 3: Scan definitions with docstrings
    # ------------------------------------------------------------------
    doc_buf = []  # accumulated ';;;' lines
    def_re = re.compile(
        r'^\s*\(\s*(defn|defmacro|defstruct|definstance)\s+'
    )

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped.startswith(';;;'):
            doc_buf.append(stripped)
            i += 1
            continue

        m = def_re.match(line)
        if m:
            kind = m.group(1)
            # Parse the name + rest from this line (may span lines)
            def_text = line
            # Accumulate until we find the closing bracket of the params
            j = i + 1
            open_brackets = def_text.count('[') - def_text.count(']')
            while open_brackets > 0 and j < len(lines):
                def_text += ' ' + lines[j].strip()
                open_brackets += lines[j].count('[') - lines[j].count(']')
                j += 1

            name, params, return_type, extra = _parse_def_line(kind, def_text)
            if name:
                exported = (name in module['exports']) or (not module['exports'])
                # Don't mark internal helpers as exported even if no export list
                if name.startswith('__'):
                    exported = False
                if name.endswith('/autolink-hint'):
                    exported = False

                docstring = _parse_docstring(doc_buf) if doc_buf else None
                defn_dict = {
                    'kind': kind,
                    'name': name,
                    'params': params,
                    'return_type': return_type,
                    'exported': exported,
                    'docstring': docstring,
                    'line': i + 1,
                }
                defn_dict.update(extra)  # LT4: merge kind-specific extras (e.g. struct_ann)
                module['definitions'].append(defn_dict)
            doc_buf = []
            i += 1
            continue

        # Non-;;; lines that reset the docstring buffer:
        # - blank lines with content that isn't a comment
        # - any non-comment, non-blank line
        # But: regular ;; comments between the ;;; block and the defn are
        # allowed (agents may place them there); do NOT reset on ;; lines.
        if stripped and not stripped.startswith(';'):
            doc_buf = []

        i += 1

    return module


def _extract_exports(text, module):
    """Parse '(export name1 name2 ...)' and add to module['exports']."""
    # Remove the (export and closing )
    inner = re.sub(r'^\s*\(\s*export\s+', '', text)
    inner = re.sub(r'\)\s*$', '', inner)
    for tok in inner.split():
        tok = tok.strip('()')
        if tok:
            module['exports'].add(tok)


def _parse_def_line(kind, text):
    """
    Parse a defn/defmacro/defstruct/definstance line (possibly multi-line collapsed).
    Returns (name, params, return_type, extra) where extra is a dict of kind-specific
    data (e.g. {'struct_ann': 'linear'} for defstruct with :linear annotation).
    """
    # Match kind name
    pattern = r'\(\s*' + kind + r'\s+([\w/\-!?<>*+]+)'
    m = re.search(pattern, text)
    if not m:
        return None, [], None, {}

    name = m.group(1)
    rest = text[m.end():]

    if kind == 'definstance':
        # (definstance TypeName [TypeParam] ...)
        type_m = re.search(r'\[\s*([\w/\-!?<>*+]+)', rest)
        type_param = type_m.group(1) if type_m else ''
        return name + '[' + type_param + ']', [], None, {}

    extra = {}

    # LT4: For defstruct, detect :copy / :move / :linear annotation before the bracket.
    if kind == 'defstruct':
        ann_m = re.search(r':(copy|move|linear)\s', rest)
        if ann_m:
            extra['struct_ann'] = ann_m.group(1)

    # Extract params bracket
    bracket_m = re.search(r'\[([^\]]*)\]', rest)
    params = []
    return_type = None
    if bracket_m:
        params = _parse_params('[' + bracket_m.group(1) + ']')
        after_bracket = rest[bracket_m.end():]
        # Return type is the first :type or ptr token
        parts = after_bracket.strip().split()
        if parts and (parts[0].startswith(':') or parts[0].startswith('ptr')):
            return_type = parts[0]

    return name, params, return_type, extra


# ---------------------------------------------------------------------------
# Doctest extraction  (Phase D0)
# ---------------------------------------------------------------------------

TESTABLE_RE = re.compile(
    r'^(-?[0-9]+\.[0-9]+|-?[0-9]+|true|false|"[^"]*"|nil)$'
)


class DocTestCase:
    """A single testable docstring example."""
    __slots__ = ('module_name', 'defn_name', 'setup_lines', 'expr', 'expected')

    def __init__(self, module_name, defn_name, setup_lines, expr, expected):
        self.module_name = module_name
        self.defn_name = defn_name
        self.setup_lines = setup_lines  # list[str] -- lines before this case
        self.expr = expr                # str -- the expression to evaluate
        self.expected = expected        # str -- expected println output


def extract_doctest_cases(module_name, defn_name, doc):
    """
    Extract testable examples from a parsed docstring dict.

    A line is testable when its '; =>' value matches TESTABLE_RE (integer,
    float, true/false, quoted string, or nil).  Lines without '; =>' are
    accumulated as setup statements for the following testable line and
    emitted ahead of it in the generated test file.

    Returns a list of DocTestCase objects; non-testable '; =>' lines are
    skipped silently.
    """
    if not doc or not doc.get('example'):
        return []

    lines = [l.strip() for l in doc['example'].splitlines() if l.strip()]
    cases = []
    pending_setup = []

    for line in lines:
        if '; =>' not in line:
            pending_setup.append(line)
            continue
        expr_part, expected_raw = line.split('; =>', 1)
        expr_part = expr_part.strip()
        expected_raw = expected_raw.strip()
        if TESTABLE_RE.match(expected_raw):
            cases.append(DocTestCase(
                module_name=module_name,
                defn_name=defn_name,
                setup_lines=list(pending_setup),
                expr=expr_part,
                expected=expected_raw,
            ))
        # Reset pending setup after each '; =>' line (testable or not)
        pending_setup = []

    return cases


# ---------------------------------------------------------------------------
# HTML Emitter
# ---------------------------------------------------------------------------

CSS = """\
/* Turmeric API Docs -- auto-generated by tools/gendocs.py */

:root {
  --gold:       #E8A020;
  --orange:     #C4581A;
  --cream:      #F5E6C8;
  --faint:      #8A7560;
  --bg-dark:    #1A1208;
  --bg-card:    #261C10;
  --bg-code:    #0F0A05;
  --border:     #3D2A18;
  --highlight:  #4A3010;
}

*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

html { font-size: 16px; }

body {
  font-family: system-ui, -apple-system, 'Segoe UI', Inter, sans-serif;
  background: var(--bg-dark);
  color: var(--cream);
  line-height: 1.6;
  min-height: 100vh;
}

a { color: var(--gold); text-decoration: none; }
a:hover { text-decoration: underline; }

/* Header */
.site-header {
  background: var(--bg-card);
  border-bottom: 1px solid var(--border);
  padding: 0.75rem 2rem;
  display: flex;
  align-items: center;
  gap: 1.5rem;
}
.site-header .brand {
  font-size: 1.25rem;
  font-weight: 700;
  color: var(--gold);
  letter-spacing: 0.02em;
}
.site-header nav { display: flex; gap: 1rem; font-size: 0.9rem; }
.site-header nav a { color: var(--faint); }
.site-header nav a:hover { color: var(--cream); }

/* Search */
.search-wrap { margin-left: auto; }
.search-input {
  background: var(--bg-dark);
  border: 1px solid var(--border);
  border-radius: 4px;
  color: var(--cream);
  padding: 0.3rem 0.65rem;
  font-size: 0.875rem;
  width: 220px;
  transition: border-color 0.15s, width 0.2s;
}
.search-input:focus { outline: none; border-color: var(--gold); width: 280px; }
.search-input::placeholder { color: var(--faint); }
.search-no-results {
  display: none;
  padding: 2rem;
  color: var(--faint);
  font-style: italic;
}

/* Layout */
.page-layout {
  display: grid;
  grid-template-columns: 240px 1fr;
  gap: 0;
  max-width: 1200px;
  margin: 0 auto;
  padding: 0;
}

/* Sidebar */
.sidebar {
  padding: 1.5rem 1rem;
  border-right: 1px solid var(--border);
  position: sticky;
  top: 0;
  height: 100vh;
  overflow-y: auto;
}
.sidebar h3 {
  font-size: 0.75rem;
  text-transform: uppercase;
  letter-spacing: 0.08em;
  color: var(--faint);
  margin-bottom: 0.75rem;
  margin-top: 1rem;
}
.sidebar h3:first-child { margin-top: 0; }
.sidebar ul { list-style: none; }
.sidebar li { margin: 0.2rem 0; }
.sidebar a { font-size: 0.875rem; color: var(--cream); font-family: 'JetBrains Mono', 'Fira Code', monospace; }
.sidebar a:hover { color: var(--gold); }

/* Content */
.content {
  padding: 2rem 2.5rem;
  min-width: 0;
}

/* Module heading */
.module-heading {
  margin-bottom: 2rem;
  padding-bottom: 1rem;
  border-bottom: 1px solid var(--border);
}
.module-heading h1 {
  font-size: 1.75rem;
  color: var(--gold);
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
}
.module-path {
  font-size: 0.85rem;
  color: var(--faint);
  margin-top: 0.25rem;
}

/* Definition cards */
.def-card {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 1.25rem 1.5rem;
  margin-bottom: 1.5rem;
  transition: background 0.15s;
}
.def-card:hover { background: var(--highlight); }
.def-card:target { border-color: var(--gold); }

.def-card-header {
  display: flex;
  align-items: baseline;
  gap: 0.75rem;
  margin-bottom: 0.75rem;
  flex-wrap: wrap;
}
.def-card-header h2 {
  font-size: 1rem;
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  color: var(--gold);
  font-weight: 600;
}

.kind-badge {
  display: inline-block;
  padding: 0.15rem 0.55rem;
  border-radius: 99px;
  font-size: 0.7rem;
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-weight: 600;
  text-transform: lowercase;
}
.kind-defn      { background: #6B4A00; color: #F5C040; }
.kind-defmacro  { background: #5A2800; color: #F57A30; }
.kind-defstruct { background: #3A4A00; color: #B5C840; }
.kind-definstance { background: #003A3A; color: #50C8B8; }

/* LT4: badge for linear types and ^linear / lref<T> annotations */
.linear-badge {
  display: inline-block;
  padding: 0.15rem 0.55rem;
  border-radius: 99px;
  font-size: 0.7rem;
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-weight: 600;
  text-transform: lowercase;
  background: #1A3A4A;
  color: #50C8E8;
}

.def-signature {
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-size: 0.875rem;
  background: var(--bg-code);
  border: 1px solid var(--border);
  border-radius: 4px;
  padding: 0.5rem 0.75rem;
  margin-bottom: 0.75rem;
  overflow-x: auto;
  white-space: pre;
}

.def-summary {
  color: var(--cream);
  margin-bottom: 0.75rem;
}

.def-section { margin-top: 0.75rem; }
.def-section-label {
  font-size: 0.75rem;
  text-transform: uppercase;
  letter-spacing: 0.07em;
  color: var(--faint);
  margin-bottom: 0.4rem;
  font-weight: 600;
}

.param-table { border-collapse: collapse; width: 100%; font-size: 0.875rem; }
.param-table td { padding: 0.2rem 0.75rem 0.2rem 0; vertical-align: top; }
.param-table td:first-child {
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  color: var(--gold);
  white-space: nowrap;
  width: 1%;
}
.param-table td:nth-child(2) { color: var(--faint); white-space: nowrap; width: 1%; }

.def-returns { font-size: 0.875rem; }

.def-example {
  background: var(--bg-code);
  border: 1px solid var(--border);
  border-radius: 4px;
  padding: 0.6rem 0.75rem;
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-size: 0.8rem;
  overflow-x: auto;
  white-space: pre;
}

.def-since { font-size: 0.8rem; color: var(--faint); margin-top: 0.5rem; }

/* Internal section */
details.internal-section {
  margin-top: 2rem;
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 1rem 1.25rem;
}
details.internal-section summary {
  cursor: pointer;
  font-size: 0.85rem;
  color: var(--faint);
  user-select: none;
}
.internal-list { margin-top: 0.75rem; }
.internal-item {
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-size: 0.8rem;
  color: var(--faint);
  padding: 0.2rem 0;
}
.internal-item-summary { color: var(--cream); margin-left: 0.5rem; font-family: inherit; }

/* Index page */
.index-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 1rem;
  margin-top: 1.5rem;
}
.index-card {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 1rem 1.25rem;
  transition: background 0.15s;
}
.index-card:hover { background: var(--highlight); }
.index-card h3 {
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-size: 0.95rem;
  color: var(--gold);
  margin-bottom: 0.35rem;
}
.index-card p { font-size: 0.85rem; color: var(--faint); }
.index-card .export-count { font-size: 0.75rem; color: var(--faint); margin-top: 0.5rem; }

/* Footer */
.site-footer {
  border-top: 1px solid var(--border);
  padding: 1.5rem 2rem;
  text-align: center;
  font-size: 0.8rem;
  color: var(--faint);
  margin-top: 4rem;
}

/* Hamburger button */
.hamburger {
  display: none;
  background: none;
  border: none;
  cursor: pointer;
  padding: 0.25rem;
  color: var(--cream);
  flex-shrink: 0;
}
.hamburger span {
  display: block;
  width: 20px;
  height: 2px;
  background: var(--cream);
  margin: 4px 0;
  transition: background 0.15s;
}
.hamburger:hover span { background: var(--gold); }

/* Sidebar overlay (mobile) */
.sidebar-overlay {
  display: none;
  position: fixed;
  inset: 0;
  background: rgba(0,0,0,0.55);
  z-index: 99;
}
.sidebar-overlay.is-open { display: block; }

/* Responsive */
@media (max-width: 768px) {
  .hamburger { display: block; }
  .site-header { padding: 0.75rem 1rem; }
  .site-header nav { display: none; }
  .search-input { width: 140px; }
  .search-input:focus { width: 180px; }
  .page-layout { grid-template-columns: 1fr; }
  .sidebar {
    display: none;
    position: fixed;
    top: 0;
    left: 0;
    width: 280px;
    height: 100vh;
    z-index: 100;
    background: var(--bg-card);
    border-right: 1px solid var(--border);
    border-bottom: none;
    overflow-y: auto;
    padding: 1.5rem 1rem;
  }
  .sidebar.is-open { display: block; }
  .content { padding: 1.25rem 1rem; }
}
"""


_SIDEBAR_TOGGLE_JS = """\
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

  // Search filtering
  document.addEventListener('DOMContentLoaded', function(){
    var input = document.querySelector('.search-input');
    if (!input) return;

    function filter() {
      var q = input.value.trim().toLowerCase();

      // Module pages: filter .def-card elements
      var defCards = document.querySelectorAll('.def-card');
      var visibleDefs = 0;
      defCards.forEach(function(card) {
        var match = !q || card.textContent.toLowerCase().includes(q);
        card.style.display = match ? '' : 'none';
        if (match) visibleDefs++;
      });

      // Sync sidebar links with card visibility
      document.querySelectorAll('.sidebar a[href^="#"]').forEach(function(link) {
        var anchor = link.getAttribute('href').slice(1);
        var target = document.getElementById(anchor);
        link.parentElement.style.display =
          (!target || target.style.display !== 'none') ? '' : 'none';
      });

      // Index pages: filter .index-card elements
      var indexCards = document.querySelectorAll('.index-card');
      var visibleIndex = 0;
      indexCards.forEach(function(card) {
        var match = !q || card.textContent.toLowerCase().includes(q);
        card.style.display = match ? '' : 'none';
        if (match) visibleIndex++;
      });

      // Show/hide "no results" message
      var noResults = document.querySelector('.search-no-results');
      if (noResults) {
        var total = defCards.length + indexCards.length;
        var visible = visibleDefs + visibleIndex;
        noResults.style.display = (q && total > 0 && visible === 0) ? 'block' : 'none';
      }
    }

    input.addEventListener('input', filter);

    // Clear on Escape
    input.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') { input.value = ''; filter(); input.blur(); }
    });

    // Focus search with '/' shortcut (when not typing elsewhere)
    document.addEventListener('keydown', function(e) {
      if (e.key === '/' && document.activeElement !== input &&
          document.activeElement.tagName !== 'INPUT' &&
          document.activeElement.tagName !== 'TEXTAREA') {
        e.preventDefault();
        input.focus();
      }
    });
  });
</script>"""


def _html_header(title, css_path='style.css'):
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{html_module.escape(title)}</title>
<link rel="stylesheet" href="{css_path}">
</head>
<body>
<header class="site-header">
  <button class="hamburger" aria-label="Toggle navigation">
    <span></span><span></span><span></span>
  </button>
  <span class="brand">turmeric</span>
  <nav>
    <a href="index.html">API Docs</a>
    <a href="../autodoc-plan.md">Plan</a>
  </nav>
  <div class="search-wrap">
    <input class="search-input" type="search" placeholder="Filter... (/)" aria-label="Filter definitions">
  </div>
</header>
<p class="search-no-results">No matching definitions.</p>
{_SIDEBAR_TOGGLE_JS}
"""


def _html_footer():
    return """\
<footer class="site-footer">
  Auto-generated by <code>tools/gendocs.py</code>.
  Do not edit by hand -- run <code>just docs</code> to regenerate.
</footer>
</body>
</html>
"""


def _render_signature(defn):
    kind = defn['kind']
    name = defn['name']
    params = defn['params']
    ret = defn['return_type']

    if kind == 'defstruct':
        param_str = ' '.join(
            f"{p} {t}" if t else p
            for p, t in params
        )
        # LT4: include :linear / :copy / :move annotation if present
        struct_ann = defn.get('struct_ann', '')
        ann_str = f' :{struct_ann}' if struct_ann else ''
        return f"(defstruct {name}{ann_str} [{param_str}])"

    if kind == 'definstance':
        # name is like 'Functor[list]'
        return f"(definstance {name.replace('[', ' [').replace(']', ']')})"

    param_str = ' '.join(
        f"{p} {t}" if t and t != '...' else (f"& {p}" if t == '...' else p)
        for p, t in params
    )
    sig = f"({name}"
    if params:
        sig += f" [{param_str}]"
    if ret:
        sig += f" {ret}"
    sig += ")"
    return sig


def _render_def_card(defn, anchor_prefix=''):
    kind = defn['kind']
    name = defn['name']
    doc = defn['docstring']
    anchor_name = re.sub(r'[^a-zA-Z0-9_\-]', '_', name)
    anchor = (anchor_prefix + anchor_name) if anchor_prefix else anchor_name

    sig = _render_signature(defn)
    kind_cls = f"kind-{kind}"

    # LT4: detect linear types -- :linear struct annotation, ^linear params, lref<T> types.
    params = defn.get('params', [])
    ret = defn.get('return_type', '') or ''
    is_linear = (
        defn.get('struct_ann') == 'linear'
        or any('^linear' in (p or '') for p, _ in params)
        or any('lref<' in (t or '') for _, t in params)
        or 'lref<' in ret
    )

    h = f'<div class="def-card" id="{html_module.escape(anchor)}">\n'
    h += f'  <div class="def-card-header">\n'
    h += f'    <span class="kind-badge {kind_cls}">{kind}</span>\n'
    if is_linear:
        h += f'    <span class="linear-badge">linear</span>\n'
    h += f'    <h2>{html_module.escape(name)}</h2>\n'
    h += f'  </div>\n'
    h += f'  <pre class="def-signature">{html_module.escape(sig)}</pre>\n'

    if doc:
        # Summary: strip leading 'name -- '
        summary = doc['summary']
        dash_idx = summary.find(' -- ')
        if dash_idx != -1:
            summary = summary[dash_idx + 4:]
        if summary:
            h += f'  <p class="def-summary">{html_module.escape(summary)}</p>\n'

        if doc['params']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Parameters</div>\n'
            h += '    <table class="param-table">\n'
            for pname, pdesc in doc['params']:
                h += f'    <tr><td>{html_module.escape(pname)}</td>'
                h += f'<td></td><td>{html_module.escape(pdesc)}</td></tr>\n'
            h += '    </table>\n'
            h += '  </div>\n'

        if doc['returns']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Returns</div>\n'
            h += f'    <p class="def-returns">{html_module.escape(doc["returns"])}</p>\n'
            h += '  </div>\n'

        if doc['example']:
            h += '  <div class="def-section">\n'
            h += '    <div class="def-section-label">Example</div>\n'
            h += f'    <pre class="def-example">{html_module.escape(doc["example"])}</pre>\n'
            h += '  </div>\n'

        if doc['since']:
            h += f'  <p class="def-since">Since: {html_module.escape(doc["since"])}</p>\n'

    h += '</div>\n'
    return h


def render_module_page(module, out_dir):
    """Render a per-module HTML page and return the filename."""
    mod_name = module['name']
    # tur/list -> tur-list.html
    page_name = mod_name.replace('/', '-') + '.html'

    exported = [d for d in module['definitions'] if d['exported']]
    internal = [d for d in module['definitions'] if not d['exported']]

    # Build sidebar TOC (exported only)
    sidebar = '<div class="sidebar">\n'
    sidebar += '  <h3>Exported</h3>\n  <ul>\n'
    for defn in exported:
        anchor = re.sub(r'[^a-zA-Z0-9_\-]', '_', defn['name'])
        sidebar += f'    <li><a href="#{html_module.escape(anchor)}">{html_module.escape(defn["name"])}</a></li>\n'
    sidebar += '  </ul>\n'
    if internal:
        sidebar += '  <h3>Internal</h3>\n  <ul>\n'
        for defn in internal:
            sidebar += f'    <li><a href="#{html_module.escape(defn["name"])}" style="color:var(--faint)">{html_module.escape(defn["name"])}</a></li>\n'
        sidebar += '  </ul>\n'
    sidebar += '</div>\n'

    content = '<div class="content">\n'
    content += '  <div class="module-heading">\n'
    content += f'    <h1>{html_module.escape(mod_name)}</h1>\n'
    content += f'    <div class="module-path">stdlib/{html_module.escape(module["file_stem"])}.tur</div>\n'
    content += '  </div>\n'

    for defn in exported:
        content += _render_def_card(defn)

    if internal:
        content += '<details class="internal-section">\n'
        content += '  <summary>Internal definitions</summary>\n'
        content += '  <div class="internal-list">\n'
        for defn in internal:
            doc = defn['docstring']
            summary_text = ''
            if doc and doc['summary']:
                s = doc['summary']
                dash_idx = s.find(' -- ')
                summary_text = s[dash_idx + 4:] if dash_idx != -1 else s
            anchor = re.sub(r'[^a-zA-Z0-9_\-]', '_', defn['name'])
            content += f'  <div class="internal-item" id="{html_module.escape(anchor)}">'
            content += f'<code>{html_module.escape(defn["name"])}</code>'
            if summary_text:
                content += f'<span class="internal-item-summary">-- {html_module.escape(summary_text)}</span>'
            content += '</div>\n'
        content += '  </div>\n</details>\n'

    content += '</div>\n'

    page = _html_header(f'tur/{html_module.escape(mod_name)} | Turmeric API')
    page += '<div class="page-layout">\n'
    page += sidebar
    page += content
    page += '</div>\n'
    page += _html_footer()

    out_path = Path(out_dir) / page_name
    out_path.write_text(page, encoding='utf-8')
    return page_name


def _index_card_html(module):
    """Render a single index card for a module."""
    mod_name = module['name']
    page_name = mod_name.replace('/', '-') + '.html'
    exported = [d for d in module['definitions'] if d['exported']]
    mod_summary = ''
    with open(module['file_path'], encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if line.startswith(';; ') and not line.startswith(';;; '):
                mod_summary = line[3:]
                break
            if line and not line.startswith(';'):
                break

    export_count = len(exported)
    h = f'  <a href="{html_module.escape(page_name)}" class="index-card" style="display:block">\n'
    h += f'    <h3>{html_module.escape(mod_name)}</h3>\n'
    if mod_summary:
        h += f'    <p>{html_module.escape(mod_summary)}</p>\n'
    h += f'    <div class="export-count">{export_count} exported definition{"s" if export_count != 1 else ""}</div>\n'
    h += '  </a>\n'
    return h


def render_index_page(modules, out_dir):
    """Render the module index page, grouped by subdirectory."""
    content = '<div class="content" style="max-width:960px;margin:0 auto;padding:2rem">\n'
    content += '  <h1 style="color:var(--gold);font-size:2rem;margin-bottom:0.5rem">Turmeric Standard Library</h1>\n'
    content += '  <p style="color:var(--faint);margin-bottom:1.5rem">Auto-generated API reference. Run <code>just docs</code> to regenerate.</p>\n'

    # Group by subdir; None -> 'Core'
    groups = {}
    for module in sorted(modules, key=lambda m: m['name'] or ''):
        key = module.get('subdir') or 'core'
        groups.setdefault(key, []).append(module)

    # Render core first, then remaining groups alphabetically
    group_order = ['core'] + sorted(k for k in groups if k != 'core')

    for group_key in group_order:
        if group_key not in groups:
            continue
        group_modules = groups[group_key]
        group_label = 'Core' if group_key == 'core' else group_key
        content += f'  <h2 style="color:var(--gold);font-size:1.25rem;margin:2rem 0 0.75rem;padding-bottom:0.4rem;border-bottom:1px solid var(--border)">'
        content += f'{html_module.escape(group_label)}</h2>\n'
        content += '  <div class="index-grid">\n'
        for module in group_modules:
            content += _index_card_html(module)
        content += '  </div>\n'

    content += '</div>\n'

    page = _html_header('Turmeric Standard Library | API Docs', css_path='style.css')
    page += content
    page += _html_footer()

    out_path = Path(out_dir) / 'index.html'
    out_path.write_text(page, encoding='utf-8')


# ---------------------------------------------------------------------------
# .tur Emitter  (--emit-tur)
# ---------------------------------------------------------------------------

def _escape_tur_string(s):
    """Escape a string for embedding in a Turmeric string literal."""
    return (s
            .replace('\\', '\\\\')
            .replace('"', '\\"')
            .replace('\n', '\\n')
            .replace('\t', '\\t'))


def _build_doc_entry(defn):
    """Build the full text for a doc entry."""
    doc = defn['docstring']
    if not doc:
        return defn['name']

    # LT4: prepend [linear] to the summary when the definition involves linear types.
    params = defn.get('params', [])
    ret = defn.get('return_type', '') or ''
    is_linear = (
        defn.get('struct_ann') == 'linear'
        or any('^linear' in (p or '') for p, _ in params)
        or any('lref<' in (t or '') for _, t in params)
        or 'lref<' in ret
    )
    summary = doc['summary']
    if is_linear:
        summary = '[linear] ' + summary

    parts = [summary]

    if doc['params']:
        parts.append('')
        parts.append('Parameters:')
        for pname, pdesc in doc['params']:
            parts.append(f'  {pname} -- {pdesc}')

    if doc['returns']:
        parts.append('')
        parts.append('Returns:')
        for line in doc['returns'].splitlines():
            parts.append(f'  {line}')

    if doc['example']:
        parts.append('')
        parts.append('Example:')
        for line in doc['example'].splitlines():
            parts.append(f'  {line}')

    if doc['since']:
        parts.append('')
        parts.append(f'Since: {doc["since"]}')

    return '\n'.join(parts)


def emit_docstrings_tur(modules, out_path, verified_names=None):
    """
    Emit stdlib/docstrings.tur with C-backed doc-lookup and doc-verified? functions.

    verified_names -- optional set of function names that have passing doctests.
    When provided, a doc-verified? function is exported that returns true for
    those names.  When absent (or empty), doc-verified? always returns false.
    """
    entries = []
    for module in modules:
        for defn in module['definitions']:
            name = defn['name']
            # Strip typeclass bracket suffix for lookup key
            key = re.sub(r'\[.*\]$', '', name)
            text = _build_doc_entry(defn)
            entries.append((key, text))

    # Deduplicate: keep the first entry for each key
    seen = {}
    deduped = []
    for key, text in entries:
        if key not in seen:
            seen[key] = True
            deduped.append((key, text))

    lines = [
        ';; AUTO-GENERATED -- do not edit. Run: just docs',
        '(defmodule tur/docstrings',
        '  (export doc-lookup doc-verified?)',
        '',
        '(defn doc-lookup [name :cstr] :cstr',
        '  ```c',
        '  static const struct { const char *key; const char *val; } entries[] = {',
    ]

    for key, text in deduped:
        escaped_key = _escape_tur_string(key)
        escaped_text = _escape_tur_string(text)
        lines.append(f'    {{"{escaped_key}", "{escaped_text}"}},')

    lines += [
        '    {NULL, NULL}',
        '  };',
        '  const char *n = (const char*)(intptr_t)name;',
        '  if (!n) return 0;',
        '  for (int __i = 0; entries[__i].key; __i++) {',
        '    if (strcmp(entries[__i].key, n) == 0) {',
        '      return (int64_t)(intptr_t)entries[__i].val;',
        '    }',
        '  }',
        '  return 0;',
        '  ```)',
        '',
    ]

    # Phase D5: emit doc-verified? backed by the verified names set
    vnames = sorted(verified_names) if verified_names else []
    lines += [
        '(defn doc-verified? [name :cstr] :bool',
        '  ```c',
        '  static const char *verified[] = {',
    ]
    for vname in vnames:
        escaped = _escape_tur_string(vname)
        lines.append(f'    "{escaped}",')
    lines += [
        '    NULL',
        '  };',
        '  const char *n = (const char*)(intptr_t)name;',
        '  if (!n) return 0;',
        '  for (int __i = 0; verified[__i]; __i++) {',
        '    if (strcmp(verified[__i], n) == 0) return 1;',
        '  }',
        '  return 0;',
        '  ```)',
        '',
        ')',
    ]

    Path(out_path).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'  Wrote {out_path} ({len(deduped)} entries, {len(vnames)} verified)')


# ---------------------------------------------------------------------------
# JSON name list emitter  (--emit-json)
# ---------------------------------------------------------------------------

def emit_doc_names_json(modules, out_path):
    """
    Emit a JSON array of {name, summary, kind} objects for the web search bar.
    Summary is the first line of the doc entry text.
    """
    import json

    entries = []
    seen = set()
    for module in modules:
        for defn in module['definitions']:
            name = defn['name']
            key = re.sub(r'\[.*\]$', '', name)
            if key in seen:
                continue
            seen.add(key)

            doc = defn['docstring']
            if doc and doc.get('summary'):
                summary = doc['summary']
            else:
                summary = key

            entries.append({
                'name': key,
                'summary': summary,
                'kind': defn['kind'],
            })

    # Sort alphabetically for deterministic output
    entries.sort(key=lambda e: e['name'].lower())

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(entries, ensure_ascii=True, indent=None, separators=(',', ':')), encoding='utf-8')
    print(f'  Wrote {out_path} ({len(entries)} entries)')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def collect_tur_files(source):
    """Collect all .tur files from a path (file or directory)."""
    source = Path(source)
    if source.is_file():
        return [source]
    files = []
    for f in sorted(source.rglob('*.tur')):
        # Skip generated files
        if f.name == 'docstrings.tur':
            continue
        files.append(f)
    return files


def main():
    parser = argparse.ArgumentParser(
        description='Generate HTML API docs from Turmeric stdlib files.'
    )
    parser.add_argument(
        'source',
        help='Path to a .tur file or a directory containing .tur files',
    )
    parser.add_argument(
        '--out', '-o',
        default='docs/html/api',
        help='Output directory for HTML files (default: docs/html/api)',
    )
    parser.add_argument(
        '--emit-tur',
        metavar='PATH',
        help='Also emit a stdlib/docstrings.tur lookup table at PATH',
    )
    parser.add_argument(
        '--emit-json',
        metavar='PATH',
        help='Also emit a JSON name list at PATH (for web search bar)',
    )
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Write CSS
    css_path = out_dir / 'style.css'
    css_path.write_text(CSS, encoding='utf-8')
    print(f'  Wrote {css_path}')

    # Collect and parse files
    tur_files = collect_tur_files(args.source)
    if not tur_files:
        print(f'No .tur files found in {args.source}', file=sys.stderr)
        sys.exit(1)

    # Resolve the source root for subdir computation (only meaningful for dirs)
    source_root = Path(args.source).resolve() if Path(args.source).is_dir() else None

    modules = []
    for f in tur_files:
        try:
            module = parse_tur_file(f)
            # Attach subdirectory relative to the source root (e.g. 'scscm', 'tidal', None)
            if source_root:
                try:
                    rel_parts = Path(f).resolve().relative_to(source_root).parts
                    module['subdir'] = rel_parts[0] if len(rel_parts) > 1 else None
                except ValueError:
                    module['subdir'] = None
            else:
                module['subdir'] = None
            modules.append(module)
            exported_count = sum(1 for d in module['definitions'] if d['exported'])
            print(f'  Parsed {f} -> {module["name"]} ({exported_count} exported defs)')
        except Exception as e:
            print(f'  Warning: failed to parse {f}: {e}', file=sys.stderr)

    # Render per-module pages
    for module in modules:
        page = render_module_page(module, out_dir)
        print(f'  Wrote {out_dir}/{page}')

    # Render index
    render_index_page(modules, out_dir)
    print(f'  Wrote {out_dir}/index.html')

    # Optionally emit docstrings.tur
    if args.emit_tur:
        # Phase D5: read verified function names from doctest manifest if present
        verified_names = None
        verified_path = Path('tests/doctest-generated/verified.txt')
        if verified_path.exists():
            verified_names = set(
                l.strip() for l in verified_path.read_text(encoding='utf-8').splitlines()
                if l.strip()
            )
        emit_docstrings_tur(modules, args.emit_tur, verified_names=verified_names)

    # Optionally emit doc-names.json for the web search bar
    if args.emit_json:
        emit_doc_names_json(modules, args.emit_json)

    print(f'\nDone. {len(modules)} modules -> {out_dir}/')


if __name__ == '__main__':
    main()
