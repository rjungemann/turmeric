// icons.js — Lucide icon set exposed as a <t-icon> custom element.
//
// Icons come from the self-hosted `lucide-static` package (ISC, ~1600 icons) —
// the same visual family already used for the copy/check buttons in site.js.
// Nothing is fetched from a CDN; Vite bundles what is used.
//
// Adding an icon needs NO change to this file: every SVG under
// lucide-static/icons/*.svg is available by its kebab-case name. Browse names
// at https://lucide.dev/icons. Because the glob below is lazy, an icon is only
// pulled into the build when a page actually references it.
//
// Usage:
//   <t-icon name="play"></t-icon>            sized by the inherited font-size
//   <t-icon name="book-open"></t-icon>       kebab-case, exactly as on lucide.dev
//   <t-icon name="zap" size="20"></t-icon>   px override (bare number) ...
//   <t-icon name="zap" size="1.5rem"></t-icon>   ... or any CSS length
//
// Icons stroke with `currentColor`, so they tint to match surrounding text —
// set `color` on the icon (or its container) to recolor.

// Lazy loaders keyed by absolute path; each value is () => Promise<string>.
const loaders = import.meta.glob('/node_modules/lucide-static/icons/*.svg', {
  query: '?raw',
  import: 'default',
});

// Bare kebab-case name -> lazy loader (e.g. "book-open" -> loader).
const ICONS = {};
for (const path in loaders) {
  const name = path.slice(path.lastIndexOf('/') + 1, -'.svg'.length);
  ICONS[name] = loaders[path];
}

// Resolved SVG markup, cached so a repeated icon imports only once.
const cache = new Map();

async function loadIcon(name) {
  if (cache.has(name)) return cache.get(name);
  const loader = ICONS[name];
  if (!loader) {
    console.warn(`<t-icon>: unknown icon "${name}" — see https://lucide.dev/icons`);
    cache.set(name, '');
    return '';
  }
  const svg = await loader();
  cache.set(name, svg);
  return svg;
}

class TIcon extends HTMLElement {
  static get observedAttributes() { return ['name', 'size']; }
  connectedCallback() { this.render(); }
  attributeChangedCallback() { if (this.isConnected) this.render(); }

  async render() {
    const name = this.getAttribute('name');
    const size = this.getAttribute('size');
    // Bare number -> px; anything else is treated as a CSS length verbatim.
    if (size) this.style.fontSize = /^\d+(\.\d+)?$/.test(size) ? `${size}px` : size;
    if (!name) { this.innerHTML = ''; return; }

    const svg = await loadIcon(name);
    // The name may have changed while awaiting; don't clobber a newer render.
    if (this.getAttribute('name') !== name) return;
    this.innerHTML = svg;
  }
}

customElements.define('t-icon', TIcon);
