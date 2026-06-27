import { test, expect } from '@playwright/test';

// Each Playwright test gets a fresh browser context, so localStorage starts
// empty -- no explicit clear() needed. Use a single navigation so reload()
// later in the test isn't fighting the service worker.
async function gotoTry(page) {
    await page.goto('/try/');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
    await page.waitForFunction(() => !!window._turiTabs);
}

// Active-tab label without the inline close-× glyph.
const activeLabel = (page) =>
    page.locator('.editor-tabs .tab-button.active .tab-label');

test.describe('Multi-tab editor', () => {
    test('fresh workspace seeds a single main.tur tab', async ({ page }) => {
        await gotoTry(page);
        const snapshot = await page.evaluate(() => window._turiTabs.tabs());
        expect(snapshot).toHaveLength(1);
        expect(snapshot[0].name).toBe('main.tur');
        const activeId = await page.evaluate(() => window._turiTabs.activeId());
        expect(activeId).toBe(snapshot[0].id);
        const strip = page.locator('.editor-tabs');
        await expect(strip.locator('.tab-button')).toHaveCount(2);  // seed + "+ New"
        await expect(activeLabel(page)).toHaveText('main.tur');
    });

    test('creating a new tab gives untitled-1.tur and switches to it', async ({ page }) => {
        await gotoTry(page);
        await page.evaluate(() => window._turiTabs.create());
        const snapshot = await page.evaluate(() => window._turiTabs.tabs());
        expect(snapshot.map(t => t.name)).toEqual(['main.tur', 'untitled-1.tur']);
        const activeId = await page.evaluate(() => window._turiTabs.activeId());
        expect(activeId).toBe(snapshot[1].id);
        await expect(activeLabel(page)).toHaveText('untitled-1.tur');
    });

    test('switching tabs preserves per-tab content', async ({ page }) => {
        await gotoTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "alpha")'));
        const tab1 = await page.evaluate(() => window._turiTabs.activeId());

        const tab2 = await page.evaluate(() => window._turiTabs.create().id);
        await page.evaluate(() => window._turiEditor.setValue('(println "beta")'));

        await page.evaluate((id) => window._turiTabs.switch(id), tab1);
        expect(await page.evaluate(() => window._turiEditor.getValue()))
            .toBe('(println "alpha")');

        await page.evaluate((id) => window._turiTabs.switch(id), tab2);
        expect(await page.evaluate(() => window._turiEditor.getValue()))
            .toBe('(println "beta")');
    });

    test('closing a tab disallows the last one and falls back to neighbor on active close', async ({ page }) => {
        await gotoTry(page);
        const seedId = await page.evaluate(() => window._turiTabs.activeId());
        await page.evaluate((id) => window._turiTabs.close(id), seedId);
        expect(await page.evaluate(() => window._turiTabs.tabs())).toHaveLength(1);

        const second = await page.evaluate(() => window._turiTabs.create().id);
        const third  = await page.evaluate(() => window._turiTabs.create().id);
        await page.evaluate((id) => window._turiTabs.switch(id), second);
        await page.evaluate((id) => window._turiTabs.close(id), second);

        const after = await page.evaluate(() => ({
            tabs: window._turiTabs.tabs().map(t => t.id),
            active: window._turiTabs.activeId(),
        }));
        expect(after.tabs).toEqual([seedId, third]);
        expect(after.active).toBe(third);
    });

    test('renaming a tab appends .tur and de-duplicates collisions', async ({ page }) => {
        await gotoTry(page);
        const seedId = await page.evaluate(() => window._turiTabs.activeId());
        const newId  = await page.evaluate(() => window._turiTabs.create().id);

        await page.evaluate((id) => window._turiTabs.rename(id, 'util'), newId);
        let snap = await page.evaluate(() => window._turiTabs.tabs());
        expect(snap.find(t => t.id === newId).name).toBe('util.tur');

        await page.evaluate((id) => window._turiTabs.rename(id, 'main.tur'), newId);
        snap = await page.evaluate(() => window._turiTabs.tabs());
        expect(snap.find(t => t.id === newId).name).toBe('main-2.tur');
        expect(snap.find(t => t.id === seedId).name).toBe('main.tur');
    });

    test('reorder API moves a tab in the persisted order', async ({ page }) => {
        await gotoTry(page);
        await page.evaluate(() => window._turiTabs.create({ name: 'b.tur' }));
        await page.evaluate(() => window._turiTabs.create({ name: 'c.tur' }));
        const ids = await page.evaluate(() => window._turiTabs.tabs().map(t => t.id));
        await page.evaluate((id) => window._turiTabs.reorder(id, 0), ids[2]);
        const names = await page.evaluate(() => window._turiTabs.tabs().map(t => t.name));
        expect(names).toEqual(['c.tur', 'main.tur', 'b.tur']);
    });

    test('tab set, active tab, and per-tab cursor are persisted to localStorage', async ({ page }) => {
        // We assert the persistence WRITE here rather than round-tripping
        // through page.reload(); the dev-server + service-worker pairing
        // currently flakes on reload under mobile webkit (the existing
        // mobile.split-and-pwa reload tests hit the same wall). Hydration
        // is independently covered by mobile.tabs-migration.spec.js, which
        // seeds tur.try.tabs.v1 via addInitScript and asserts the loaded
        // state matches.
        await gotoTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "seed")'));
        const newId = await page.evaluate(() =>
            window._turiTabs.create({ name: 'util.tur' }).id);
        await page.evaluate(() => window._turiEditor.setValue('(defn add [a b] (+ a b))'));
        await page.evaluate(() =>
            window._turiEditor.setPosition({ lineNumber: 1, column: 12 }));
        // Past the 500ms cursor-change debounce -> persistTabs() then needs
        // another 250ms tab-write debounce, so wait > 750ms with margin.
        await page.waitForTimeout(1200);

        const stored = await page.evaluate(() => ({
            tabs:   JSON.parse(localStorage.getItem('tur.try.tabs.v1')),
            active: JSON.parse(localStorage.getItem('tur.try.activeTab.v1')),
        }));
        expect(stored.tabs.map(t => t.name).sort()).toEqual(['main.tur', 'util.tur']);
        expect(stored.active).toBe(newId);
        const util = stored.tabs.find(t => t.name === 'util.tur');
        expect(util.content).toBe('(defn add [a b] (+ a b))');
        expect(util.cursor.column).toBe(12);
    });
});
