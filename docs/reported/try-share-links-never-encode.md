# Try Turmeric: Share, and `#code=` links, do nothing -- `pako` is never loaded

**Severity:** medium (a user-facing feature that has no working path). Not a
security finding; found during security-audit-plan WP6.

## Summary

The Share button reports **"Failed to encode code"** every time, the editor's
URL hash is never updated, and a `#code=...` link leaves the editor as it was. All
three go through `encodeState` / `decodeState` (`web/main.js`), which call
`pako.gzip` / `pako.ungzip` -- and nothing defines `pako`. It is not a
dependency in `web/package.json`, not imported, not loaded by a `<script>`.
The comment where it would be set up says so:

```js
// We'll use a lightweight implementation or load pako from CDN
// For now, we'll use a simple base64 encoding without compression
```

-- but no fallback was written. Both functions catch the `ReferenceError` and
return `''`, which every caller reads as "nothing to share".

## Repro

On `/try/`, with the page ready:

```js
typeof window.pako          // 'undefined'
```

Click **Share**: the status reads `Failed to encode code` and nothing reaches
the clipboard. Confirmed in Chromium against a dev server, 2026-09-30.

## Fix directions

- `CompressionStream('gzip')` / `DecompressionStream` are built into every
  browser the playground supports (it already needs `SharedArrayBuffer`), so
  no dependency is needed. Both are async, so `encodeState`/`decodeState` and
  their three callers (`shareCode`, `updateUrlHash`, `loadFromUrlHash`) become
  async.
- Or add `pako` to `web/package.json` and import it; it is bundled, so the
  site's CSP (`web/csp.js`, `script-src 'self'`) is unaffected. A CDN
  `<script>` would need a CSP allowance -- do not add one.
- Keep the decoder tolerant of links minted by any older encoding, and add a
  Playwright round trip (Share, open the copied URL, compare the buffer).

A working share link puts someone else's text into the editor. That is safe
now that the language picker escapes its attributes (WP6, W-2); the round-trip
test should include a first line such as `#lang x"onfocus="...` so it stays
that way.
