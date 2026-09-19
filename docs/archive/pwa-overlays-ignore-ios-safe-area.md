---
status: RESOLVED 2026-09-18 -- confirmed in the installed app on the
  reporter's iPhone
severity: high (the docs pane has no reachable exit in the installed PWA)
discovered: 2026-09-17
area: web / Try Turmeric PWA (iOS standalone, safe-area insets)
---

# Try Turmeric PWA: `position: fixed` overlays ignore the safe-area inset, so the docs pane has no way out

## Summary

Two reports from an installed Try Turmeric PWA on an iPhone:

1. "I can't figure out how to go back when in a doc."
2. "PWA window is a bit pushed up. Some of topbar not accessible, black area
   at bottom."

They share a cause, which is what the reporter suspected. The app is served
`viewport-fit=cover` with
`apple-mobile-web-app-status-bar-style: black-translucent`, so in standalone
the top ~59 CSS px of the viewport is drawn over by the clock and the battery
indicator. `#app` reserved that strip. **Nothing else did** -- and
`position: fixed` resolves against the viewport, not against `#app`, so every
overlay escaped the padding the shell had reserved.

The docs pane is the overlay where that is fatal. Its 44px `.docs-topbar` sat
at viewport y 0-44, entirely inside a 59px inset, and **every control that
leaves the pane lives in that bar**: the close button, the Contents toggle, and
search. A reader who opened a doc saw an article and a footer and no way back.

## Repro

Install `/try/` to the Home Screen on a notched iPhone, open it, tap the docs
button, then try to leave the doc.

Headless, without an iPhone -- the geometry is the finding, and it reproduces
from the numbers alone:

```js
// 390x844, and 59px is an iPhone 15/16 portrait top inset.
document.documentElement.style.setProperty('--safe-top', '59px');
window.turmericApp.openDocsPane();
document.querySelector('#docs-close').getBoundingClientRect();
// pre-fix: { top: 7.5, bottom: 35.5, height: 28 }  -- all of it under the clock
```

`tests/mobile.safe-area.spec.js` is that measurement as a spec. It drives the
inset through the `--safe-*` tokens rather than `env()` because `env()` cannot
be set from script and Chromium will not report `(display-mode: standalone)`
under automation; the tokens exist partly for that reason.

## Root cause

`web/styles.css`. `.overlay` was `position: fixed; inset: 0` with no inset
padding, and `@media (max-width: 768px) .docs-overlay { padding: 0 }` made the
docs shell full-bleed on top of that. The only safe-area rule in the file was
on `#app`, which a fixed overlay is not laid out inside:

```css
@media (max-width: 1024px) and (display-mode: standalone) {
    #app { padding-top: env(safe-area-inset-top, 0px); padding-bottom: 0; }
}
```

Two aggravating factors on the same control. `.btn-close` is 28px, below the
44pt minimum tap target, and on a full-screen sheet a bare `x` in a trailing
corner does not read as the way out in the first place -- so even restored to
the screen it was a poor answer to "how do I go back".

## The bottom band

Report 2's "black area at bottom" is the same inset seen at the other end, and
this half is **diagnosed by arithmetic, not observed on the device.** Measuring
the reporter's screenshot (iPhone 15/16 class, 393x852 CSS px):

| | screenshot | code, as written |
|---|---|---|
| editor toolbar row | 60-96 | 59-95 (top inset reserved -- correct) |
| console footer bottom | ~793 | 852 |
| unpainted band | 793-852 = **59px** | none expected |

59px is exactly `safe-area-inset-top`. The shell took its height from
`100dvh`; if iOS in standalone resolves `100dvh` to the safe viewport
(852 - 59 = 793) rather than to the full screen `viewport-fit=cover` hands the
page, `#app` is 793 tall at y 0, its content ends at 793, and the remaining
59px shows through as unpainted canvas. Every number in the report follows.
Chrome resolves `100dvh` to the full 852 and cannot reproduce it, so this is
inference from the geometry and not a measurement.

**The earlier fix for this band misdiagnosed it.** 37d44e55d (2026-06-24)
dropped `padding-bottom: env(safe-area-inset-bottom)` on the theory that the
bottom inset was the dead space. The bottom inset is ~34px and the band is
~59px, so it was the top inset arriving at the other end; the band survived,
and the same report came back three months later.

The fix avoids needing to know which reading of `100dvh` iOS uses: the
standalone shell is **pinned** (`position: fixed; inset: 0`) instead of sized
from a viewport unit, so its bottom edge is the viewport's bottom edge under
every reading. The bottom inset is reserved by `.console-footer` growing, not
by padding `#app`, so the home-indicator strip is painted panel-colored and
continuous with the status line rather than reading as a dead band below the
app -- which is the complaint 37d44e55d was answering.

## Fix

`web/styles.css`, `web/try/index.html`, `web/tests/mobile.safe-area.spec.js`:

- `--safe-top/right/bottom/left` tokens on `:root`, zero outside standalone,
  `env(safe-area-inset-*)` under `(display-mode: standalone)`. Raw `env()` at
  each use site is what let one box reserve the inset and the rest forget;
  a token makes "which boxes reserve it" greppable, and script-settable for
  tests. `#ios-a2hs-hint` deliberately keeps raw `env()` -- it is shown only in
  a browser tab, where the tokens are zero by design.
- `.overlay` pads by the tokens, so a centered dialog clears the status bar.
- The docs sheet carries the insets on its bars (`.docs-topbar` padding-top,
  `.docs-footer` padding-bottom), so both still paint into the inset strips
  while their controls sit clear. Both size to content with a `min-height`;
  a hard-coded height is off by the 1px border once a 44px control is in the
  same border-box.
- `.docs-nav` is anchored to `.docs-body` (`top: 0; bottom: 0`) instead of
  restating the bar heights it floated between -- those now carry insets, and
  the restatement would have had to track them.
- The mobile exit control is leading-edge, 44px, and labeled `< Back`.
- The standalone shell is pinned, per above.

## Resolved 2026-09-18: confirmed on the device

The reporter checked the installed app and reports it fixed. That was the one
thing no checkout could answer, and it took a second commit to get there.

**The first fix never ran on the device.** It gated the safe-area tokens on
`(display-mode: standalone), (display-mode: fullscreen)` but the pinned shell
that consumes them on `(max-width: 1024px) and (display-mode: standalone)`
alone. iOS reports `fullscreen`, not `standalone`, for a home-screen app whose
status bar is black-translucent -- which `try/index.html` sets two lines above
the viewport meta -- so on the only platform with a notch to reserve, the
shell block never matched. `#app` kept `height: 100dvh` from the plain mobile
block and landed short by top+bottom inset (59 + 34 = 93 CSS px), painting the
shortfall through as the band. **The measured band matches that sum, not
either inset alone**, which is also why 37d44e55d's "drop `padding-bottom`"
reading did not hold. `site-nav, .footer { display: none }` lived in the same
dead block, so the nav was never hidden either -- in the report screenshot it
is on screen, crushed under the status bar.

`ce5a6ad3a` replaced both gates with one class, `html.pwa`, set by an inline
script in `try/index.html` before first paint from `navigator.standalone`
(definitive on iOS) falling back to the standalone/fullscreen/minimal-ui media
queries. One switch, so the two halves cannot drift apart again.

**Why it shipped green twice.** The test asserted that a rule containing
`position: fixed` EXISTS in the CSSOM -- reading rule TEXT via
`rule.conditionText.includes('display-mode: standalone')`, because standalone
cannot be emulated under automation -- and never that it applies. A dead rule
satisfies that perfectly. It now adds the class and MEASURES the rendered
shell: its position, that `#app`'s bottom edge reaches the viewport bottom
(the band's absence), and that the top inset is still reserved. A second test
pins the gate in the other direction, that an ordinary browser tab is NOT
pinned -- otherwise this would take the site nav off the page for every mobile
visitor.

The lesson worth carrying: a CSS fix behind a media query the test environment
cannot enter is unverifiable by construction, and asserting on rule text
instead is not a substitute -- it is an assertion that the author typed
something, which was true both times the band survived.
