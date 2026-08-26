// In-app docs browser (OD2).
//
// The pane renders the docs pack -- chrome-free fragments emitted by the same
// generators that build turmeric-lang.com/docs/html/ -- over the REPL. The
// contract these tests pin is that reading docs is not a navigation: the
// editor buffer and console survive browsing, cross-links resolve inside the
// pane, and a #doc= deep link restores a location.
//
// Offline behaviour is a separate concern and lives in docs-offline.spec.js.

import { test, expect } from '@playwright/test';

/**
 * Open /try/ and wait for the editor.
 *
 * Deliberately does NOT wait for `#wasm-status-text` to read Ready: the docs
 * pane must not depend on the WASM boot, and pinning that here is what keeps
 * it that way.
 */
async function openTry(page, hash = '') {
    await page.goto(`/try/${hash}`);
    await page.waitForFunction(() => !!window.turmericApp && !!window._turiEditor,
                               null, { timeout: 30_000 });
}

async function openDocs(page) {
    await page.click('#docs-btn');
    await page.waitForSelector('#docs-nav .docs-nav-section', { timeout: 15_000 });
}

/** Navigate the pane by ref, bypassing the collapsed-by-default nav groups. */
async function gotoDoc(page, ref) {
    await page.evaluate((r) => window.turmericApp.showDocsPage(r), ref);
    await page.waitForFunction(
        (r) => window.turmericApp.getState().docsRef === r, ref, { timeout: 15_000 });
}

test.describe('docs pane', () => {
    test('opens from the toolbar and lists guides and API modules', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        const sections = await page.locator('#docs-nav .docs-nav-section h4').allTextContents();
        expect(sections).toContain('Guides');
        expect(sections).toContain('API');

        // The version stamp answers "are my offline docs stale?" at a glance.
        await expect(page.locator('#docs-version')).toContainText(/^Docs v\d+\.\d+\.\d+$/);

        // A page is showing, rendered from a fragment (no site chrome inside).
        await expect(page.locator('#docs-article h1')).toHaveCount(1);
        await expect(page.locator('#docs-article .site-header')).toHaveCount(0);
    });

    test('renders a guide with highlighting, toggles, and load-into-editor', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');

        await expect(page.locator('#docs-article h1')).toContainText('Higher-Kinded Types');
        // The shared guide runtime from /docs-pack/guide.js ran against the
        // freshly rendered fragment.
        expect(await page.locator('#docs-article .hl-keyword').count()).toBeGreaterThan(10);
        expect(await page.locator('#docs-article .code-syntax-toggle').count()).toBeGreaterThan(0);
        expect(await page.locator('#docs-article .docs-load-btn').count()).toBeGreaterThan(0);
    });

    test('a guide cross-link stays inside the pane', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');

        const link = page.locator('#docs-article a[href^="#doc="]').first();
        const href = await link.getAttribute('href');
        const target = href.slice('#doc='.length);
        await link.click();

        await page.waitForFunction(
            (t) => window.turmericApp.getState().docsRef === t, target, { timeout: 15_000 });
        // Still on /try/ -- the whole point of embedding rather than linking out.
        expect(new URL(page.url()).pathname).toBe('/try/');
        expect(await page.evaluate(() => window.turmericApp.getState().docsOpen)).toBe(true);
    });

    test('search finds pages and symbols', async ({ page }) => {
        await openTry(page);
        await openDocs(page);

        await page.fill('#docs-search', 'vec');
        await expect(page.locator('#docs-nav .docs-results li').first()).toBeVisible();
        const headers = await page.locator('#docs-nav .docs-nav-section h4').allTextContents();
        // One box, two result kinds: pack pages and doc-names.json symbols.
        expect(headers).toContain('Pages');
        expect(headers).toContain('Symbols');

        // Clearing restores the tree.
        await page.fill('#docs-search', '');
        await expect(page.locator('#docs-nav .docs-nav-group').first()).toBeVisible();
    });

    test('#doc= deep-links restore a location and compose with #code=', async ({ page }) => {
        await openTry(page, '#doc=guides/effects-system-guide');
        await page.waitForFunction(
            () => window.turmericApp.getState().docsRef === 'guides/effects-system-guide',
            null, { timeout: 20_000 });
        await expect(page.locator('#docs-article h1')).toContainText('Effects');

        // Sharing writes #code=; it must merge with the docs key rather than
        // replacing the hash and closing the pane mid-read.
        await page.evaluate(() => {
            window._turiEditor.setValue('(+ 1 2)');
            window.turmericApp.getState();
        });
        await page.waitForTimeout(1200);
        const hash = await page.evaluate(() => location.hash);
        expect(hash).toContain('doc=guides/effects-system-guide');
        expect(await page.evaluate(() => window.turmericApp.getState().docsOpen)).toBe(true);
    });

    test('browsing docs leaves the REPL session untouched', async ({ page }) => {
        await openTry(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "keep me")'));

        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');
        await gotoDoc(page, 'api/tur-list');
        await page.click('#docs-close');

        await expect(page.locator('#docs-overlay')).toBeHidden();
        expect(await page.evaluate(() => window._turiEditor.getValue()))
            .toBe('(println "keep me")');
    });

    test('load into editor drops a snippet in the buffer and closes the pane', async ({ page }) => {
        await openTry(page);
        await openDocs(page);
        await gotoDoc(page, 'guides/hkt-guide');

        const btn = page.locator('#docs-article .docs-load-btn').first();
        await btn.click();

        await expect(page.locator('#docs-overlay')).toBeHidden();
        const buffer = await page.evaluate(() => window._turiEditor.getValue());
        expect(buffer.trim().length).toBeGreaterThan(0);
    });

    test('the doc panel opens the pane instead of navigating away', async ({ page }) => {
        await openTry(page);
        // Surface the doc panel the way the REPL does, then follow its link.
        await page.evaluate(() => window.turmericApp.showDocPanel('cons', null, null));
        await expect(page.locator('#doc-pane')).toBeVisible();

        await page.click('#doc-full-link');
        await expect(page.locator('#docs-overlay')).toBeVisible();
        expect(new URL(page.url()).pathname).toBe('/try/');
    });
});
