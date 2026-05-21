# Guide Syntax Toggle Plan

A plan to give every code example in the Turmeric guides two interchangeable
forms -- a standard S-expression version (`#lang turmeric`) and a
sweet-expression version (`#lang sweet-exp`) -- presented behind an in-page
toggle, matching the `turmeric` / `sweet-exp` segmented control already shipped
on the marketing homepage (`web/index.html`).

The work splits into three concerns, addressed in order: deciding how authors
write paired examples, building the rendering pipeline that turns a pair into a
toggle, and the (large) content effort of writing the second version for ~549
existing code blocks.

Task IDs use the `ST` (Syntax Toggle) prefix.

---

## 1. Background and current state

### How guides are authored and rendered

- Guides are plain Markdown in `docs/guides/*.md` (44 files, excluding
  `README.md`).
- `tools/genguides.py` renders them to `docs/html/guides/*.html`. It is invoked
  by the `guides` target in `Justfile` (`just guides`, also run as a dependency
  of `just docs`).
- Code examples use fenced blocks tagged ` ```turmeric `. There are **549**
  such blocks today. The Python `markdown` library (`fenced_code` extension)
  turns each into `<pre><code class="language-turmeric">...</code></pre>`.
- `genguides.py` injects `TURMERIC_HIGHLIGHT_JS`, a client-side tokenizer that
  highlights every `pre code.language-turmeric` element.
- Guide CSS is inlined by `genguides.py` via the `GUIDE_CSS` constant; the
  shared site stylesheet (`docs/html/api/style.css`) is linked separately.

### How the homepage toggle works

`web/index.html` has hand-authored example cards. Each card contains:

```html
<div class="code-card">
  <div class="code-card-bar">
    ...
    <span class="code-card-filename" data-turmeric="effects.tur"
          data-sweet-exp="effects.neoteric">effects.tur</span>
    <div class="code-syntax-toggle">
      <button class="seg-btn active" data-syntax="turmeric">turmeric</button>
      <button class="seg-btn" data-syntax="sweet-exp">sweet-exp</button>
    </div>
  </div>
  <div class="code-card-body">
    <div class="code-version turmeric-version">;; S-expression form ...</div>
    <div class="code-version sweet-exp-version" style="display:none">#lang ...</div>
  </div>
</div>
```

- The toggle handler lives in `web/site.js` (lines ~123-148): on a `.seg-btn`
  click it flips `.active`, swaps the filename via `data-*` attributes, and
  shows/hides `.code-version` children scoped to the enclosing `.code-card`.
- Styling (`.code-syntax-toggle`, `.seg-btn`, `.code-version`) lives in
  `web/site.css` (lines ~313-340).

### What `#lang` values the compiler actually accepts

`detect_lang()` in `src/compiler/reader.c` recognizes exactly four directives:

| Directive | Reader |
|-----------|--------|
| `#lang turmeric` | standard S-expression |
| `#lang turmeric/curly-infix` | SRFI-105 curly-infix only |
| `#lang turmeric/neoteric` | curly-infix + neoteric `f(x)` calls |
| `#lang sweet-exp` | full sweet-expressions (indentation + neoteric + curly-infix) |

Note: the homepage currently mixes `#lang turmeric/sweet-exp` (card 1) and
`#lang turmeric/neoteric` (cards 2-3) inside blocks both labelled
*sweet-exp*. `#lang turmeric/sweet-exp` is **not** a valid directive -- the
implemented name is `#lang sweet-exp`. This inconsistency must be resolved
before it propagates into 44 guides (see ST0.1, ST5.3).

---

## 2. Design decisions

### ST0.1 -- Pin down what the "sweet-exp" variant means

**Decision needed before any content work.** Standardize:

- The toggle's second label is `sweet-exp`.
- The second variant always carries the directive `#lang sweet-exp` (the full
  reader, which subsumes curly-infix and neoteric). Do not use the invalid
  `#lang turmeric/sweet-exp`, and do not mix `turmeric/neoteric` into
  sweet-exp-labelled examples.
- The first variant carries `#lang turmeric` only when the example is a
  complete, runnable program; inline snippets omit the directive (as guides do
  today).

**Action:** record this convention in `docs/guides/README.md` and reconcile the
homepage in ST5.3.

### ST0.2 -- Authoring convention: adjacent paired fences

Authors write the two versions as **two consecutive fenced blocks**, the
`turmeric` block immediately followed (no prose between) by a `sweet-exp` block:

````markdown
```turmeric
(defn use-ask [] :int
  (+ 1 (perform (Ask))))
```
```sweet-exp
defn use-ask [] :int
  {1 + perform(Ask())}
```
````

Rules:

- A `turmeric` block with **no** immediately-following `sweet-exp` sibling
  renders exactly as today (plain code block, no toggle). This keeps all 549
  existing blocks valid and makes the rollout fully incremental.
- A lone `sweet-exp` block (no preceding `turmeric`) also renders plain.
- The pair is detected by `genguides.py`, not by Markdown -- the two blocks stay
  ordinary fenced code, so editors, linters, and `grep` keep working.

**Alternatives considered and rejected:**

- *Single fence with an inline `;; ---8<---` separator* -- breaks syntax
  highlighting and copy-paste of each half.
- *Author only `turmeric`, auto-generate `sweet-exp` at build time* -- viable
  only once a reliable S-expr->sweet-exp printer exists (Phase ST2); even then,
  hand-tuned examples read better. Auto-generation is an accelerator, not the
  authoring model.

---

## 3. Rendering infrastructure (`genguides.py`, CSS, JS)

### ST1.1 -- Pair detection and toggle markup in `genguides.py`

After `md_lib.Markdown(...).convert(text)`, post-process `body_html`:

- Find each `<pre><code class="language-turmeric">` element immediately
  followed by `<pre><code class="language-sweet-exp">` (allowing only
  whitespace between).
- Wrap the pair in a toggle widget reusing the homepage class names so guide
  pages and the homepage look identical:

  ```html
  <div class="code-card code-toggle">
    <div class="code-card-bar">
      <div class="code-syntax-toggle">
        <button class="seg-btn active" data-syntax="turmeric">turmeric</button>
        <button class="seg-btn" data-syntax="sweet-exp">sweet-exp</button>
      </div>
    </div>
    <div class="code-card-body">
      <div class="code-version turmeric-version"><pre><code ...></code></pre></div>
      <div class="code-version sweet-exp-version" style="display:none">
        <pre><code ...></code></pre></div>
    </div>
  </div>
  ```

- Leave unpaired blocks untouched.

**Files:** `tools/genguides.py`. **Acceptance:** rendering a guide with one
paired example produces a single toggle card; a guide with only lone blocks is
byte-identical to current output except for unrelated changes.

### ST1.2 -- Toggle CSS in `GUIDE_CSS`

Port `.code-syntax-toggle`, `.seg-btn`, `.seg-btn.active`, `.code-version`, and
the minimal `.code-card` bar styling from `web/site.css` into the `GUIDE_CSS`
constant in `genguides.py` (guides inline their CSS; they do not load
`site.css`). Ensure the CSS variables it references (`--gold`, `--border`,
etc.) exist in `docs/html/api/style.css`; add fallbacks for any that do not.

**Files:** `tools/genguides.py` (and `docs/html/api/style.css` if vars missing).

### ST1.3 -- Toggle JS for guide pages

Add a small script (alongside `SIDEBAR_TOGGLE_JS` / `TURMERIC_HIGHLIGHT_JS` in
`genguides.py`) that replicates the `web/site.js` handler: per-card,
click-to-switch, scoped show/hide of `.code-version`. Guides have no filename
chip inside code blocks, so the `data-*` filename swap is omitted.

Keep it a straight port so the two implementations do not drift; if practical,
extract the handler into one shared snippet referenced by both `site.js` and
`genguides.py`.

**Files:** `tools/genguides.py`.

### ST1.4 -- Highlight `language-sweet-exp` blocks

`TURMERIC_HIGHLIGHT_JS` currently selects only `pre code.language-turmeric`.
Extend it to also process `language-sweet-exp`. The existing tokenizer handles
strings, `;` comments, `:type` annotations, numbers, and keywords, which covers
most sweet-exp tokens. Verify/adjust for: the leading `#lang sweet-exp` line,
curly-infix `{ ... }`, and neoteric `name(...)` (the `(` should not change
tokenization). No indentation-specific colouring is required.

**Files:** `tools/genguides.py`. **Acceptance:** a sweet-exp block renders with
keyword/string/number colours and no raw `<`/`&` escaping bugs.

### ST1.5 -- (Optional) Sticky per-page preference

Persist the reader's last choice in `localStorage` and apply it to every toggle
card on load, so a reader who picks `sweet-exp` sees it everywhere. Optional
polish; ship ST1.1-ST1.4 first.

**Files:** `tools/genguides.py`.

---

## 4. Translation accelerator (optional, recommended)

Hand-writing a sweet-exp version for 549 blocks is the bulk of the effort. A
printer can produce solid first drafts.

### ST2.1 -- S-expression -> sweet-exp printer

The compiler already has a sweet-exp *reader* (`READER_SWEET` in
`src/compiler/reader.c`) but no *writer*. Add a pretty-printer that walks the
`Form` IR and emits sweet-expression text (indentation, neoteric `f(x)`,
curly-infix `{a + b}` for binary operators). Expose it as a CLI mode, e.g.
`tur fmt --sweet <file>`, reusing the existing formatter (`src/compiler/fmt.c`,
see `docs/guides/formatter-guide.md`).

**Files:** `src/compiler/fmt.c` + `src/main.c` (or a new `tools/` script).

### ST2.2 -- Bulk draft generator

A `tools/` script that scans a guide, and for every lone ` ```turmeric ` block
runs ST2.1 to emit a draft ` ```sweet-exp ` sibling, inserted right after.
Output is a **draft for human review**, never committed unreviewed -- sweet-exp
formatting has aesthetic choices (when to break lines, when to prefer `{}` over
prefix) the printer cannot always get right.

**Files:** new `tools/gen-sweet-variants.py` (or similar).

> If ST2 is skipped, content authors write both versions by hand; the rest of
> the plan is unaffected.

---

## 5. Equivalence verification harness

The two versions of an example must mean the same thing, and must not drift as
guides are edited.

### ST3.1 -- Pair equivalence checker

A tool that extracts every toggle pair from `docs/guides/*.md` and verifies the
two blocks are equivalent:

- **Parse-equality** (default): feed each block to its reader and compare the
  resulting `Form` trees structurally. Works for snippets that are not complete
  programs.
- **Eval-equality** (when the block is a runnable program): compile/run both
  with `tur` and compare stdout.

Snippets that legitimately cannot be checked (deliberately partial code, error
examples) are skipped via an explicit allowlist or an info-string marker such
as ` ```turmeric no-check `.

**Files:** new `tools/check-guide-pairs.py`.

### ST3.2 -- Wire into build and CI

Add a `check-guides` target to `Justfile` running ST3.1, and invoke it from the
GitHub Actions workflow under `.github/`. A drifted or non-equivalent pair
fails the build.

**Files:** `Justfile`, `.github/workflows/*`.

---

## 6. Content rollout

Each guide is independent; convert in batches. Per the existing repo rules,
all code in `docs/guides/**` and `tests/fixtures/**` must be **ASCII only**
(use `--`, never em dashes) -- this applies to the new sweet-exp blocks too.

### ST4.1 -- Pilot (3 high-traffic guides)

Convert `quickstart.md` (28 blocks), `repl-tutorial.md` (49), and
`effects-system-guide.md`. Validate the full pipeline end-to-end: authoring
convention, rendering, highlighting, ST3 verification. Adjust ST0-ST3 based on
what the pilot surfaces before scaling.

### ST4.2 -- Remaining guides, by category

Roll out the rest in batches grouped by the `docs/guides/README.md` categories.
Largest remaining: `tidal-guide.md` (50), `tidal-cookbook.md` (49),
`threading-guide.md` (31), `web-continuations-tutorial.md` (24),
`error-handling-guide.md` (24). Each guide can merge independently; a guide is
"done" when all its `turmeric` blocks worth a second form have a verified
`sweet-exp` sibling. Examples where a sweet-exp form adds nothing (e.g. a bare
REPL value) may stay lone.

### ST4.3 -- Coverage tracking

Track conversion progress (e.g. a checklist in this file, or a counter in
ST3.1's output: "X / 549 blocks paired"). Update `docs/guides/README.md` with a
short note that examples are available in both syntaxes.

---

## 7. Polish and testing

### ST5.1 -- Rendered-page smoke test

Add a Playwright test (alongside `web/tests/`) that loads a rendered guide page
with a toggle, clicks `sweet-exp`, and asserts the sweet-exp block becomes
visible and the turmeric block hides. Cover at least one pilot guide.

**Files:** `web/tests/`, `web/playwright.config*.js`.

### ST5.2 -- Accessibility

Give the segmented control proper semantics: `role="tablist"` / `role="tab"`,
`aria-selected`, and arrow-key navigation. Apply the same fix to the homepage
control so both stay consistent.

**Files:** `tools/genguides.py`, `web/index.html`, `web/site.js`.

### ST5.3 -- Fix homepage `#lang` inconsistency

Correct `web/index.html`: the card-1 sweet-exp block uses
`#lang turmeric/sweet-exp` (invalid) and cards 2-3 use `#lang turmeric/neoteric`
under a `sweet-exp` label. Standardize all three sweet-exp blocks on
`#lang sweet-exp` per ST0.1.

**Files:** `web/index.html`.

---

## 8. Task summary

| ID | Task | Depends on |
|----|------|-----------|
| ST0.1 | Pin down sweet-exp variant + `#lang sweet-exp` directive | -- |
| ST0.2 | Document adjacent-paired-fence authoring convention | ST0.1 |
| ST1.1 | Pair detection + toggle markup in `genguides.py` | ST0.2 |
| ST1.2 | Toggle CSS into `GUIDE_CSS` | ST1.1 |
| ST1.3 | Toggle JS for guide pages | ST1.1 |
| ST1.4 | Highlight `language-sweet-exp` blocks | ST1.1 |
| ST1.5 | (Optional) sticky per-page preference | ST1.3 |
| ST2.1 | S-expr -> sweet-exp printer (`tur fmt --sweet`) | -- |
| ST2.2 | Bulk draft generator script | ST2.1 |
| ST3.1 | Pair equivalence checker | ST0.2 |
| ST3.2 | Wire `check-guides` into `Justfile` + CI | ST3.1 |
| ST4.1 | Pilot: quickstart, repl-tutorial, effects-system | ST1.*, ST3.1 |
| ST4.2 | Remaining guides by category | ST4.1 |
| ST4.3 | Coverage tracking + README note | ST4.2 |
| ST5.1 | Playwright smoke test for guide toggle | ST1.* |
| ST5.2 | Accessibility for the segmented control | ST1.3 |
| ST5.3 | Fix homepage `#lang` inconsistency | ST0.1 |

**Critical path:** ST0.1 -> ST0.2 -> ST1.1 -> (ST1.2, ST1.3, ST1.4) -> ST4.1
-> ST4.2. ST2.* and ST3.* run in parallel and accelerate / guard ST4.

**Minimum viable slice:** ST0.1, ST0.2, ST1.1-ST1.4, ST4.1, ST5.3 -- delivers a
working toggle on three guides plus a consistent homepage. ST2, ST3, and the
full ST4 rollout follow incrementally without blocking a first release.
