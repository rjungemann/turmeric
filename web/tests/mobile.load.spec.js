import { test, expect } from '@playwright/test';

async function gotoTry(page) {
    await page.goto('/try/');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
    await page.waitForFunction(() => !!window._turiTabs);
    // Auto-accept the replace-confirm dialog so loadBytes resolves.
    page.on('dialog', (dialog) => dialog.accept());
}

test.describe('Project load (Phase 3)', () => {
    test('round-trip: capture a workspace, mutate it, then load the capture back', async ({ page }) => {
        // Single page session -- the dev-server + service-worker pairing
        // currently flakes on multiple gotos under mobile webkit (same wall
        // mobile.split-and-pwa reload tests hit). Mutate-then-restore within
        // one page is enough to validate the round-trip.
        await gotoTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "alpha")'));
        const utilId = await page.evaluate(() =>
            window._turiTabs.create({ name: 'util.tur' }).id);
        await page.evaluate(() => window._turiEditor.setValue('(defn square [x] (* x x))'));
        await page.waitForTimeout(700);

        const zipBytes = await page.evaluate(() => window._turiTabs.projectZipBytes());

        // Mutate the workspace so the load has something to restore from.
        await page.evaluate(() => window._turiEditor.setValue(';; mutated'));
        await page.evaluate(() => window._turiTabs.create({ name: 'extra.tur' }));

        const ok = await page.evaluate((bytes) =>
            window._turiTabs.loadBytes(new Uint8Array(bytes)), zipBytes);
        expect(ok).toBe(true);

        const loaded = await page.evaluate(() => window._turiTabs.tabs());
        expect(loaded.map(t => t.name).sort()).toEqual(['main.tur', 'util.tur']);
        const active = await page.evaluate(() => window._turiTabs.activeId());
        expect(active).toBe(utilId);
        const util = loaded.find(t => t.name === 'util.tur');
        expect(util.content).toBe('(defn square [x] (* x x))');
        const main = loaded.find(t => t.name === 'main.tur');
        expect(main.content).toBe('(println "alpha")');

        // Persisted state mirrors the in-memory state.
        const stored = await page.evaluate(() => ({
            tabs:   JSON.parse(localStorage.getItem('tur.try.tabs.v1')),
            active: JSON.parse(localStorage.getItem('tur.try.activeTab.v1')),
        }));
        expect(stored.tabs.map(t => t.name).sort()).toEqual(['main.tur', 'util.tur']);
        expect(stored.active).toBe(utilId);
    });

    test('parseBytes honors workspace.json activeId and tab order', async ({ page }) => {
        await gotoTry(page);
        await page.evaluate(() => window._turiTabs.create({ name: 'b.tur' }));
        await page.evaluate(() => window._turiTabs.create({ name: 'c.tur' }));
        // Activate the middle tab.
        const bId = await page.evaluate(() => window._turiTabs.tabs()[1].id);
        await page.evaluate((id) => window._turiTabs.switch(id), bId);
        const zipBytes = await page.evaluate(() => window._turiTabs.projectZipBytes());

        const parsed = await page.evaluate((bytes) =>
            window._turiTabs.parseBytes(new Uint8Array(bytes)), zipBytes);
        expect(parsed.tabs.map(t => t.name)).toEqual(['main.tur', 'b.tur', 'c.tur']);
        expect(parsed.activeId).toBe(bId);
    });

    test('rejects bytes that are not a ZIP', async ({ page }) => {
        await gotoTry(page);
        const bad = Array.from(new TextEncoder().encode('not a zip file at all'));
        const result = await page.evaluate((b) =>
            window._turiTabs.loadBytes(new Uint8Array(b)).catch(e => ({ error: e.message })),
            bad);
        expect(result).toBe(false);
        // The seed tab survived; nothing was applied.
        const tabsAfter = await page.evaluate(() => window._turiTabs.tabs());
        expect(tabsAfter).toHaveLength(1);
        expect(tabsAfter[0].name).toBe('main.tur');
    });

    test('rejects a ZIP whose src/ is empty', async ({ page }) => {
        await gotoTry(page);
        // Synthesize a "valid" ZIP that contains only build.tur (no src/).
        const emptySrcZip = await page.evaluate(() => {
            // Reach into the loaded module via projectEntries-like flow: build
            // a single-entry ZIP. We piggy-back on buildZip via projectZipBytes
            // on a workspace whose only file is the build.tur stub by first
            // wiping all tabs except main.tur and giving it empty content.
            window._turiEditor.setValue('');
            return window._turiTabs.projectZipBytes();
        });
        const result = await page.evaluate((b) =>
            window._turiTabs.loadBytes(new Uint8Array(b)).catch(e => ({ error: e.message })),
            emptySrcZip);
        expect(result).toBe(false);
    });

    test('rejects oversized ZIP (> 5 MB)', async ({ page }) => {
        await gotoTry(page);
        // 6 MB of zeros -- doesn't matter what's inside; the size guard
        // fires before parsing.
        const huge = new Array(6 * 1024 * 1024).fill(0);
        const result = await page.evaluate((b) =>
            window._turiTabs.loadBytes(new Uint8Array(b)), huge);
        expect(result).toBe(false);
    });

    test('file-input change loads a project end-to-end', async ({ page }) => {
        // Single page session for the same reason as the round-trip test:
        // mutate the workspace, capture, then load via the hidden file
        // input. setInputFiles drives the actual user-facing path.
        await gotoTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "before-load")'));
        await page.evaluate(() => window._turiTabs.create({ name: 'lib.tur' }));
        await page.evaluate(() => window._turiEditor.setValue('(defn add [a b] (+ a b))'));
        await page.waitForTimeout(700);
        const zipBytes = await page.evaluate(() => window._turiTabs.projectZipBytes());

        // Mutate so the load has work to do.
        await page.evaluate(() => {
            const ids = window._turiTabs.tabs().map(t => t.id);
            for (const id of ids.slice(1)) window._turiTabs.close(id);
            window._turiEditor.setValue(';; junk');
        });

        await page.locator('#open-project-input').setInputFiles({
            name: 'project.zip',
            mimeType: 'application/zip',
            buffer: Buffer.from(zipBytes),
        });
        // Loading is async (FileReader -> arrayBuffer); wait for tab count.
        await page.waitForFunction(() => window._turiTabs.tabs().length >= 2);
        const names = await page.evaluate(() =>
            window._turiTabs.tabs().map(t => t.name).sort());
        expect(names).toEqual(['lib.tur', 'main.tur']);
    });
});
