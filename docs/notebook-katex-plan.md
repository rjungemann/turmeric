# Notebook Plan: KaTeX Math Blocks

> **Status:** Draft Plan
> **Last Updated:** 2026-05-27
> **Type:** Spice Extension (`tur-notebook`)
> **Supersedes:** the "Math blocks" deferred item in
> [`notebook-spice-plan.md`](notebook-spice-plan.md) (Parser scope -> Deferred)

---

## Overview

Add first-class math support to `.tur.md` notebooks. Authors write LaTeX
between `$$ ... $$` (display) or `$ ... $` (inline); the notebook renderer
emits markup that [KaTeX](https://katex.org) typesets in the browser for
HTML output, and leaves the source untouched in the round-tripped `.md`
output so GitHub and pandoc render it natively.

The work lives entirely in the `tur-notebook` spice
(`../turmeric-spices/spices/notebook/`). No changes to the main turmeric
repo are required; this is a renderer / parser extension.

Why KaTeX rather than MathJax:

- Synchronous, no FOUC on page load -- KaTeX renders before paint when
  `renderMathInElement` is invoked at the bottom of `<body>`.
- Smaller (~280 KB gzipped CSS+JS+fonts) than MathJax 3 (~1.1 MB).
- Self-contained: no font CDN dependency once vendored.
- No JavaScript execution required at parse time -- the renderer just
  emits placeholder spans / divs and lets KaTeX walk the DOM client-side.

Drawback: KaTeX implements a subset of LaTeX (no `\newcommand` chains,
no `\begin{tikzpicture}`, etc.). The spice documents the supported
[function list](https://katex.org/docs/supported.html); authors who hit
an unsupported macro can fall back to embedding pre-rendered SVG via the
existing image pipeline.

---

## Scope

### In scope (v0.1)

- Parser recognition of `$$ ... $$` as a new block AST node
  (`md-tag-math-block`).
- Parser recognition of `$ ... $` as a new inline AST node
  (`md-tag-math-inline`).
- HTML renderer emits `<span class="math inline">\(...\)</span>` /
  `<span class="math display">\[...\]</span>` (KaTeX auto-render delimiter
  convention).
- HTML renderer injects vendored KaTeX CSS, JS, and the
  auto-render extension into the rendered page's `<head>` / end-of-`<body>`,
  with `delimiters: [{left:'$$', right:'$$', display:true}, {left:'$',
  right:'$', display:false}]`.
- Markdown renderer round-trips math nodes verbatim (`$$...$$` stays
  `$$...$$`).
- Cell output capture: if a cell prints a string that looks like
  `$$...$$`, it is rendered as math in the HTML output block (opt-in via
  a `math=true` attribute -- see [Cell output math](#cell-output-math)).

### Out of scope (deferred)

| Future work | What it adds |
|-------------|--------------|
| Server-side render | KaTeX SSR via a vendored WASM build, eliminating client JS |
| MathML fallback | Emit MathML in parallel for accessibility / no-JS |
| `equation` env | First-class numbered equations with `\label` / `\ref` |
| TUI math preview | ASCII or sixel rendering of math in the terminal |
| `\newcommand` | Notebook-level macro preamble loaded across cells |

---

## File format

A math block is a fenced span on its own line, opened and closed by a
line that *begins* with `$$`:

````markdown
The Gaussian PDF is

$$
f(x) = \frac{1}{\sigma\sqrt{2\pi}}\,e^{-\tfrac{1}{2}\left(\tfrac{x - \mu}{\sigma}\right)^{2}}
$$

so when $\sigma = 1$ and $\mu = 0$ we recover the standard normal.
````

Rules:

- Display math (`$$`) opens at column 0 and closes at column 0. The body
  may span multiple lines and may itself contain `$`.
- Inline math (`$`) opens and closes on the same logical line. A literal
  `$` is written `\$`. To avoid the well-known "currency conflict"
  ambiguity, an opening `$` is only recognized when **not** immediately
  followed by whitespace or a digit, matching the pandoc rule.
- Math content is *not* CommonMark-parsed -- it passes through to KaTeX
  verbatim. Inside math, the only escape recognized by the parser is
  `\$` (so the close delimiter can be quoted in source).

This matches the Quarto / Jupyter / GitHub flavor and means existing
`.tur.md` notebooks that already use `$$ ... $$` (which today parse as
literal text per `notebook-spice-plan.md:864`) start rendering correctly
without any source edits.

---

## Parser changes (`notebook/cmark`)

Two new AST tags slot in after the existing inline / block set:

```turmeric
(md-tag-math-block)      ;; 22  block; md-node-text holds LaTeX source
(md-tag-math-inline)     ;; 23  inline; md-node-text holds LaTeX source
```

### Block-level scan

In `md-parse-block` (the line-by-line block dispatcher), insert a math
probe before the fenced-code-block probe -- both look at the start-of-line,
and `$$` is unambiguous:

1. If the current line trimmed-left starts with `$$` and contains no
   other non-whitespace before EOL, open a math block.
2. Scan forward, accumulating raw lines until a line whose trimmed-left
   value is `$$` (with optional trailing whitespace).
3. Emit a `md-tag-math-block` node whose `md-node-text` is the joined
   inner lines (LF-separated, no trailing newline).
4. Closing fence not found before EOF -> emit the original lines as a
   paragraph (graceful degradation), mirroring how unterminated fenced
   code blocks are handled today.

A one-line form `$$ x = 1 $$` is recognized too: opening `$$`, content,
closing `$$` all on one line collapses into a single math-block node.

### Inline scan

In `md-parse-inline` (the span scanner), add a `$` delimiter rule that
runs **before** the emphasis-pair resolution pass:

1. A `$` is an opener iff the next character is not whitespace and not a
   digit, and the preceding character (if any) is not a backslash.
2. From an opener, scan forward for the next un-escaped `$`. The closer
   must be preceded by a non-whitespace character.
3. If both ends match, emit `md-tag-math-inline` with the bytes between
   them as `md-node-text`.
4. If no closer is found, treat the opening `$` as a literal text byte
   (no error, mirrors how unmatched `*` is treated).

Backslash-escaped `\$` decays to a literal `$` text node in the normal
escape pass, **except** inside an open math span where `\$` stays as the
two literal characters (KaTeX consumes the escape itself).

### Emission round-trip (`md-emit`)

- Math block -> `$$\n<body>\n$$` (always block form, even if the source
  was the one-line variant; idempotent under repeated parse/emit).
- Math inline -> `$<body>$`.

### HTML emission (`md-emit-html`)

- `md-tag-math-block` -> `<span class="math display">$$<body>$$</span>`
- `md-tag-math-inline` -> `<span class="math inline">$<body>$</span>`

The literal `$$` / `$` are kept inside the span so KaTeX's auto-render
extension finds them via its delimiter scan. The wrapping `<span
class="math ...">` is a defensive label for CSS / future SSR.

The body is **not** HTML-escaped beyond `&`, `<`, `>` -> entity. KaTeX
expects raw LaTeX bytes inside its delimiters; emitting `&lt;` would
break `<`-based macros (rare, but they exist).

---

## Renderer changes (`notebook/render-html`)

Two additions to `render-html-string`:

1. **Asset embedding.** Concatenate three vendored files into the
   page `<head>` (or load them by relative `<link>` / `<script>` when
   `opts.assets = sibling`):
   - `katex.min.css`
   - `katex.min.js`
   - `auto-render.min.js` (the contrib `renderMathInElement` script)

   Embedding mode (the default) inlines them with `<style>` and
   `<script>` blocks so the resulting HTML file is fully self-contained
   and works offline -- consistent with the existing image embedding
   default. Sibling mode writes the three files next to the HTML output
   for users who want lighter pages and are serving over HTTP. This
   mirrors the existing `--images=inline|sibling` flag on `tur nb export
   html`.

   The KaTeX font files (`KaTeX_Main-Regular.woff2` etc., ~270 KB total)
   are **always** sibling files in their own `katex-fonts/` directory
   regardless of mode. Inlining `woff2` as base64 inside the CSS works
   but inflates the HTML by ~360 KB per export and prevents font
   caching across multiple notebooks served from one directory; the
   tradeoff isn't worth it. In `inline` mode the vendored CSS has its
   `@font-face src` rewritten to point at this sibling directory.

2. **Auto-render bootstrap.** A small `<script>` at end-of-`<body>`:

   ```html
   <script>
     document.addEventListener("DOMContentLoaded", function () {
       renderMathInElement(document.body, {
         delimiters: [
           {left: "$$", right: "$$", display: true},
           {left: "$",  right: "$",  display: false}
         ],
         throwOnError: false,
         errorColor: "#cc0000",
         strict: "ignore"
       });
     });
   </script>
   ```

   `throwOnError: false` means a malformed expression renders as its red
   source text rather than blocking everything else on the page.

A new `opts.math` field (default `auto`) controls whether the KaTeX
bundle is emitted at all:

| Value | Behavior |
|-------|----------|
| `auto` | Embed iff any math node exists in the document (saves ~280 KB on math-free notebooks) |
| `always` | Always embed (useful if cells emit math at runtime) |
| `never` | Never embed; math nodes render as their raw `$$...$$` source |

---

## Cell output math

A cell can emit math at runtime by adding `math=true` to its fence
attributes:

````markdown
```turmeric {math=true}
(println "$$ E = mc^2 $$")
```
````

When `math=true`, the renderer wraps `cell-output.stdout` in a
`<div class="math-output">` instead of `<pre class="cell-output">`, runs
the same `$$...$$` / `$...$` recognition pass over its bytes, and emits
math spans interleaved with text. This is the only way runtime-generated
math is recognized -- non-`math=true` cell outputs render as
preformatted text exactly as today.

Why opt-in: many notebooks print debug values containing `$` (paths,
shell snippets, jq output). Auto-detecting math in every cell's stdout
would mis-render those.

The `math=true` attribute also implies `opts.math = always` for that
notebook -- the renderer has to embed KaTeX even if no static math
node exists in the prose, because the math is only visible at evaluation
time.

---

## Vendoring

The KaTeX assets ship inside the spice tree under
`spices/notebook/src/notebook/vendor/katex/`:

```
katex/
  katex.min.css
  katex.min.js
  auto-render.min.js
  LICENSE          -- KaTeX MIT license file
  VERSION          -- pinned release tag (e.g. "0.16.11")
  fonts/
    KaTeX_Main-Regular.woff2
    KaTeX_Main-Bold.woff2
    KaTeX_Math-Italic.woff2
    ...            -- the woff2 subset of the KaTeX font directory
```

The `woff2`-only subset is ~270 KB; the full font directory (woff, ttf,
woff2 for legacy browsers) is ~1.4 MB. Modern browsers all support
woff2; supporting IE / pre-2016 browsers isn't a goal for a literate
programming tool emitted from a Turmeric build.

The vendored bundle is checked into `turmeric-spices`. A `Justfile`
target (`just vendor-katex <version>`) wraps the download / verify /
extract steps so future bumps are reproducible. The README documents
the update procedure (analogous to the existing `style.css` snapshot
refresh in NB7).

---

## CLI surface

No new subcommands; math support is automatic.

- `tur nb export html foo.tur.md` -- embeds KaTeX iff math is present
  (the `auto` default).
- `tur nb export html foo.tur.md --math=always|auto|never` -- override.
- `tur nb export html foo.tur.md --assets=inline|sibling` -- already
  exists for CSS / images; gains KaTeX as a third asset class.
- `tur nb render foo.tur.md --to md` -- math nodes round-trip as
  `$$...$$` / `$...$` source bytes; KaTeX is irrelevant in this path.

---

## Phases

- [ ] **KX0** -- `notebook/cmark` adds `md-tag-math-block` and
  `md-tag-math-inline`; block-level and inline scanners recognize the
  delimiters; round-trip parse/emit/parse golden tests.

- [ ] **KX1** -- `md-emit-html` emits `<span class="math display|inline">`
  wrappers. Tests: math-only fixtures render with the right wrapper and
  the right inner literal bytes.

- [ ] **KX2** -- Vendor KaTeX 0.16.x into `spices/notebook/src/notebook/
  vendor/katex/`; `just vendor-katex` task; LICENSE / VERSION files.

- [ ] **KX3** -- `notebook/render-html` embeds the vendored CSS / JS /
  auto-render script and the bootstrap `<script>` block. Implements
  `opts.math = auto | always | never` (the `--math` flag on `tur nb
  export html`).

- [ ] **KX4** -- `--assets=sibling` mode writes `katex.min.css`,
  `katex.min.js`, `auto-render.min.js`, and the `katex-fonts/` directory
  next to the rendered HTML; HTML uses `<link>` / `<script src>` paths
  instead of inline blocks.

- [ ] **KX5** -- `math=true` cell attribute: post-process
  `cell-output.stdout` through the same math scanner; wraps output in
  `<div class="math-output">`; implies `opts.math = always`.

- [ ] **KX6** -- Docs: `docs/guides/notebook-guide.md` gains a "Math"
  section with the inline / display syntax, the supported-functions
  link, the unsupported-macro fallback (pre-rendered SVG via the image
  pipeline), and the `math=true` example. Update
  `notebook-spice-plan.md` to remove math from the "Deferred" list and
  link to this plan.

- [ ] **KX7** -- Example: `examples/math-walkthrough.tur.md` exercising
  inline math, display math, multi-line equations, a `math=true` cell,
  and a deliberately broken expression to show the red error-color
  fallback.

---

## Design notes

### Why pandoc-style delimiter rules and not GFM

GitHub recently shipped its own math support using `$$...$$` for display
math but with stricter line rules (the opening `$$` must be alone on a
line; inline `$...$` must not cross line boundaries). Pandoc's rule set
is a superset of GitHub's and the existing convention in Quarto /
Jupyter -- the audience this format already targets. Files that pass
the stricter GitHub rules also pass the pandoc rules, so adopting
pandoc costs nothing on the GitHub-rendering side.

### Why client-side render rather than server-side

KaTeX has a Node SSR mode (`katex.renderToString`). Calling it from
Turmeric would require either bundling a JavaScript engine into the
spice or shelling out to `node`. The first contradicts the
"no C dependencies, pure Turmeric" stance of `tur-notebook`; the second
makes notebook export depend on the user having `node` on `$PATH`,
which is the kind of friction we explicitly avoided when picking the
parser, TUI, and image pipelines. Client-side render is one HTTP
request worth of slower first paint in exchange for a build that works
on any machine with `tur` installed. SSR remains a candidate for a
future `tur-notebook` v0.2 if a Turmeric-native WASM JS runtime
("`tur-quickjs`"?) appears.

### Why no math support in the TUI

Math rendering in a terminal is either ASCII-art (ugly, hard to read,
~1000 lines of layout code) or sixel / Kitty image protocol (depends
on a heavyweight off-process TeX engine to generate the image). Neither
fits the TUI's "fast and dependency-free" design. The TUI displays
math cells as their raw `$$...$$` source bytes, same as any other
prose. Users who want to *see* the math run `tur nb export html` and
open the result -- the same workflow as today for cells that emit
images. The TUI guide spells this out.

### Why not a `math` fence language

`notebook-spice-plan.md:864` suggested a fourth fence language (`math`)
alongside `turmeric` and `sweet-exp`. We don't pursue that here because:

- Math blocks aren't *executable* the way code cells are -- they have
  no session, no output, no `eval=` / `echo=` / `cache=` attributes
  worth honoring. Reusing the cell apparatus for static content adds
  conceptual weight for no gain.
- `$$...$$` is the convention every other notebook tool already uses;
  authors don't have to learn a Turmeric-specific spelling.
- The `notebook/cmark` parser can recognize math blocks at the same
  cost (one extra line probe) as a `math` fence would, with no fence
  attribute machinery to wire up.

If a future use case genuinely needs executable math -- e.g. a `math`
cell that calls into a CAS spice and replaces itself with its rendered
output -- it can be added as a `math=true` cell attribute on a
`turmeric` cell rather than a new fence language.

### Why not strip the closing `$$` from emit and rely on `<span class="math display">` alone

KaTeX's auto-render extension finds math by *scanning for delimiters*
in text nodes, not by matching `class` attributes. If we stripped the
`$$` from the inner text, the script would see only an empty span and
skip it. Keeping the source delimiters inside the wrapper is the only
way to make auto-render and the CSS class label coexist without
shipping a custom render driver. The same applies to inline `$...$`.

### Failure modes

- **Malformed LaTeX** -- KaTeX renders the offending source in red
  (`errorColor`) and continues with the rest of the page. Authors see
  the broken span immediately on reload; nothing else on the page is
  affected. We do **not** parse LaTeX at build time -- doing so would
  require shipping a LaTeX parser, which is a research project.
- **Mismatched delimiters** -- the parser gracefully degrades
  (unterminated `$$` becomes a paragraph, unterminated inline `$`
  becomes a literal `$` text byte). No build error; the source renders
  visibly wrong, which is the right feedback loop.
- **`$` inside code spans / code blocks** -- inline code spans
  (`` `...` ``) and fenced code blocks already short-circuit the inline
  / block parsers; math recognition runs *after* those, so a shell
  snippet `` `echo $PATH` `` stays literal. This is the same precedence
  pandoc uses.

---

## Open questions

- Do we want a `--math-engine=katex|none` future-proofing flag, or is
  `opts.math = never` sufficient? Leaning toward the latter -- adding a
  second engine isn't on the roadmap and we can rename the flag if it
  ever is.
- Should `math=true` be the default for all `output=true` cells whose
  output starts with `$$`? Tempting but fragile (see the path /
  jq-output concern above). Keep opt-in for v0.1.
- Where does the supported-function reference live? Probably a link out
  to `katex.org/docs/supported.html` in the notebook guide; mirroring
  the full list locally would rot quickly.
