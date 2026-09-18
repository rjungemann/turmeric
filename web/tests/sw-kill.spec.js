// The service-worker kill-switch, run against a real worker and real caches.
//
// This is the recovery path for a client stuck on a build it will not let go
// of. The previous version of sw-kill.js was written as insurance, referenced
// by nothing, and never executed once -- and it did its work in an `async`
// listener with no event.waitUntil(), so the browser could terminate the worker
// at the first await, before a single cache was deleted. An untested kill
// switch is not insurance.
//
// So these do not stub anything: a real worker is registered, real entries are
// put in Cache Storage, and the assertions are on what the browser reports.
//
// Note what the end state is NOT. The page keeps running main.js, which
// registers /sw.js on load, so "zero registrations forever" is not the target
// and asserting it only measures the race. While the kill-switch is deployed,
// /sw.js IS the kill-switch: every load installs it, it wipes, it unregisters.
// That is the steady state during recovery, and it is fine. What has to hold is
// that the caches go, and that the page does not spin.

import { test, expect } from '@playwright/test';

const cacheKeys = (page) => page.evaluate(() => caches.keys().then((k) => k.sort()));

async function openTry(page) {
    // ?sw=1 opts back in to the service worker, which main.js disables on
    // loopback hosts so a dev server stops serving yesterday's bundle.
    await page.goto('/try/?sw=1');
    await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
    await page.evaluate(() => navigator.serviceWorker.ready);
}

/* Registering a different script at the same scope is what deploying the
 * kill-switch at /sw.js does to a client: a different worker script takes over.
 * (Production changes the bytes at one URL rather than the URL itself, which
 * reaches the same install/activate path.) */
const installKillSwitch = (page) => page.evaluate(() =>
    navigator.serviceWorker.register('/sw-kill.js', { scope: '/' }));

test.describe('service worker kill-switch', () => {
    test('deletes every cache, whatever it is named', async ({ page }) => {
        await openTry(page);

        // Stand in for a stuck client's caches. Deliberately NOT named after
        // today's CACHE_VERSION scheme: a client stuck on an old build has
        // cache names from that build, and the kill-switch must not assume it
        // recognises them.
        await page.evaluate(async () => {
            const c = await caches.open('tur-try-v1-0.1.0-ancient-precache');
            await c.put('/stuck-asset.js', new Response('stale'));
            const d = await caches.open('some-other-cache-entirely');
            await d.put('/other.js', new Response('stale'));
        });
        expect(await cacheKeys(page)).toEqual(
            expect.arrayContaining(['tur-try-v1-0.1.0-ancient-precache',
                                    'some-other-cache-entirely']));

        await installKillSwitch(page);

        await expect.poll(() => cacheKeys(page), { timeout: 20_000 })
            .not.toContain('tur-try-v1-0.1.0-ancient-precache');
        expect(await cacheKeys(page)).not.toContain('some-other-cache-entirely');
    });

    test('unregisters the worker that was controlling the page', async ({ page }) => {
        await openTry(page);
        const before = await page.evaluate(
            () => navigator.serviceWorker.controller?.scriptURL || null);
        expect(before).toContain('/sw.js');

        await installKillSwitch(page);

        // The old worker is gone: either nothing controls this page any more,
        // or the kill-switch itself does (it has no fetch handler, so every
        // request goes to the network either way). What must NOT still be true
        // is the original worker sitting in front of the network.
        await expect.poll(async () => {
            const url = await page.evaluate(
                () => navigator.serviceWorker.controller?.scriptURL || '');
            return url.endsWith('/sw.js');
        }, { timeout: 20_000 }).toBe(false);
    });

    test('reloads the page once, and does not spin', async ({ page }) => {
        // The loop this guards against is not hypothetical: while the
        // kill-switch is deployed, the page it reloads registers /sw.js on
        // load, which IS the kill-switch, which activates and reloads again.
        // A PWA spinning is worse than a PWA one build behind.
        let navigations = 0;
        page.on('framenavigated', (f) => { if (f === page.mainFrame()) navigations++; });

        await openTry(page);
        navigations = 0;

        await installKillSwitch(page);

        // One reload, carrying the marker that stops the next activation from
        // reloading again.
        await expect.poll(() => page.url(), { timeout: 20_000 }).toContain('swkill=');
        await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });

        const afterFirst = navigations;
        expect(afterFirst).toBeGreaterThan(0);

        // Give the re-registered kill-switch every chance to reload us again.
        await page.waitForTimeout(6000);
        expect(navigations).toBe(afterFirst);
    });

    test('the build can ship the kill-switch at /sw.js', async () => {
        // The delivery half. The worker above is only reachable in production
        // if the build puts it at the URL a stuck client refetches, and a flag
        // that silently does nothing is a recovery everyone believes happened
        // -- so swKillSwitch() throws rather than warns when it finds no dist
        // sw.js to replace.
        const { readFileSync } = await import('node:fs');
        const cfg = readFileSync(new URL('../vite.config.js', import.meta.url), 'utf-8');
        expect(cfg).toContain("process.env.TUR_SW_KILL === '1'");
        expect(cfg).toContain('public/sw-kill.js');
        expect(cfg).toMatch(/throw new Error\('TUR_SW_KILL=1 but no dist sw\.js/);
        expect(cfg).toMatch(/plugins:\s*\[[^\]]*swKillSwitch\(\)/);
    });
});
