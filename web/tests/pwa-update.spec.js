// How an installed PWA notices, and applies, a new build.
//
// The reported symptom was an installed Try Turmeric on iOS stuck several
// builds behind while Safari on the same phone was current, and immune to every
// lever a user has: relaunching the app, "Force update", and deleting and
// re-adding the icon. It was not a caching subtlety -- the app had no update
// path at all. main.js called `register()` and nothing else: no `update()`, no
// `controllerchange` handler (`grep -c` for either returned 0).
//
// That leaves two holes, and these specs pin both:
//
//   - NOTICING.  The browser checks for a new sw.js on a navigation in scope.
//     `register()` is one, but it runs on `load`, and a standalone web app
//     relaunched from the app switcher is RESUMED, not navigated -- so on the
//     platform where this matters the check never ran. Ask on every foreground.
//   - APPLYING.  sw.js self-skipWaiting()s and claims clients, so a new worker
//     does take over -- but nothing re-rendered, so the reader kept looking at
//     the old worker's assets until some later cold launch that iOS may never
//     give them.
//
// These stub the service worker rather than driving a real one: what is under
// test is main.js's reaction to the lifecycle, and a real SW update needs two
// builds and a deploy to provoke.
//
// EVERY NEGATIVE HERE IS PRECEDED BY A POSITIVE. A "does not reload" assertion
// passes just as well when the listener was never attached, and on this file's
// first run all of them did: main.js disables service workers on loopback hosts
// unless the URL says `?sw=1`, so the entire branch under test was dead and the
// suite was green about nothing. Proving the wiring is live first is what makes
// the negatives mean anything.

import { test, expect } from '@playwright/test';

/**
 * Install a fake serviceWorker container before any page script runs.
 *
 * `controller` and `document.visibilityState` are readonly getters, so they are
 * redefined here rather than assigned -- and visibilityState is backed by a
 * variable the test can flip, which is the only way to exercise "defer the
 * reload until the app is next foregrounded" for real.
 */
async function stubServiceWorker(page, { controlled }) {
    await page.addInitScript(({ controlled }) => {
        window.__updateChecks = 0;
        window.__reloaded = false;
        window.__vis = 'visible';
        Object.defineProperty(document, 'visibilityState', {
            get: () => window.__vis,
            configurable: true,
        });
        window.__turiReload = () => { window.__reloaded = true; };
        window.__turiNavigate = () => { window.__reloaded = true; };

        const reg = { update: async () => { window.__updateChecks++; } };
        const sw = navigator.serviceWorker;
        Object.defineProperty(sw, 'controller', {
            get: () => (controlled ? {} : null),
            configurable: true,
        });
        sw.register = async () => reg;
        sw.getRegistrations = async () => [reg];
    }, { controlled });
}

/* `?sw=1` is load-bearing: see the header comment. */
async function openTry(page) {
    await page.goto('/try/?sw=1');
    await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
}

const foreground = async (page) => {
    await page.evaluate(() => {
        window.__vis = 'hidden';
        document.dispatchEvent(new Event('visibilitychange'));
        window.__vis = 'visible';
        document.dispatchEvent(new Event('visibilitychange'));
    });
};

/**
 * Background and foreground the app until the update check fires.
 *
 * Doubles as the wiring probe every negative assertion below leans on: the
 * listeners are attached from the `load` handler's promise chain, so this
 * retries rather than assuming they are up yet, and a test that gets past it
 * knows the branch under test is live.
 */
async function foregroundUntilChecked(page) {
    await expect.poll(async () => {
        await foreground(page);
        return page.evaluate(() => window.__updateChecks);
    }, { timeout: 20_000 }).toBeGreaterThan(0);
}

test.describe('PWA update lifecycle', () => {
    test('coming back to the foreground checks for a new build', async ({ page }) => {
        await stubServiceWorker(page, { controlled: true });
        await openTry(page);

        // The whole point: a relaunched-from-the-switcher PWA fires no
        // navigation, so this is the only check that will ever happen.
        await foregroundUntilChecked(page);
    });

    test('a new worker taking over reloads the page -- once, and not mid-edit',
         async ({ page }) => {
        await stubServiceWorker(page, { controlled: true });
        await openTry(page);
        await foregroundUntilChecked(page);   // wiring is live

        await page.evaluate(() =>
            navigator.serviceWorker.dispatchEvent(new Event('controllerchange')));

        // Not while the reader is looking at it: an edit reaches localStorage
        // through two chained 250ms debounces, so reloading on the keystroke
        // that coincided with an activation drops typing.
        await page.waitForTimeout(500);
        expect(await page.evaluate(() => window.__reloaded)).toBe(false);

        await foreground(page);
        await expect.poll(() => page.evaluate(() => window.__reloaded), { timeout: 10_000 })
            .toBe(true);

        // Once. A second activation must not start a reload loop.
        await page.evaluate(() => {
            window.__reloaded = false;
            navigator.serviceWorker.dispatchEvent(new Event('controllerchange'));
        });
        await foreground(page);
        await page.waitForTimeout(500);
        expect(await page.evaluate(() => window.__reloaded)).toBe(false);
    });

    test('the first visit does not reload itself', async ({ page }) => {
        // An uncontrolled load becomes controlled when `activate` calls
        // clients.claim(), which fires controllerchange for a worker whose
        // assets this page is ALREADY running. Reloading there would make every
        // first visit flash for no reason.
        await stubServiceWorker(page, { controlled: false });
        await openTry(page);
        await foregroundUntilChecked(page);   // wiring is live

        await page.evaluate(() =>
            navigator.serviceWorker.dispatchEvent(new Event('controllerchange')));
        await foreground(page);
        await page.waitForTimeout(500);

        expect(await page.evaluate(() => window.__reloaded)).toBe(false);
    });

    test('the overflow menu names the build that is actually running',
         async ({ page }) => {
        // No stub here: the stamp is read from the live cache key, so this one
        // case needs the real worker. Without one there is nothing to report,
        // which is the documented behaviour.
        await page.goto('/try/?sw=1');
        await page.waitForFunction(() => !!window.turmericApp, null, { timeout: 30_000 });
        await page.evaluate(() => navigator.serviceWorker.ready);

        await expect.poll(async () => {
            await page.locator('#more-btn').click();
            const text = await page.locator('#build-stamp').textContent();
            await page.keyboard.press('Escape');
            return text || '';
        }, { timeout: 20_000 }).toMatch(/^Build \d+\.\d+\.\d+/);
    });
});
