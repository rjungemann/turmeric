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

// ── COPY BUTTONS ────────────────────────────────────────────────────────────

const SVG_COPY  = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>';
const SVG_CHECK = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="20 6 9 17 4 12"/></svg>';

function makeCopyBtn(text) {
  const btn = document.createElement('button');
  btn.className = 'copy-btn';
  btn.setAttribute('aria-label', 'Copy to clipboard');
  btn.title = 'Copy';
  btn.innerHTML = SVG_COPY;
  btn.addEventListener('click', () => {
    navigator.clipboard.writeText(text.trim()).then(() => {
      btn.innerHTML = SVG_CHECK;
      btn.classList.add('copied');
      btn.setAttribute('aria-label', 'Copied!');
      setTimeout(() => {
        btn.innerHTML = SVG_COPY;
        btn.classList.remove('copied');
        btn.setAttribute('aria-label', 'Copy to clipboard');
      }, 2000);
    });
  });
  return btn;
}

// Inline copy button after .install-cmd (fits into the flex row naturally)
document.querySelectorAll('.install-cmd').forEach(el => {
  el.insertAdjacentElement('afterend', makeCopyBtn(el.textContent));
});

// Absolutely-positioned copy button overlaid top-right on each .step-code block
document.querySelectorAll('.step-code').forEach(el => {
  el.appendChild(makeCopyBtn(el.textContent));
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

    const linkHTML = links.map(([href, label]) =>
      `<a href="${href}"${active === label ? ' class="active"' : ''}>${label}</a>`
    ).join('');

    this.innerHTML = `
      <nav>
        <a class="nav-logo" href="/">
          <img src="/logo-icon.svg" class="nav-logo-icon" width="28" height="28" alt="">
          <img src="/logo.svg" class="nav-logo-wordmark" width="101" height="28" alt="Turmeric">
        </a>
        <div class="nav-links">${linkHTML}</div>
        <div class="nav-right">
          <a href="https://github.com/rjungemann/turmeric" class="btn-ghost">GitHub</a>
          <a href="/try" class="btn-gold">Try it →</a>
        </div>
        <button class="nav-hamburger" aria-label="Open menu" aria-expanded="false">
          <span></span><span></span><span></span>
        </button>
        <div class="nav-mobile-panel">
          ${linkHTML}
          <div class="nav-mobile-cta">
            <a href="https://github.com/rjungemann/turmeric" class="btn-ghost">GitHub</a>
            <a href="/try" class="btn-gold">Try it →</a>
          </div>
        </div>
      </nav>`;

    const nav    = this.querySelector('nav');
    const btn    = this.querySelector('.nav-hamburger');
    const panel  = this.querySelector('.nav-mobile-panel');

    btn.addEventListener('click', () => {
      const open = nav.classList.toggle('nav-open');
      btn.setAttribute('aria-expanded', String(open));
    });

    document.addEventListener('click', e => {
      if (!this.contains(e.target) && nav.classList.contains('nav-open')) {
        nav.classList.remove('nav-open');
        btn.setAttribute('aria-expanded', 'false');
      }
    });

    panel.querySelectorAll('a').forEach(a => {
      a.addEventListener('click', () => {
        nav.classList.remove('nav-open');
        btn.setAttribute('aria-expanded', 'false');
      });
    });
  }
}

class SiteFooter extends HTMLElement {
  connectedCallback() {
    this.innerHTML = `
      <footer>
        <div class="footer-inner">
          <div class="footer-brand footer-col">
            <a href="/" class="nav-logo" style="display:inline-flex;gap:10px;align-items:center;">
              <img src="/logo-icon.svg" class="nav-logo-icon" width="28" height="28" alt="">
              <img src="/logo.svg" class="nav-logo-wordmark" width="101" height="28" alt="Turmeric">
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
      btn.setAttribute('aria-selected', btn === e.target ? 'true' : 'false');
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

  // Arrow-key navigation within the tablist
  toggle.addEventListener('keydown', (e) => {
    const btns = Array.from(toggle.querySelectorAll('.seg-btn'));
    const idx = btns.indexOf(document.activeElement);
    if (idx === -1) return;
    if (e.key === 'ArrowRight') { btns[(idx + 1) % btns.length].focus(); e.preventDefault(); }
    if (e.key === 'ArrowLeft')  { btns[(idx - 1 + btns.length) % btns.length].focus(); e.preventDefault(); }
  });
});
