# A fenced code block inside a blockquote renders as prose, not code

**Severity: medium.** Content is silently mangled in the rendered guide, and
the failure mode escalates to a hard `tur run docs` error (exit 1) only when
the mangled output happens to be malformed HTML -- so the quiet version can sit
in `main` for weeks, as it did here.

## Minimal repro

In any `docs/guides/*.md`:

```
> Some callout prose:
>
> ```sh
> # a comment
> cmd --flag <placeholder>
> ```
```

Then `./build/tur run docs`.

## What happens

`tools/genguides.py:1335` renders guides with
`md_lib.Markdown(extensions=['fenced_code', 'tables', 'toc'], ...)`.
python-markdown's `fenced_code` is a **preprocessor** whose fence regex anchors
at the start of a line, so a fence prefixed by `> ` is never recognized. The
blockquote processor then strips `> ` and the fence body is treated as ordinary
markdown block content, which means:

- the opening fence becomes literal text -- `<p>```sh</p>`;
- any `#` shell comment becomes a heading -- `<h1 id="a-comment">a comment</h1>`;
- `<placeholder>` is emitted as a raw, unclosed HTML tag.

Indented code blocks are unaffected: those are core markdown, handled *after*
the blockquote prefix is stripped, so `>     cmd` renders as a real
`<pre><code>`.

## Why it surfaced as a CI failure now

`docs/guides/releases-and-installation-guide.md` had carried a single-line
blockquoted fence since 4726f9ab4 (2026-09-30). With exactly one body line, the
two ``` markers paired up as an *inline code span*, so the output was
well-formed -- `<code>sh\ngh attestation verify turmeric-&lt;tag&gt;...</code>`,
a stray `sh` and no code block, but nothing the fragment validator could flag.
It was already rendering wrong; it just was not malformed.

3d3e027a2 added `#` comment lines to that block. Those became `<h1>` elements,
which broke the accidental inline-span pairing, so the placeholders emitted raw
and `tools/genpack.py`'s fragment validator failed the build:

```
error: guides/releases-and-installation-guide.html: line 48: <target> not closed before </p>
error: 4 malformed-fragment problem(s); the docs pane renders fragments with innerHTML, so these lose content
```

The validator is working as designed -- it caught real content loss. What it
cannot see is the quiet case above.

## Fix directions

The guide itself is fixed by using an indented code block inside the blockquote
(the companion change to this report). The general hole is still open; options,
cheapest first:

1. **Lint it. DONE.** `find_blockquoted_fences` in
   `tools/check-guide-pairs.py` rejects a fence line inside a blockquote and
   points at the indented-block form; it gates the `Check guide toggle pairs`
   job. Only fence OPENERS are reported, and the pattern is anchored at
   end-of-line so an inline span that merely *talks* about a fence (the
   4-backtick form in `package-management-guide.md:121`) is not flagged.
   Verified against the pre-fix guide, a nested `> >` fence and a `~~~` fence
   as positives, and against that inline span, a column-0 fence and the
   correct `>     cmd` form as negatives. The silent mangle is now an
   actionable error -- which is what was missing when this shipped.
2. **Pre-process blockquoted fences.** A preprocessor ahead of `fenced_code`
   that rewrites `> ```lang` ... `> ``` ` into an indented block keeps the
   source form authors expect. More code, and it has to not disturb the
   4-backtick inline spans that `package-management-guide.md:121` uses to
   *talk about* fences.
3. **Switch the guide renderer to a CommonMark implementation**, which supports
   fenced code in blockquotes natively. Largest change; would re-render every
   guide fragment, so it wants its own regen window.

Only one guide used the pattern at the time of writing, so (1) is likely the
right cost/benefit until that changes.
