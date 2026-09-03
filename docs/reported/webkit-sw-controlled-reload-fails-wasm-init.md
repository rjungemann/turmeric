# Try Turmeric fails to boot on a WebKit reload once the service worker controls the page

**Severity:** medium, and **high if it reproduces on real iOS Safari** --
confirming that is step 1 below. The break is on the *second* load, so it hits
returning visitors rather than first-time ones, and the message it shows them
("Please refresh the page") is advice that cannot work: the service worker
still controls the reload.

**Status:** OPEN. Observed in CI, not yet reproduced by hand, and the root
cause is not established -- only narrowed. Do not treat the mechanism section
below as a diagnosis.

Invisible until 2026-08-28. The `mobile` Playwright project is
`devices['iPhone 13']`, which is WebKit, but CI installed only Chromium, so all
32 mobile tests died at `browserType.launch: Executable doesn't exist at
.../webkit-*/pw_run.sh` before any test body ran -- and the step's
`continue-on-error` hid it. Installing WebKit
(try-turmeric-navigation-and-minimap-plan, section 10.5) turned 32 launch
errors into 30 passes and these 2 real failures.

## Repro

```
npx playwright test --project=mobile tests/mobile.split-and-pwa.spec.js
```

Both failures are in `web/tests/mobile.split-and-pwa.spec.js`, and both have
the same shape -- first load fine, reload broken:

| Line | Test |
| --- | --- |
| 25 | split fraction persists across reload |
| 44 | editor buffer persists across reload |

```
Locator:  locator('#wasm-status-text')
Expected: "Ready"
Received: "Failed to load WASM"

  3 x locator resolved to <span id="wasm-status-text">Initializing WASM...</span>
  1 x locator resolved to <span id="wasm-status-text">Loading WASM module...</span>
 59 x locator resolved to <span id="wasm-status-text">Failed to load WASM</span>

  > 53 |  await expect(page.locator('#wasm-status-text')).toHaveText('Ready', ...)
```

Both reach `Ready` on the `goto` at the top of the test (`gotoTry`,
`mobile.split-and-pwa.spec.js:3`) and only fail after `page.reload()`.

Observed on CI run
[33151156197](https://github.com/rjungemann/turmeric/actions/runs/33151156197),
job `Try Turmeric smoke test (browser)`, head `9a6ee5c7`.

## What is established

1. **Not the SharedArrayBuffer path.** "Failed to load WASM" is the `else`
   branch at `web/main.js:1178`. The `shared-array-buffer-unavailable` branch
   two lines up (`web/main.js:1168`) prints a different message and did not
   fire, so cross-origin isolation survived the reload and `SharedArrayBuffer`
   was present. This rules out the obvious first guess.

2. **The worker got as far as trying.** The status went
   `Initializing WASM...` -> `Loading WASM module...` -> `Failed to load WASM`,
   so `eval-worker.js` was created and ran. By elimination the `init-error` it
   posted was one of `web/public/eval-worker.js:167`
   (`TurmericModule not found after importScripts`), `:175`
   (`turi_wasm_init failed: <n>`), or `:182` (`String(err)` from the module
   factory). Which one is unknown -- see step 1 below.

3. **The service worker itself is fine.** `service worker registers and
   precaches WASM` (`mobile.split-and-pwa.spec.js:59`) **passed** on the same
   run: `navigator.serviceWorker.ready` resolved with an active worker, and
   `/turmeric.wasm` was found in a cache.

4. **Chromium is unaffected.** The desktop suite's 87 tests include
   reload-based ones (`minimap.spec.js` "the toggle turns it off, and the
   choice survives a reload") and pass.

5. **First load vs reload is exactly the uncontrolled/controlled boundary.**
   `web/public/sw.js:82-88` states it: a brand-new worker does not control the
   page that installed it, because that page's asset requests were already in
   flight before `clients.claim()` ran. So on the first load `/turmeric.js`
   and `/turmeric.wasm` come from the network, and on the reload they come from
   `cacheFirst` (`web/public/sw.js:344`) out of the Cache API.

That last point is the whole finding: **the failure appears exactly when the
wasm assets are replayed from the Cache API instead of fetched.**

## Candidate mechanisms -- none confirmed

- **Content-Type lost on the Cache API round-trip.**
  `WebAssembly.instantiateStreaming` rejects anything not served as
  `application/wasm`. Emscripten normally falls back to `arrayBuffer()`, so
  this alone should not be fatal -- unless the fallback is also failing.
- **A COEP/CORP judgement on a worker-provided response.** The document is
  `Cross-Origin-Embedder-Policy: require-corp` (`web/public/_headers`). A
  same-origin response needs no CORP, but a service-worker-replayed response
  may be treated differently by WebKit than by Blink.
- **A truncated body.** Both `cacheOne` (`sw.js:68`) and `cacheFirst`
  (`sw.js:344`) do `cache.put(..., res.clone())` while returning or discarding
  the original. `turmeric.wasm` is several MB; a clone-then-put that stores a
  short body would produce exactly this -- a module that fetches but will not
  instantiate. Testable: compare `(await caches.match('/turmeric.wasm')).blob()`
  size against `Content-Length`.

## Fix directions

1. **Get the actual error first.** The CI logs carry no browser console. Add a
   `page.on('console')` / `page.on('pageerror')` dump to the two failing tests,
   or read `#console` (the page prints
   `Error: Failed to load WASM module` there, and `main.js:1164` has already
   `console.error`'d the real one). Everything below is guesswork until this
   is done, and it is ten minutes of work.
2. **Confirm on real Safari.** Playwright's WebKit is not Safari -- it is
   WebKit built for Linux, and service-worker plus COEP behaviour is exactly
   the area where the two can differ. If real iOS Safari reproduces it, this
   is high severity and a shipping bug for every returning mobile visitor. If
   it does not, it is a test-environment issue and should be recorded as one.
3. **Check the cached body length** per the third candidate above.
4. If it is the Cache API replay, the narrow fix is to exclude
   `/turmeric.wasm` and `/turmeric.js` from `cacheFirst` on WebKit, or to
   revalidate their length before serving. Do not reach for disabling the
   service worker: offline support is the feature it exists for
   (`docs/archive/` offline-docs work), and the failure is on one engine.

## What this is not

Not caused by the navigation/minimap work; that change installed WebKit in CI
and so revealed it, but these two tests were failing (as launch errors) before
it.

Nor is it a regression, as far as the history goes. `web/public/sw.js` and
`web/tests/mobile.split-and-pwa.spec.js` arrived in the same merge (#759), and
`playwright install` in `.github/workflows/ci.yml` named only `chromium` from
then until `9a6ee5c7`. So these two tests have never once executed, on any
head, and there is no green run of them to bisect back to. Whatever this is,
it has been true for the whole life of the service worker.

## Seen again 2026-09-01 / triage is cheap now (2026-09-02)

The same two failures were re-observed in the browser job on run
33460242737 while resolving
[try-turmeric-browser-suites-green-while-failing](../archive/try-turmeric-browser-suites-green-while-failing.md)
(a duplicate report filed from that log was folded into this one). What
changed on the CI side makes step 1 cheaper: the browser job now always
uploads the `playwright-report` artifact (the `error-context.md` for each
failure is in it), and the `web_mobile` row on `/ci` carries the failure
count, so a regression or a fix shows as a trend rather than a log dig. The
desktop `sw-dev` / `docs-offline` specs pass, so it is not a general
service-worker fault; the mobile project may simply be the one place a
controlled reload is exercised on WebKit.
