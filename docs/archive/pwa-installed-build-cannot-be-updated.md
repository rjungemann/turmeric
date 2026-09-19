---
status: RESOLVED 2026-09-18 -- recovery lever in ebcdda4b0 (#903), update
  lifecycle in c9afe0aac (#904)
severity: high (an installed PWA can be stuck on an old build indefinitely, with
  no user-reachable way out)
discovered: 2026-09-17
area: web / Try Turmeric PWA (service worker update lifecycle)
---

# An installed Try Turmeric PWA has no way to notice or apply a new build

## Summary

An installed Try Turmeric on iOS sat several builds behind for months. Every
lever a user has was tried and none of them shifted it:

- relaunching the app,
- the in-app **Force update**,
- deleting the Home Screen icon and re-adding it,
- clearing Safari's website data.

Safari on the same phone was current the whole time. That is not the
contradiction it looks like, and it is worth stating because it wasted a round
of diagnosis: nearly all of the PWA-specific CSS is gated on
`@media (display-mode: standalone)`, so a Safari tab renders identically on a
stale build and a current one. "Works in Safari" discriminates nothing.

## What the app does about updates

Nothing. `web/main.js`:

```
$ grep -c "controllerchange\|\.update()" web/main.js
0
```

The whole of it is:

```js
window.addEventListener('load', () => {
    navigator.serviceWorker.register('/sw.js', { scope: '/' })
        .catch((err) => console.warn('SW registration failed:', err));
});
```

Two holes follow.

**Noticing.** The browser checks for a new `sw.js` on a navigation in scope.
`register()` is one, but it runs on `load` -- and a standalone web app
relaunched from the app switcher is *resumed*, not navigated. On the platform
where this matters the check may never run at all, which is why "close it and
open it again" cannot help.

**Applying.** `sw.js` ends `install` with `skipWaiting()` and `activate` with
`clients.claim()`, so a new worker does take over. But nothing re-renders, so
the reader keeps looking at the assets the *old* worker served. The update
becomes visible only on some later cold launch, which iOS may not give them.

## Force update was a placebo on iOS

`forceUpdatePWA()` unregisters every worker and deletes every Cache Storage
entry correctly -- and then calls `window.location.reload()`. In an iOS
standalone web app that reload can be answered from WebKit's own HTTP/page
cache, which neither of those two steps touches. The old HTML comes back, it
names the same hashed assets, and the command appears to do nothing. It showed
`Updating...` and reloaded, so it looked like it had worked.

Its test asserts the reload *hook fired* (`window.__turiReload`), never that a
fetch happened, so the gap was invisible on desktop, where it is not a gap.

## Evidence the client was stale

From a screenshot of the installed app on an iPhone 16 Pro (402x874 CSS,
`safe-area-inset-top` 62):

```
app chrome (--bg-panel 22,21,19) ends at   CSS y = 811
--bg-base (11,10,8) from 812 to bottom      = 63 px band
874 - 62 = 812
```

Under the current build `#app` is `position: fixed; inset: 0` and cannot end at
811, and `.console-footer` is 58px of `--bg-panel` pinned to the bottom. So the
device was running the pre-fix build -- and, incidentally, this confirms to the
pixel the `100dvh` behaviour that
[pwa-overlays-ignore-ios-safe-area](pwa-overlays-ignore-ios-safe-area.md)
could only infer: in an iOS standalone web app `100dvh` resolves to the screen
height *minus the top inset*.

## Fixed here

Only the recovery lever, because that is the half that can reach a client which
is already stuck. `web/public/sw-kill.js` -- previously written as insurance,
referenced by nothing, and never executed once -- now works and is tested:

- Its `activate` did async work in an `async` listener with **no
  `event.waitUntil()`**, so the browser was free to terminate the worker at the
  first `await`, before a single cache was deleted.
- It claimed clients without awaiting, and did nothing about already-open pages.
- It reloaded open windows unconditionally, which while deployed at `/sw.js`
  is an **infinite reload loop**: the reloaded page registers `/sw.js`, which is
  the kill-switch, which activates and reloads again. Caught by a test before
  it ever shipped; guarded now by a `swkill` URL marker.

`swKillSwitch()` in `web/vite.config.js` ships it at `/sw.js` under
`TUR_SW_KILL=1`, and throws rather than warns if it finds nothing to replace.
Procedure: [docs/guides/pwa-recovery-runbook.md](../guides/pwa-recovery-runbook.md).

`tests/sw-kill.spec.js` drives a real worker and real Cache Storage -- no stubs
-- and asserts the wipe, the unregister, the single reload, and the absence of a
loop.

## Resolved: the update lifecycle (c9afe0aac, PR #904)

All four items landed. The parked `claude/pwa-update-on-resume` sketch was not
what shipped -- the branch was rewritten and verified before merge.

1. **Noticing.** `reg.update()` on every foreground -- `visibilitychange` plus
   a persisted `pageshow`, since a standalone app relaunched from the app
   switcher is RESUMED, not navigated, so `register()` on `load` never ran on
   the one platform where it mattered.
2. **Applying.** A `controllerchange` handler re-navigates once, guarded on the
   page having been controlled at load, so a first visit's `clients.claim()`
   does not flash. Deferred to the next foreground rather than done on the
   spot, because an edit reaches `localStorage` through two chained 250ms
   debounces.
3. **Force update** navigates to a cache-busting URL instead of
   `location.reload()` -- WebKit can answer a reload out of its own HTTP/page
   cache, which neither the unregister nor the Cache Storage wipe touches,
   which is exactly why it was a placebo on iOS. `applySwUpdate` navigates for
   the same reason.
4. **Build stamp** in the overflow menu, read from the live service-worker
   cache key (version + commit) rather than a compiled-in constant -- a
   constant reports the build the page WANTS to be, even while a stale worker
   serves everything around it.

`tests/pwa-update.spec.js` is red against the pre-change files and green after,
checked by reverting `main.js` / `try/index.html`. Two test defects had to be
fixed for that to mean anything: the suite's first run passed 2 of 4 for the
wrong reason (service workers are disabled on loopback hosts without `?sw=1`,
so the branch under test was dead and the negatives were green about nothing --
every negative is now preceded by a positive that proves the wiring is live),
and the Force-update smoke case waited on the WASM boot, making it unrunnable
in any fresh worktree, which is precisely where its assertion had changed and
gone unverified.

This ships IN a build, so it cannot reach a client already stuck -- that is
what the kill-switch above is for.

## Not to be confused with: the black band (resolved separately)

A reader hitting this report because an installed app "looks like an old
build" should check the band first, because it is NOT evidence of staleness
and it cost a full round of cache archaeology on 2026-09-18.

Symptom: `TUR_SW_KILL=1 npm run deploy`, force-quit, "Clear History and
Website Data", delete and re-add the icon -- and the app still renders the
old layout. That reads as an unkillable cache. It was not one. Checked from
the outside, at that moment:

- `/sw.js` was serving the kill-switch (so there was no caching worker at all)
- the deployed bundle matched HEAD
- apex and `www` were byte-identical (same ETag)
- `/sw.js` is and has always been the only registration URL

Every cache layer was already gone; the bytes on the wire were current. What
the reader was seeing was the CURRENT build, rendering wrong: the pinned-shell
rule was gated on `(display-mode: standalone)` while iOS reports `fullscreen`
for a black-translucent home-screen app, so it never applied. Fixed by gating
it on `html.pwa` instead -- see the commit for
`web/styles.css` / `web/try/index.html`.

The tell, if it happens again: measure the band. `height: 100dvh` lands short
by top+bottom inset (~93 CSS px on a notched iPhone). A stale build is a
plausible story for anything; a band of exactly that height is a layout bug.

This says nothing about whether the update path in this report works -- the
kill-switch was deployed at the time, so that run could not have exercised it.
