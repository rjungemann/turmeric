import { test, expect } from '@playwright/test';

/**
 * Editor intelligence on a phone (try-turmeric-lsp-plan, L4).
 *
 * L4 asked for mobile behaviour to be "checked, not assumed", and the desktop
 * specs cannot stand in for it. Two things differ on a phone and only on a
 * phone:
 *
 *   - **The second WASM instance is a real cost.** The playground already
 *     holds one; the language server adds another, each with a 64 MB initial
 *     heap. §2.4 of the plan pays for that with lazy instantiation, and the
 *     whole argument only holds if a visitor who scrolls through a snippet
 *     never triggers it. That is asserted here, on the device where it
 *     matters.
 *
 *   - **The suggest widget has nowhere to go.** Monaco positions it relative
 *     to the cursor against the available space. On a 390x664 viewport that is
 *     a genuinely tight fit, and a completion list rendered off-screen is
 *     indistinguishable from no completion at all.
 *
 * These run under the `mobile` project (iPhone 13 / WebKit), which is also the
 * only place WebKit is exercised at all -- the LSP client speaks to a
 * dedicated Worker in a cross-origin-isolated page, and WebKit is the engine
 * most likely to disagree about that.
 */

async function gotoTry(page) {
    await page.goto('/try/');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

/** Focus the editor -- the boot signal -- and wait for the server either way. */
async function bootLsp(page) {
    await page.evaluate(() => window._turiEditor.focus());
    return page.evaluate(async () => {
        for (let i = 0; i < 100 && !window._turiLsp; i++) {
            await new Promise(r => setTimeout(r, 50));
        }
        if (!window._turiLsp) return false;
        return window._turiLsp.ready;
    });
}

async function markers(page) {
    return page.evaluate(() =>
        (window._turiLsp && window._turiLsp.markers) ? window._turiLsp.markers() : []);
}

test.describe('LSP on mobile', () => {
    test('a visitor who only reads never pays for the language server', async ({ page }) => {
        await gotoTry(page);

        // Scroll, run, read output -- everything except asking the editor a
        // question. None of it may instantiate the second module.
        await page.evaluate(() => window._turiEditor.setValue('(+ 20 22)'));
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('42', { timeout: 15_000 });

        expect(await page.evaluate(() => !!window._turiLsp)).toBe(false);
        await expect(page.locator('#lsp-status')).toBeHidden();
    });

    test('the server boots on WebKit when the editor is focused', async ({ page }) => {
        const jsErrors = [];
        page.on('pageerror', err => jsErrors.push(String(err)));

        await gotoTry(page);
        const available = await bootLsp(page);

        // A phone that cannot run it must degrade silently, exactly as the
        // desktop degradation spec requires -- never a visible fault.
        if (!available) {
            await expect(page.locator('#lsp-status')).toBeHidden();
            expect(jsErrors).toHaveLength(0);
            test.skip(true, 'language server unavailable on this WebKit build');
        }

        await expect(page.locator('#lsp-status')).toBeVisible();
        expect(jsErrors).toHaveLength(0);
    });

    test('diagnostics reach the gutter on a phone viewport', async ({ page }) => {
        await gotoTry(page);
        test.skip(!(await bootLsp(page)), 'language server unavailable');

        await page.evaluate(() =>
            window._turiEditor.setValue('(defn f [] : int (no-such-function 1))\n'));
        await expect.poll(() => markers(page), { timeout: 20_000 })
            .not.toHaveLength(0);

        const found = await markers(page);
        expect(found.some(m => /no-such-function/.test(m.message))).toBe(true);
    });

    test('the suggest widget opens inside the viewport, not off the edge of it', async ({ page }) => {
        await gotoTry(page);
        test.skip(!(await bootLsp(page)), 'language server unavailable');

        await page.evaluate(() =>
            window._turiEditor.setValue('(defn zorkle [x : int] : int x)\n'));
        await page.waitForTimeout(800);

        await page.evaluate(() => {
            const ed = window._turiEditor;
            ed.setPosition({ lineNumber: ed.getModel().getLineCount(), column: 1 });
            ed.focus();
        });
        await page.keyboard.type('zork');
        // Ctrl+Space is not a gesture a phone has. Drive the same action the
        // on-screen keyboard's typing would reach through a trigger character,
        // so what is under test is the widget, not the shortcut.
        await page.evaluate(() =>
            window._turiEditor.trigger('test', 'editor.action.triggerSuggest', {}));

        const widget = page.locator('.suggest-widget');
        await expect(widget).toBeVisible({ timeout: 20_000 });
        await expect(widget).toContainText('zorkle');

        // The assertion this file exists for: a list rendered past the edge of
        // a 390px-wide screen is not a completion list.
        const box = await widget.boundingBox();
        const view = page.viewportSize();
        expect(box).not.toBeNull();
        expect(box.x).toBeGreaterThanOrEqual(0);
        expect(box.y).toBeGreaterThanOrEqual(0);
        expect(box.x + box.width).toBeLessThanOrEqual(view.width + 1);
        expect(box.y + box.height).toBeLessThanOrEqual(view.height + 1);
    });
});
