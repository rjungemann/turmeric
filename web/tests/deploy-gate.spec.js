// Minimal pre-deploy gate for Try Turmeric.
//
// One test: page loads, WASM initializes, `(+ 1 2)` evaluates to `3`.
// Runs in CI on every push/PR to main so a broken preload chain (or any
// regression that stalls `_turi_wasm_init`) blocks the branch before the
// manual `npm run deploy`.
//
// Kept deliberately separate from the broader tests/smoke.spec.js suite so
// this file stays the one-thing-you-must-not-break contract, and doesn't get
// mixed in with UI-detail tests that may flake.

import { test, expect } from '@playwright/test';

test('Try Turmeric loads and evaluates a basic expression', async ({ page }) => {
    const jsErrors = [];
    page.on('pageerror', err => jsErrors.push(String(err)));
    page.on('console', msg => {
        if (msg.type() === 'error') jsErrors.push(msg.text());
    });

    await page.goto('/try/');

    // WASM init: the loading overlay stays visible until the eval Worker
    // posts 'ready' and main.js sets wasmState = READY.  The status text
    // is the load-bearing signal; overlay display is secondary.
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', {
        timeout: 30_000,
    });

    // Basic evaluation: type into the Monaco editor via the global handle
    // main.js exposes as window._turiEditor, then click Run.
    await page.evaluate(() => window._turiEditor.setValue('(+ 1 2)'));
    await page.click('#run-btn');

    // Assert the console pane surfaces `3` and no evaluator error span
    // (evaluator errors render as `<span class="console-error">#<error ...`).
    await expect(page.locator('#console')).toContainText('3', {
        timeout: 10_000,
    });
    const errorSpans = await page.locator('#console .console-error').allTextContents();
    const evalErrors = errorSpans.filter(t => t.startsWith('#<error'));
    expect(evalErrors, `unexpected evaluator errors: ${JSON.stringify(evalErrors)}`).toHaveLength(0);

    // No hard JS exceptions during the run.
    const fatalJs = jsErrors.filter(e =>
        e.includes('TypeError') || e.includes('RuntimeError') || e.includes('Failed to')
    );
    expect(fatalJs, `unexpected page JS errors: ${JSON.stringify(fatalJs)}`).toHaveLength(0);
});
