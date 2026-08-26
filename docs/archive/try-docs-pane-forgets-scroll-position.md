# Try Turmeric: the docs pane forgets your scroll position when closed and reopened

**Severity: low** -- UX/enhancement, no correctness impact. The docs pane
remembers *which page* you were reading across a close/reopen but not *where in
it* you were, so reopening a long guide drops you back at the top and you have
to re-find your place.

## Summary

In the Try Turmeric REPL (`web/`), open the docs pane, navigate to a long guide,
scroll partway down, close the pane (Escape, the close button, or a click on the
backdrop), then reopen it. The pane returns to the same page -- and to the top
of it. The scroll offset is discarded.

The half-remembered state is what makes this feel like a bug rather than a
missing feature: `closeDocsPane` deliberately does **not** clear
`docsCurrentRef` (`web/main.js:3914-3919`), so reopening restores your page.
Having gone to that trouble, dropping the offset reads as an oversight. It bites
hardest in exactly the intended workflow -- read a guide, close it to try
something in the editor, reopen to continue -- which is the reason to have docs
inside the REPL at all.

## Current behavior, in code

`.docs-article` is the scrolling element (`web/styles.css:1949-1957`):

```css
.docs-article {
    flex: 1;
    min-width: 0;
    overflow-y: auto;
    padding: 24px 32px 64px;
    scroll-behavior: smooth;
```

Reopening with no explicit ref falls back to the remembered page
(`web/main.js:3901-3907`):

```js
const target = refWithAnchor
    || docsCurrentRef
    || (index && index.guides && index.guides.length
        ? `guides/${index.guides[0].slug}` : null);
if (target) showDocsPage(target);
```

and `showDocsPage` re-fetches the fragment, replaces `article.innerHTML`, and
ends by explicitly homing the scroll (`web/main.js:3795`):

```js
    // Anchor scrolling has to wait for the fragment to be in the document.
    if (anchor) {
        const target = article.querySelector(`#${CSS.escape(anchor)}`);
        if (target) { target.scrollIntoView({ block: 'start' }); return; }
    }
    article.scrollTop = 0;
```

There is no early-return for "this ref is already rendered", so a reopen always
re-renders and always resets. Nothing anywhere reads or writes a per-page scroll
offset -- the only `scrollTop` state in the file belongs to editor tabs
(`web/main.js:115`).

Note this is the docs **pane** (`#docs-overlay`, `openDocsPane`). The symbol
popover (`showDocPanel`, reached from a `data-doc-symbol` link) is a separate
surface and is out of scope here.

## Suggested behavior

Remember the scroll offset **per page ref**, not just one global offset -- the
pane is a browser over many pages, and coming back to guide A should not restore
guide B's offset. Restore on reopen and on back-navigation to a page you have
already read; still go to the top for a page opened for the first time, and
still let an explicit `#anchor` win over a remembered offset.

## Implementation notes

1. Keep a `Map<ref, scrollTop>` (a plain object is fine). Write to it from a
   `scroll` listener on `.docs-article`, throttled or on `requestAnimationFrame`,
   keyed by `docsCurrentRef`. Also capture in `closeDocsPane` before the overlay
   is hidden, so the last position is not lost to a throttle window.
2. In `showDocsPage`, replace the unconditional `article.scrollTop = 0` with:
   anchor wins if present; else a remembered offset for this ref if there is
   one; else 0.
3. **Watch `scroll-behavior: smooth`** on `.docs-article`
   (`web/styles.css:1954`). Assigning `scrollTop` under it animates, so a
   restore would visibly glide down the page on every reopen. Restore with
   `article.scrollTo({ top: saved, behavior: 'instant' })`, or toggle
   `scroll-behavior: auto` for the duration of the assignment. This is the one
   detail most likely to make an otherwise-correct fix feel broken.
4. Restore only *after* `article.innerHTML` is populated and laid out, or the
   element will not yet be tall enough to accept the offset and the assignment
   silently clamps to the scrollable range. The existing code already sequences
   the anchor scroll after render for this reason.
5. A remembered offset can also be stale-clamped if the fragment renders
   shorter than before (a docs pack update between visits). Clamping to
   `scrollHeight - clientHeight` is the browser's default and is the right
   behavior; no special handling needed, just do not assume the restored value
   round-trips.
6. Session-scoped in-memory state is enough. If it should survive a reload,
   the file already has a guarded `localStorage` helper with quota handling
   (`web/main.js:127-141`) and a `STORAGE_KEYS` map to add a key to -- but note
   `web/main.js:189` clears every `STORAGE_KEYS` entry on reset, so a new key
   joins that sweep automatically, which is probably what you want.

## Verification status

**Read-verified against the source, not exercised in a browser.** The code path
is unambiguous -- `article.scrollTop = 0` is unconditional on every
`showDocsPage`, and no scroll offset is persisted anywhere -- but per the house
rule that Try Turmeric behavior is confirmed in the browser rather than inferred,
a reproduction on the deployed page is worth doing before and after any fix. If
a fix appears not to land on the live site, check `sw.js` `CACHE_VERSION` before
suspecting the code.

---

## Resolution (2026-08-26)

Fixed in `web/main.js`, following the implementation notes. **Exercised in a
browser**, not just read-verified -- the report's own verification-status
section asks for that, and it turned out to matter (see "The trap" below).

### What changed

- `docsScrollByRef`, a `Map<ref, scrollTop>`, as note 1 specifies. Per-ref, so
  returning to guide A does not restore guide B's offset.
- Written from three places: `showDocsPage` banks the outgoing page's offset
  before it replaces `innerHTML`; `closeDocsPane` banks before hiding the
  overlay; and an rAF-coalesced `scroll` listener keeps the map current as a
  backstop. The first two are the mechanism -- they cover both ways of leaving
  a page -- and the listener only guards against a future exit path that
  forgets to bank.
- `showDocsPage`'s unconditional `article.scrollTop = 0` became: anchor wins,
  else a remembered offset for this ref, else 0 (note 2), still after the
  fragment is in the document (note 4).
- Restores go through `scrollTo({ behavior: 'instant' })` with a `scrollTop`
  fallback, per note 3. Under `.docs-article`'s `scroll-behavior: smooth` a
  plain assignment glides down the page on every reopen, which reads as the
  pane scrolling by itself rather than as returning you to your place.
- Session-scoped in memory, per note 6. No `STORAGE_KEYS` entry: surviving a
  reload is not worth it for something this cheap to re-establish.

Note 5's stale-clamp case needs no handling, as the report says -- the browser
clamps to `scrollHeight - clientHeight` and that is the right behaviour.

### One hazard the notes did not have

`rememberDocsScroll` refuses to record while the pane is closed. A hidden
element reports `scrollTop` 0, so a `scroll` callback still queued when the
overlay went `display: none` overwrites the offset `closeDocsPane` had *just*
banked -- with exactly the value the restore exists to avoid. Scroll, then hit
Escape inside the same frame, and the feature silently does nothing. The guard
is one `docsPaneIsOpen()` call; finding it without one is a bad afternoon.

### The trap: a scroll-restore test that passes against no restore at all

Worth writing down, because the first version of these tests passed against the
unfixed file and would have shipped as proof of a fix that was not there.

`showDocsPage` re-fetches the fragment. Until that promise resolves, the article
column is still showing -- and still scrolled to -- the previous render. A
`waitForFunction` poll accepts the first sample that matches, so it latches onto
that stale offset and reports success before the code under test has run. Two of
the three new tests passed against the pre-fix `main.js` this way.

The fix is to assert the *settled* state: wait for the fetch to clear
`aria-busy`, let the frame in which `showDocsPage` assigns `scrollTop` go by,
then take a single reading. That is what `settleArticle` / `expectArticleAt` in
the spec do.

The same shape hides a second confound: toggling `display: none`/`flex` on the
overlay *preserves* `scrollTop` in the DOM, so a close/reopen appears to keep
your place for a moment before the re-render homes it. Retention by accident of
the DOM is not restoration, and only the settled reading tells them apart.

### Coverage

Three tests in `web/tests/docs-pane.spec.js`:

- `reopening the pane restores where you were in the page` -- the report's
  workflow. Fails against pre-fix `main.js`, passes after.
- `each page remembers its own offset` -- two guides with different offsets,
  plus the assertion that a page opened for the *first* time still starts at
  the top. Fails against pre-fix, passes after.
- `an explicit anchor wins over a remembered offset` -- a **guard**, not a
  repro: it passes both ways. Restoring an offset is exactly the kind of change
  that quietly outranks an anchor, and that failure would present as "deep
  links stopped working" rather than as anything to do with scrolling.

All 11 tests in the file pass; the 8 pre-existing ones were run before the
change as a baseline and after, and the two repro tests were confirmed to fail
against the pre-fix `main.js`.

Scope of what was exercised: `docs-pane.spec.js` only. The rest of
`web/tests/` waits on `#wasm-status-text` reaching Ready, which needs a WASM
build this container does not have -- those tests cannot pass here whatever the
change is. `docs-pane.spec.js` deliberately does not wait for the WASM boot
(the pane must not depend on it), which is what makes it runnable, and it is
also the only file this change can affect.
