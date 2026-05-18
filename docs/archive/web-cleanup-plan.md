# web/ Cleanup Plan

## 1. CSS Consolidation

**Problem:** CSS variables are defined in three places with subtle drift (e.g. `--bg-surface`
is `#111009` in `site.css` and `#131109` in `styles.css`). `roadmap.html` has a 385-line
self-contained `<style>` block duplicating most of `site.css`. `index.html` has a 348-line
embedded `<style>` block.

**Changes:**
- Extract the shared Dark Spice Market color palette and spacing scale into a new `vars.css`,
  imported by both `site.css` and `styles.css`
- Move `index.html`'s embedded `<style>` block into `site.css`
- Move `roadmap.html`'s embedded `<style>` block into a new `roadmap.css` (or into `site.css`
  if the additions are small), and replace the `<style>` block with a `<link>` tag
- Replace the two inline font-size overrides on `.code-card` elements with named modifier
  classes (e.g. `.code-card--sm`)

---

## 2. Replace Baked Syntax Examples with a Highlighter

**Problem:** The code cards in `index.html` contain hundreds of hand-authored
`<span class="kw">...</span>` tags. They're painful to update and easy to get wrong.

**Recommendation: use [Prism.js](https://prismjs.com/)** -- it's ~2 KB (core, minified +
gzip), has no runtime dependencies, and supports custom language grammars defined as a plain
JS object. The grammar definition in `main.js` (Monaco) can be adapted directly for Prism.

The Turmeric grammar can live inline in `site.js` alongside the existing web components:

```js
Prism.languages.turmeric = {
  comment:     /;.*/,
  string:      /"(?:[^"\\]|\\.)*"/,
  keyword:     /\b(?:defn|defmacro|defstruct|defclass|definstance|defdata|defgadt|defeffect|let|let\*|letrec|if|cond|when|match|fn|do|begin|and|or|not|handle|perform|resume)\b/,
  type:        /:[a-zA-Z][a-zA-Z0-9_\-?!]*/,
  number:      /\b\d[\d._]*\b/,
  punctuation: /[()[\]{}]/,
};
```

The markup in `index.html` then becomes plain, readable Turmeric code in `<pre><code>` blocks
-- the toggle between `turmeric-version` and `sweet-exp-version` is kept as-is (two sibling
divs, toggled by `site.js`), but the content is just plain text that Prism highlights on page
load.

Prism's CSS output maps to `--syn-*` variables already defined in `site.css`, so no color
changes are needed.

---

## 3. `roadmap.html` -- Use Existing Web Components

**Problem:** `roadmap.html` has its own static `<nav>` and `<footer>` HTML duplicating what
`<site-nav>` and `<site-footer>` already generate. It also duplicates the reveal-on-scroll
`IntersectionObserver` that already lives in `site.js`.

**Changes:**
- Replace the static nav and footer with `<site-nav>` and `<site-footer>` (add
  `<script type="module" src="/site.js">`)
- Remove the duplicate IntersectionObserver script block (it's already in `site.js`)

---

## 4. `main.js` -- Targeted Cleanup

**Problem:** The file is 1,479 lines with several rough edges.

**Changes (scope-limited -- no architectural rewrites):**
- Remove the dead ANSI parsing code (lines ~111-154, commented out)
- Extract `TUTORIAL_STEPS` to a new `tutorials.js` file -- it's ~120 lines of data with no
  logic
- Deduplicate `runCode()` and the REPL-input eval path -- they share 90% of the same logic;
  extract a shared `executeCode(source)` function
- Add a debounce (~150ms) to the doc-panel search input (currently filters on every keystroke)

---

## 5. `site.js` -- Robustness

**Problem:** The syntax toggle handler uses brittle selectors with no error handling. The
`IntersectionObserver` is never disconnected.

**Changes:**
- Guard `filenameEl.dataset[key]` lookups with existence checks
- Disconnect the `IntersectionObserver` once all targets have been revealed (prevents the
  observer persisting for the page lifetime)

---

## 6. Remaining Inline Styles

A handful of inline styles left in `index.html` after phase 1:

- `style="display: none;"` on `.sweet-exp-version` -- handled automatically once Prism is in
  place (becomes a JS-managed show/hide as it is today, no change needed)
- `style="color:var(--border-str);"` on the influence-list separator dots -- add `.dot-sep`
  class in `site.css`
- `style="margin-bottom: 24px;"` and `style="margin-top: 80px;"` -- add spacing utilities or
  inline the rules in the relevant section class

---

## Execution Order

| Step | Scope    | Risk   |
|------|----------|--------|
| 1. Extract `vars.css` + reconcile variable drift | CSS only | Low |
| 2. Move `index.html` + `roadmap.html` `<style>` blocks to CSS files | CSS only | Low |
| 3. Prism.js + replace baked code examples | HTML + JS | Medium -- needs visual QA |
| 4. `roadmap.html` nav/footer -- web components | HTML | Low |
| 5. `main.js` dead code removal + `tutorials.js` extraction | JS | Low |
| 6. `main.js` deduplicate eval + debounce | JS | Medium -- needs REPL testing |
| 7. `site.js` robustness fixes | JS | Low |
| 8. Remaining inline styles | HTML/CSS | Low |

Steps 1-2 and 4 can be done in any order. Step 3 (Prism) should be done before steps 6-8 so
the code card HTML is clean before other passes touch `index.html`.
