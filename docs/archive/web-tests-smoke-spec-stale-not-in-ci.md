# `web/tests/smoke.spec.js` is stale and not wired into CI

**Severity:** medium -- 15+ Playwright tests exist that are supposed to cover
Try Turmeric behavior (basic eval, REPL history, examples-dropdown scenarios
like factorial / fibonacci / effects / pattern matching), but they are never
run automatically and at least the entrypoint navigations look stale after
the site was split into `/` (home) and `/try/` (playground). The new
`deploy-gate.spec.js` covers only the one-thing-you-must-not-break contract
(load + `(+ 1 2)`); this report is about restoring the broader coverage.

## Symptoms

1. `.github/workflows/ci.yml` runs `deploy-gate.spec.js` only (the `web-smoke`
   job added alongside this report). Nothing runs `smoke.spec.js`,
   `guide-toggle.spec.js`, `meta-commands.spec.js`, `diag.spec.js`, or any of
   the `mobile.*.spec.js` files. A regression in any of those surfaces has
   no CI signal.
2. `smoke.spec.js` calls `await page.goto('/')` (lines 44, 51, 60, 68, 86,
   and inside the `Examples` describe's `beforeEach`). The `#wasm-status-text`
   element and `window._turiEditor` global only exist on the `/try/` page --
   `web/try/index.html:151`, `web/main.js:1203`. The home `web/index.html`
   is the marketing page. So every `smoke.spec.js` test currently fails at
   the first `waitForReady(page)` with `element(s) not found`, exactly the
   same way `deploy-gate.spec.js` did on its first draft before I fixed the
   goto (verified locally, 2026-07-08).
3. `web/tests/prod-smoke.spec.js` may have the same issue; not checked.

## Repro

```
cd web
npx playwright test smoke.spec.js --project=desktop --reporter=list
```

Expect: every desktop test fails at `#wasm-status-text` locator timeout.
Actual triage effort needed to confirm.

## Root cause

The suite predates the site's split into a marketing homepage at `/` and the
playground at `/try/` (see `web/vite.config.js` rollup inputs -- `main`,
`try`, `tour`). The specs never got a bulk `s|/|/try/|` sweep, and no CI job
would have flagged the regression.

## Fix directions

1. **Dust off the specs.** Bulk update `page.goto('/')` -> `page.goto('/try/')`
   in `smoke.spec.js` (and any sibling spec that expects the playground DOM).
   Verify each test locally with `npx playwright test <spec> --project=desktop`.
   Delete cases that reference removed features rather than "fixing" them.
2. **Right-size the boundary between `deploy-gate.spec.js` and `smoke.spec.js`.**
   The gate stays as the tiny load + one-eval contract. `smoke.spec.js` becomes
   the "regression net" -- REPL history, examples-dropdown eval, meta commands,
   guide toggle, diag. It runs on every PR but a single flake is a signal
   to fix, not a merge blocker, so mark it `continue-on-error: true` in CI.
3. **Wire into `.github/workflows/ci.yml`.** Add either:
   - a second Playwright job (`web-full`) that reuses the `web-smoke` job's
     wasm build via `needs: web-smoke` + `actions/upload-artifact` for the
     `web/public/turmeric.{js,wasm}` pair, then runs `npx playwright test`
     with no filter; or
   - one job that runs `deploy-gate.spec.js` first (blocking) then the rest
     (non-blocking `continue-on-error`). Simpler; slower wall clock.
   Pick whichever is easier to keep green.
4. **Mobile suite.** The `mobile.*.spec.js` files use a separate
   `playwright.config.js` project (`mobile`, iPhone 13). They should run in
   the same job matrix; audit them for the same `/` vs `/try/` drift.
5. **Prod smoke.** `prod-smoke.spec.js` runs against `https://turmeric-lang.com`
   via `playwright.config.prod.js`. Consider a scheduled CI job (e.g. cron
   every 6 h on `main`) so a prod regression pages someone.

## Impact

Right now the only automated browser-side check that exists (once this PR
lands) is `deploy-gate.spec.js`. Everything else in `web/tests/` is dead
code from an observability standpoint. The v1 push justifies the small
cleanup: the tests were already written, someone just needs to spend an
hour reviving them and adding one CI job. Not a blocker for v1, but a cheap
win.
