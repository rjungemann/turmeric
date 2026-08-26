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
