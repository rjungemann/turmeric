import { test, expect } from '@playwright/test';

/**
 * Minimap and overview ruler (try-turmeric-navigation-and-minimap-plan, M1).
 *
 * Three separate claims, and they fail in different ways:
 *
 *   - The strip renders at all. Monaco's minimap is a supported component, so
 *     the risk is not that it breaks; it is that our width gate or our toggle
 *     turns it off and nobody notices, because "no minimap" is exactly what
 *     the page looked like before this shipped.
 *   - Diagnostics reach the overview ruler. `overviewRulerLanes: 0` is what
 *     suppressed them, and it is one number away from being suppressed again.
 *   - The toggle beats the gate, and survives a reload. An explicit choice
 *     that the width gate silently overrides on the next layout pass is worse
 *     than no toggle.
 */

async function waitForReady(page) {
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

/** Editor-level truth: what the editor is doing, not what storage remembers. */
async function minimapOn(page) {
    return page.evaluate(() => window._turiMinimap.enabled());
}

test.describe('Minimap', () => {
    test.beforeEach(async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
    });

    test('renders on a desktop-width editor', async ({ page }) => {
        await expect.poll(() => minimapOn(page), { timeout: 10_000 }).toBe(true);
        // The gate decides; the DOM is what the user sees. Assert both, since
        // an option set to true with no canvas behind it is still no minimap.
        // Scoped to the main editor: the REPL prompt is a second Monaco
        // instance with its own minimap node, and an unscoped selector matches
        // both (strict-mode violation).
        await expect(page.locator('#editor .monaco-editor .minimap')).toBeVisible();
    });

    test('the toggle turns it off, and the choice survives a reload',
         async ({ page }) => {
        await expect.poll(() => minimapOn(page), { timeout: 10_000 }).toBe(true);

        await page.evaluate(() => window._turiMinimap.toggle());
        expect(await minimapOn(page)).toBe(false);
        expect(await page.evaluate(() => window._turiMinimap.preference())).toBe(false);

        await page.reload();
        await waitForReady(page);
        // Still off, and still off for the stated reason -- an explicit user
        // choice, not a width the editor happens to have on reload.
        await expect.poll(() => minimapOn(page), { timeout: 10_000 }).toBe(false);
        expect(await page.evaluate(() => window._turiMinimap.preference())).toBe(false);
    });

    test('a narrow editor turns it off, until the user says otherwise',
         async ({ page }) => {
        await expect.poll(() => minimapOn(page), { timeout: 10_000 }).toBe(true);

        // Drive the split rather than the viewport: the case the width gate
        // exists for is a desktop window whose *editor pane* is narrow, which
        // no media query can see.
        await page.evaluate(() => {
            const c = document.querySelector('.repl-container');
            c.style.setProperty('--split-h', '0.15');
            window._turiEditor.layout();
        });
        await expect.poll(() => minimapOn(page), { timeout: 10_000 }).toBe(false);

        // An explicit choice outranks the gate: the strip comes back and
        // stays back at the same width.
        await page.evaluate(() => window._turiMinimap.toggle());
        expect(await minimapOn(page)).toBe(true);
        await page.evaluate(() => window._turiEditor.layout());
        await page.waitForTimeout(300);
        expect(await minimapOn(page)).toBe(true);
    });

    test('the overview ruler has lanes to paint diagnostics into', async ({ page }) => {
        // `overviewRulerLanes: 0` is what suppressed the diagnostic ticks, and
        // it is one number away from suppressing them again. Read the option
        // by name rather than through EditorOption's numbering, so a Monaco
        // bump that renumbers the enum cannot make this assert nothing.
        const lanes = await page.evaluate(
            () => window._turiEditor.getRawOptions().overviewRulerLanes);
        expect(lanes).toBeGreaterThan(0);

        // Unconditional, unlike the minimap: the ruler is a few pixels wide at
        // any pane size, so it is not width-gated.
        await expect(page.locator('#editor .monaco-editor .decorationsOverviewRuler'))
            .toHaveCount(1);
        // (That a *diagnostic* actually lands in it needs a marker, which
        // needs a language server -- asserted in lsp.spec.js against the
        // scripted one.)
    });

    test('nothing about the strip breaks evaluation', async ({ page }) => {
        // The failure contract: anything we add around Monaco's minimap --
        // the gate, the toggle, the theme extension -- must leave the editor
        // working if it throws.
        const jsErrors = [];
        page.on('pageerror', err => jsErrors.push(String(err)));

        await page.evaluate(() => window._turiMinimap.toggle());
        await page.evaluate(() => window._turiMinimap.toggle());
        await page.evaluate(() => window._turiEditor.setValue('(+ 20 22)'));
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('42', { timeout: 15_000 });
        expect(jsErrors).toHaveLength(0);
    });
});
