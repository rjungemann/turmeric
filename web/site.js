// site.js — Shared JS for turmeric-lang.com marketing pages (/, /tour)
// Load this with <script type="module" src="/site.js"> in <head>; the module
// is deferred by default, so it runs after the DOM is parsed.

import Prism from 'prismjs';

// ── TURMERIC SYNTAX GRAMMAR ─────────────────────────────────────────────────

Prism.languages.turmeric = {
  comment:     /;.*/,
  string:      /"(?:[^"\\]|\\.)*"/,
  keyword: {
    pattern: /\b(?:defn|defmacro|defstruct|defclass|definstance|defdata|defgadt|defeffect|defpackage|let|let\*|letrec|if|cond|when|match|fn|do|begin|and|or|not|handle|perform|resume)\b/,
    greedy: false,
  },
  type:        /:[a-zA-Z][a-zA-Z0-9_\-?!]*/,
  number:      /\b\d[\d._]*\b/,
  punctuation: /[()[\]{}]/,
};

// ── SYNTAX HIGHLIGHTING ─────────────────────────────────────────────────────

// Highlight all .code-version and .step-code elements that contain plain code.
// site.js runs after DOM parsing (module = implicit defer), so elements are
// available immediately without waiting for DOMContentLoaded.
document.querySelectorAll('.code-version, .step-code').forEach(el => {
  const raw = el.textContent;
  el.innerHTML = Prism.highlight(raw, Prism.languages.turmeric, 'turmeric');
});

// ── WEB COMPONENTS ─────────────────────────────────────────────────────────

class SiteNav extends HTMLElement {
  connectedCallback() {
    const active = this.getAttribute('active') ?? '';
    const links = [
      ['/tour',              'Tour'],
      ['/try',               'Try It'],
      ['/docs/html/guides/', 'Guides'],
      ['/docs/html/api/',    'API Docs'],
      ['/docs/spice/',       'Packages'],
    ];

    this.innerHTML = `
      <nav>
        <a class="nav-logo" href="/">
          <div class="nav-mark">T</div>
          <span class="nav-name">Turmeric</span>
        </a>
        <div class="nav-links">
          ${links.map(([href, label]) =>
            `<a href="${href}"${active === label ? ' class="active"' : ''}>${label}</a>`
          ).join('')}
        </div>
        <div class="nav-right">
          <a href="https://github.com/rjungemann/turmeric" class="btn-ghost">GitHub</a>
          <a href="/try" class="btn-gold">Try it →</a>
        </div>
      </nav>`;
  }
}

class SiteFooter extends HTMLElement {
  connectedCallback() {
    this.innerHTML = `
      <footer>
        <div class="footer-inner">
          <div class="footer-brand footer-col">
            <a href="/" class="nav-logo" style="display:inline-flex;gap:9px;align-items:center;">
              <div class="nav-mark">T</div>
              <span class="nav-name">Turmeric</span>
            </a>
            <p>A lightning-fast functional language with typeclasses, effects, and a
               Lisp-flavored syntax. Open source under the MIT license.</p>
          </div>
          <div class="footer-col">
            <div class="footer-col-title">Language</div>
            <a href="/tour">Tour</a>
          </div>
          <div class="footer-col">
            <div class="footer-col-title">Ecosystem</div>
            <a href="/docs/html/guides/">Guides</a>
            <a href="/docs/html/api/">Standard Library</a>
          </div>
          <div class="footer-col">
            <div class="footer-col-title">Community</div>
            <a href="https://github.com/rjungemann/turmeric">GitHub</a>
          </div>
        </div>
        <div class="footer-bottom">
          <span class="footer-copy">© 2025 The Turmeric Project and
            <a href="https://phasor.space">Roger Jungemann</a>. MIT License.</span>
          <div class="footer-links"></div>
        </div>
      </footer>`;
  }
}

customElements.define('site-nav', SiteNav);
customElements.define('site-footer', SiteFooter);

// ── REVEAL ON SCROLL ────────────────────────────────────────────────────────

const revealTargets = Array.from(document.querySelectorAll('.reveal'));
let revealPending = revealTargets.length;

const revealObs = new IntersectionObserver(entries => {
  entries.forEach(e => {
    if (e.isIntersecting) {
      e.target.classList.add('visible');
      revealObs.unobserve(e.target);
      revealPending--;
      if (revealPending <= 0) revealObs.disconnect();
    }
  });
}, { threshold: 0.08, rootMargin: '0px 0px -40px 0px' });

revealTargets.forEach((el, i) => {
  el.style.transitionDelay = (i === 0 ? 0.1 : 0) + 's';
  revealObs.observe(el);
});

// ── SYNTAX TOGGLE ──────────────────────────────────────────────────────────

// Supports multiple independent toggles per page
document.querySelectorAll('.code-syntax-toggle').forEach(toggle => {
  toggle.addEventListener('click', (e) => {
    if (!e.target.classList.contains('seg-btn')) return;
    const syntax = e.target.dataset.syntax;
    const card = toggle.closest('.code-card');

    toggle.querySelectorAll('.seg-btn').forEach(btn => {
      btn.classList.toggle('active', btn === e.target);
    });

    // Update filename using data attributes on the filename element
    const filenameEl = card.querySelector('.code-card-filename');
    if (filenameEl) {
      const key = syntax.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
      if (filenameEl.dataset[key]) filenameEl.textContent = filenameEl.dataset[key];
    }

    // Show/hide code versions scoped to this card
    card.querySelectorAll('.code-version').forEach(v => {
      v.style.display = v.classList.contains(syntax + '-version') ? '' : 'none';
    });
  });
});
