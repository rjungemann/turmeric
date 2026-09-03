# Try Turmeric on mobile/WebKit shows "Failed to load WASM" after a reload

**Severity: medium if it reproduces on a real iOS Safari** (a user who reloads
Try Turmeric on a phone gets a dead REPL); unattributed until then. Split out
of [try-turmeric-browser-suites-green-while-failing](../archive/try-turmeric-browser-suites-green-while-failing.md)
on 2026-09-02, which found it in a CI log and could not triage it.

## Symptom

Two tests in `web/tests/mobile.split-and-pwa.spec.js` (`split fraction
persists across reload`, `editor buffer persists across reload`) fail in the
`Try Turmeric smoke test (browser)` job's mobile project (`devices['iPhone
13']`, WebKit):

```
Locator:  locator('#wasm-status-text')
Expected: "Ready"
Received: "Failed to load WASM"
    4 x  "Initializing WASM..."
    1 x  "Loading WASM module..."
    58 x "Failed to load WASM"
```

Both do `await page.reload()` and then wait for `Ready`. The initial load
works (30 of 32 mobile tests pass), and the status walks Initializing ->
Loading -> **Failed** only on the reload path.

## Not the same as the resolved WebKit-reload report

`webkit-sw-controlled-reload-fails-wasm-init` (docs/reported) describes a
service-worker-controlled reload failing on WebKit; check whether this is
that report's shape before treating it as new -- the desktop `sw-dev` /
`docs-offline` specs pass, so it is not a general SW fault, but the mobile
project may be the one place a controlled reload is exercised on WebKit.

## How to triage now

The browser job always uploads `playwright-report` and the `web_mobile` row
on `/ci` carries the failure count, so the `error-context.md` for each
failure is one artifact download away. Candidates to separate: the
service-worker cache on the reload; a WebKit-specific WASM fetch /
instantiate path (`web/main.js` WASM init); a CI-only artifact of the
Playwright WebKit build. Reproduce against a real iOS Safari before deciding
severity.
