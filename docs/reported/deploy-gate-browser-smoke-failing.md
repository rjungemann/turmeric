# `Try Turmeric` deploy-gate browser smoke test failing in CI

**Severity:** medium (CI-only; blocks the `Try Turmeric smoke test (browser)`
job on every push/PR). Root cause **not yet pinned** -- needs the uploaded
Playwright report artifact to diagnose. Pre-existing on `main`, independent of
any one PR.

**One-line:** the single deploy-gate Playwright test
(`web/tests/deploy-gate.spec.js:14` -- "Try Turmeric loads and evaluates a basic
expression") fails, so the browser smoke job is red. All build steps before it
(native `tur`, gendocs, WASM configure+build, web deps, Playwright install)
succeed; only the test assertion fails.

## Repro

CI: run #2015 (PR) and run #1999 (`main`), job `Try Turmeric smoke test
(browser)` -> step "Run deploy-gate smoke test":

```
1 failed
  [desktop] > tests/deploy-gate.spec.js:14:1 > Try Turmeric loads and evaluates a basic expression
##[error]Process completed with exit code 1.
```

A `playwright-report` artifact is uploaded on failure (e.g. run #2015 artifact
ID 8481985491) -- that HTML report has the actual assertion + trace and is the
fastest path to root cause.

Local repro (not yet run): build WASM + serve `web/`, then
`cd web && npx playwright test tests/deploy-gate.spec.js`.

## What the test asserts (surfaces to check)

From `web/tests/deploy-gate.spec.js`:

1. `page.goto('/try/')` then wait up to 30s for `#wasm-status-text` to read
   `Ready` -- i.e. the eval Worker posts `ready` and `main.js` sets
   `wasmState = READY`. A stalled `_turi_wasm_init` / broken preload chain fails
   here.
2. Set the Monaco editor to `(+ 1 2)` via `window._turiEditor`, click
   `#run-btn`, expect `#console` to contain `3` within 10s and no
   `.console-error` span.

So the failure is one of: (a) WASM never reaches `Ready` (init/preload
regression), (b) eval produces the wrong text or an evaluator-error span, or
(c) an uncaught JS `pageerror`/console error (the spec also collects those).
The report artifact will say which.

## Fix direction

Pull the Playwright report artifact from a failed run, identify which assertion
tripped, and trace back:
- If `#wasm-status-text` never hits `Ready`: inspect the WASM preload/worker
  init chain in `web/` (`main.js`, the eval worker, `web/turmeric.js`).
- If eval mismatch / error span: check the WASM `turi` eval path for `(+ 1 2)`.
- If a JS pageerror: read the collected `jsErrors` in the report.

Note the WASM build embeds the same DK/CPS runtime prelude as native builds, so
confirm whether this predates recent CPS codegen changes by checking older
`main` runs (it is already red on run #1999, so it predates the
`cps-reopen-perform-onode-leak` PR).

Discovered while triaging CI on the `cps-reopen-perform-onode-leak` PR.
