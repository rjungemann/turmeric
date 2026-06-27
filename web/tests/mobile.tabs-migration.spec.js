import { test, expect } from '@playwright/test';

test.describe('Multi-tab migration from legacy single-buffer keys', () => {
    test('synthesizes main.tur from legacy keys and deletes the legacy entries', async ({ page }) => {
        // Inject the legacy keys before /try/ runs any of its own JS, so the
        // migration branch in hydrateTabs() picks them up on the very first
        // load. addInitScript runs in every navigation including reload.
        await page.addInitScript(() => {
            localStorage.setItem('tur.try.buffer.v1', JSON.stringify('(println "legacy-seed")'));
            localStorage.setItem('tur.try.cursor.v1', JSON.stringify({ lineNumber: 1, column: 7 }));
            localStorage.setItem('tur.try.scroll.v1', JSON.stringify(0));
        });

        await page.goto('/try/');
        await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
        await page.waitForFunction(() => !!window._turiTabs);

        const snap = await page.evaluate(() => window._turiTabs.tabs());
        expect(snap).toHaveLength(1);
        expect(snap[0].name).toBe('main.tur');
        expect(snap[0].content).toBe('(println "legacy-seed")');
        expect(snap[0].cursor).toEqual({ lineNumber: 1, column: 7 });

        // New keys are written.
        const stored = await page.evaluate(() => ({
            tabs:   localStorage.getItem('tur.try.tabs.v1'),
            active: localStorage.getItem('tur.try.activeTab.v1'),
        }));
        expect(stored.tabs).not.toBeNull();
        expect(stored.active).not.toBeNull();

        // Legacy keys are gone (the migration deletes them only after the
        // new keys are written, so a crash mid-migration leaves them intact).
        const legacy = await page.evaluate(() => ({
            buffer: localStorage.getItem('tur.try.buffer.v1'),
            cursor: localStorage.getItem('tur.try.cursor.v1'),
            scroll: localStorage.getItem('tur.try.scroll.v1'),
        }));
        expect(legacy.buffer).toBeNull();
        expect(legacy.cursor).toBeNull();
        expect(legacy.scroll).toBeNull();
    });

    test('migration is idempotent: a second reload does not re-seed from legacy', async ({ page }) => {
        // Pre-seed both the legacy buffer AND a pre-existing multi-tab set.
        // The hydration path should pick up tabs.v1 and ignore the legacy
        // key entirely.
        await page.addInitScript(() => {
            localStorage.setItem('tur.try.buffer.v1', JSON.stringify('(println "legacy")'));
            const id = 'tab-preexisting';
            localStorage.setItem('tur.try.tabs.v1', JSON.stringify([{
                id, name: 'main.tur', content: '(println "edited")',
                cursor: { lineNumber: 1, column: 1 }, scrollTop: 0, createdAt: 1,
            }]));
            localStorage.setItem('tur.try.activeTab.v1', JSON.stringify(id));
        });

        await page.goto('/try/');
        await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
        await page.waitForFunction(() => !!window._turiTabs);

        const snap = await page.evaluate(() => window._turiTabs.tabs());
        expect(snap).toHaveLength(1);
        expect(snap[0].content).toBe('(println "edited")');
        // The legacy key survives untouched -- the migration branch never ran.
        const legacyBuffer = await page.evaluate(() =>
            localStorage.getItem('tur.try.buffer.v1'));
        expect(legacyBuffer).toBe('"(println \\"legacy\\")"');
    });
});
