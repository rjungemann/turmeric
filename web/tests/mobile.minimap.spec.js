import { test, expect } from '@playwright/test';

/**
 * The minimap on a phone (try-turmeric-navigation-and-minimap-plan, M1).
 *
 * At 390px the editor is the whole screen, and a minimap takes a fifth of the
 * code column to show three characters of shape. The width gate is what keeps
 * it off, and it is the kind of default that is easy to lose: it lives in
 * measured-width logic rather than in a media query, so no stylesheet failure
 * would reveal it.
 *
 * The overview ruler is deliberately *not* gated, and that is asserted here
 * too -- it is a few pixels wide at any size, and a diagnostic you can find by
 * looking is worth those pixels on a phone.
 */

test.describe('Minimap on mobile', () => {
    test.beforeEach(async ({ page }) => {
        await page.goto('/try/');
        await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
    });

    test('is off at 390px', async ({ page }) => {
        await expect.poll(() => page.evaluate(() => window._turiMinimap.enabled()),
                          { timeout: 10_000 }).toBe(false);
        // Off because nobody chose -- the gate, not a remembered preference.
        expect(await page.evaluate(() => window._turiMinimap.preference())).toBe(null);
    });

    test('the overview ruler is not gated with it', async ({ page }) => {
        const lanes = await page.evaluate(
            () => window._turiEditor.getRawOptions().overviewRulerLanes);
        expect(lanes).toBeGreaterThan(0);
    });

    test('a phone user who wants it can still have it', async ({ page }) => {
        await page.evaluate(() => window._turiMinimap.toggle());
        expect(await page.evaluate(() => window._turiMinimap.enabled())).toBe(true);
    });
});
