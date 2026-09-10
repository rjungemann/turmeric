import { test, expect } from '@playwright/test';

async function gotoTry(page) {
    // ?sw=1 opts back in to the service worker, which main.js disables on
    // loopback hosts so a dev server stops serving yesterday's main.js and
    // styles.css cache-first. The PWA assertions below need the real thing.
    await page.goto('/try/?sw=1');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });

    // Let the worker finish installing before any test reloads.
    //
    // `install` precaches the wasm and the whole docs pack before it calls
    // skipWaiting(), so a reload fired the instant the REPL says Ready lands
    // in the middle of that, on a page no worker controls yet. Against the
    // vite dev server WebKit then refuses to start /eval-worker.js at all
    // ("Refused to load ... because of Cross-Origin-Embedder-Policy") -- and
    // it does so with the service worker removed entirely, so it is the dev
    // server's revalidation behaviour, not anything these specs are about.
    // The built site does not do it. Waiting here keeps the reload tests
    // measuring what they claim to measure.
    //
    // This does NOT paper over the bug these specs caught: with the
    // pre-fix sw.js they still go red after this wait, because the failure
    // there is on the controlled reload itself.
    await page.evaluate(async () => {
        if (!('serviceWorker' in navigator)) return;
        await navigator.serviceWorker.ready;
    });
}

test.describe('Mobile layout + PWA', () => {
    // webkit-sw-controlled-reload-fails-wasm-init, fix direction 1: the two
    // reload tests fail on WebKit with "Failed to load WASM" and the CI log
    // carries no browser console, so the real error (which main.js has already
    // console.error'd, and which eval-worker.js posted as init-error) was
    // unknowable.  Collect console output and page errors for every test and,
    // on failure, attach them to the report and echo them into the log.
    const captured = [];
    test.beforeEach(async ({ page }) => {
        captured.length = 0;
        page.on('console', (msg) => captured.push(`[console.${msg.type()}] ${msg.text()}`));
        page.on('pageerror', (err) => captured.push(`[pageerror] ${err.message}`));
    });
    test.afterEach(async ({ page }, testInfo) => {
        if (testInfo.status === testInfo.expectedStatus) return;
        let onPage = '';
        try { onPage = await page.locator('#console').innerText({ timeout: 2_000 }); } catch (_) {}
        const dump = [...captured, onPage ? `[#console]\n${onPage}` : ''].filter(Boolean).join('\n');
        await testInfo.attach('browser-console', { body: dump || '(nothing captured)', contentType: 'text/plain' });
        console.log(`--- browser console for "${testInfo.title}" ---\n${dump || '(nothing captured)'}\n---`);
    });

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
        // Wait for the write itself, not for a guessed interval. An edit
        // reaches localStorage through TWO chained 250ms debounces --
        // persistContent -> persistTabs (main.js:2161, main.js:231) -- so the
        // 400ms this used to sleep was short of the ~500ms floor and the tab
        // snapshot was still unwritten at reload time. Polling the key is
        // immune to both that arithmetic and to a slow machine.
        await page.waitForFunction(
            (want) => (localStorage.getItem('tur.try.tabs.v1') || '').includes(want),
            'persist-me-7', { timeout: 10_000 });

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

    // webkit-sw-controlled-reload-fails-wasm-init. On WebKit -- real Safari
    // included -- `fetch(request)` inside a service worker rejects with
    // `TypeError: Load failed` when the request is a top-level dedicated worker
    // script the browser already holds in its HTTP cache. That is precisely a
    // returning visitor: the first, uncontrolled load put /eval-worker.js in the
    // HTTP cache, and the controlled reload could then never construct the
    // Worker, so the REPL reported "Failed to load WASM".
    //
    // The two reload tests above already go red if that regresses, but they say
    // "the status never reached Ready", which is a symptom shared by half a
    // dozen unrelated faults. This one names the mechanism: the worker script is
    // precached, and the evaluator genuinely runs on a controlled reload.
    test('the eval worker survives a service-worker-controlled reload', async ({ page }) => {
        await gotoTry(page);
        await page.evaluate(() => navigator.serviceWorker.ready);

        // The precache row is half the fix -- without it the reload has to go
        // through the network path that WebKit breaks, and an offline visit has
        // no evaluator at all.
        const precached = await page.evaluate(async () => {
            for (const k of await caches.keys()) {
                if (await (await caches.open(k)).match('/eval-worker.js')) return true;
            }
            return false;
        });
        expect(precached).toBe(true);

        await page.reload();
        await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });

        // Controlled, and the Worker really did start: `Ready` is only posted
        // after eval-worker.js answered the `init` message.
        expect(await page.evaluate(() => !!navigator.serviceWorker.controller)).toBe(true);

        // And it still evaluates -- a worker that loaded but cannot run is the
        // same outage to a reader.
        await page.evaluate(() => window._turiEditor.setValue('(println "sw-reload-ok")'));
        await page.locator('#run-btn').click();
        await expect(page.locator('#console')).toContainText('sw-reload-ok', { timeout: 30_000 });
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
