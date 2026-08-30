import { test, expect } from '@playwright/test';

async function gotoTry(page) {
    // ?sw=1 opts back in to the service worker, which main.js disables on
    // loopback hosts so a dev server stops serving yesterday's main.js and
    // styles.css cache-first. The PWA assertions below need the real thing.
    await page.goto('/try/?sw=1');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

test.describe('Mobile layout + PWA', () => {
    test('mobile viewport renders split handle and stacks panes', async ({ page }) => {
        await gotoTry(page);

        const container = page.locator('.repl-container');
        await expect(container).toBeVisible();

        const display = await container.evaluate((el) => getComputedStyle(el).display);
        expect(display).toBe('grid');

        const handle = page.locator('#split-handle');
        await expect(handle).toBeVisible();

        const ariaOrientation = await handle.getAttribute('aria-orientation');
        expect(ariaOrientation).toBe('horizontal');
    });

    test('split fraction persists across reload', async ({ page }) => {
        await gotoTry(page);

        await page.evaluate(() => {
            const c = document.querySelector('.repl-container');
            c.style.setProperty('--split-v', '0.3');
            localStorage.setItem('tur.try.split.v.v1', '0.3');
        });

        await page.reload();
        await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });

        const v = await page.evaluate(() => {
            const c = document.querySelector('.repl-container');
            return parseFloat(getComputedStyle(c).getPropertyValue('--split-v'));
        });
        expect(v).toBeCloseTo(0.3, 2);
    });

    test('editor buffer persists across reload', async ({ page }) => {
        await gotoTry(page);

        const probe = '(println "persist-me-7")';
        await page.evaluate((c) => window._turiEditor.setValue(c), probe);
        // Wait past the 250ms persistence debounce.
        await page.waitForTimeout(400);

        await page.reload();
        await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });

        const value = await page.evaluate(() => window._turiEditor.getValue());
        expect(value).toBe(probe);
    });

    test('service worker registers and precaches WASM', async ({ page, context }) => {
        await gotoTry(page);

        // Wait for the SW to take control.
        const registered = await page.evaluate(async () => {
            if (!('serviceWorker' in navigator)) return false;
            const reg = await navigator.serviceWorker.ready;
            return !!reg.active;
        });
        expect(registered).toBe(true);

        // Confirm /turmeric.wasm is in a cache.
        const cached = await page.evaluate(async () => {
            const keys = await caches.keys();
            for (const k of keys) {
                const c = await caches.open(k);
                const hit = await c.match('/turmeric.wasm');
                if (hit) return true;
            }
            return false;
        });
        expect(cached).toBe(true);
    });

    test('manifest is reachable and valid', async ({ page }) => {
        const res = await page.request.get('/manifest.webmanifest');
        expect(res.ok()).toBe(true);
        const json = await res.json();
        expect(json.start_url).toBe('/try/');
        expect(json.display).toBe('standalone');
        expect(Array.isArray(json.icons)).toBe(true);
    });
});
