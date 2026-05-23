/**
 * Production smoke tests for turmeric-lang.com
 *
 * Coverage:
 *   - Homepage loads and key elements are visible
 *   - Homepage nav/hero links resolve correctly
 *   - The Try REPL at https://turmeric-lang.com/try
 *   - All guide HTML pages return 200
 *   - API docs index returns 200
 *
 * Run with:  npm run test:prod
 */

import { test, expect } from '@playwright/test';

const MAIN     = 'https://turmeric-lang.com';
const SUBROUTE = `${MAIN}/try`;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Navigate to a URL and assert the response is 200. Returns the response. */
async function get200(page, url) {
    const resp = await page.goto(url, { waitUntil: 'commit' });
    expect(resp.status(), `Expected 200 from ${url}, got ${resp.status()}`).toBe(200);
    return resp;
}

/** Wait for the WASM runtime to finish initializing in the Try page. */
async function waitForReady(page) {
    await expect(page.locator('#wasm-status-text'))
        .toHaveText('Ready');
}

/**
 * Shared REPL test suite.
 *
 * Called once for the subdomain and once for the subroute so the two entry
 * points are always tested with identical assertions. Any divergence in
 * behaviour shows up as one passing and one failing.
 *
 * @param {string} label  - Human-readable name used in describe/test titles.
 * @param {string} tryUrl - Full URL of the Try page under test.
 */
function replSuite(label, tryUrl) {
    test.describe(`Try REPL (${label})`, () => {

        test('loads with HTTP 200 and shows the editor', async ({ page }) => {
            const resp = await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });
            expect(resp.status(), `${tryUrl} returned ${resp.status()}`).toBe(200);
            await expect(page.locator('#editor')).toBeVisible();
        });

        test('page title contains "Try Turmeric"', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });
            await expect(page).toHaveTitle(/Try Turmeric/i);
        });

        test('assets load without fatal JS errors', async ({ page }) => {
            const jsErrors = [];
            page.on('console', msg => {
                if (msg.type() === 'error') jsErrors.push(msg.text());
            });
            page.on('pageerror', err => jsErrors.push(err.message));

            await page.goto(tryUrl, { waitUntil: 'networkidle' });

            // Must be the REPL page, not a fallback/homepage — otherwise the
            // asset check is meaningless (the homepage has no JS errors either).
            await expect(page.locator('#editor'),
                `${tryUrl} did not serve the REPL page (#editor missing)`
            ).toBeVisible();

            const fatal = jsErrors.filter(e =>
                /Failed to (fetch|load)|TypeError|Cannot read|turmeric\.js|main\.js/i.test(e)
            );
            expect(fatal, `Fatal JS errors: ${fatal.join('; ')}`).toHaveLength(0);
        });

        test('WASM module initializes to "Ready"', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });
            await waitForReady(page);
        });

        test('evaluates (println "hello") and shows output', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });
            await waitForReady(page);

            await page.evaluate(() =>
                window._turiEditor.setValue('(println "hello from prod smoke test")')
            );
            await page.click('#run-btn');

            await expect(page.locator('#console'))
                .toContainText('hello from prod smoke test');
            const errors = await page.locator('#console .console-error').allTextContents();
            const evalErrors = errors.filter(e => e.startsWith('#<error'));
            expect(evalErrors, `Eval errors: ${evalErrors.join('; ')}`).toHaveLength(0);
        });

        test('evaluates arithmetic: (+ 1 2) returns 3', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });
            await waitForReady(page);

            await page.evaluate(() => window._turiEditor.setValue('(+ 1 2)'));
            await page.click('#run-btn');

            await expect(page.locator('#console')).toContainText('3');
        });

        test('evaluates a defn then calls it: factorial(10) = 3628800', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });
            await waitForReady(page);

            await page.evaluate(() => window._turiEditor.setValue([
                '(defn factorial [n :int] :int',
                '  (if (<= n 1) 1 (* n (factorial (- n 1)))))',
                '(println (factorial 10))',
            ].join('\n')));
            await page.click('#run-btn');

            await expect(page.locator('#console')).toContainText('3628800');
            const errors = await page.locator('#console .console-error').allTextContents();
            expect(errors.filter(e => e.startsWith('#<error'))).toHaveLength(0);
        });

        test('evaluates algebraic effects: Ask/resume prints 42', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });
            await waitForReady(page);

            await page.evaluate(() => window._turiEditor.setValue([
                '(defeffect Ask [] :int)',
                '(defn use-ask [] :int',
                '  (+ 1 (perform (Ask))))',
                '(println (handle (use-ask)',
                '  (Ask [] k) (resume k 41)))',
            ].join('\n')));
            await page.click('#run-btn');

            await expect(page.locator('#console')).toContainText('42');
            const errors = await page.locator('#console .console-error').allTextContents();
            expect(errors.filter(e => e.startsWith('#<error'))).toHaveLength(0);
        });

        test('home/logo link href points to turmeric-lang.com', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });

            const homeHref = await page.locator('.logo a, a.nav-logo').first().getAttribute('href');
            expect(
                homeHref,
                `Logo href "${homeHref}" should point to turmeric-lang.com`
            ).toMatch(/^https?:\/\/(www\.)?turmeric-lang\.com/);
        });

        test('home link href navigates to turmeric-lang.com homepage', async ({ page }) => {
            await page.goto(tryUrl, { waitUntil: 'domcontentloaded' });

            const homeHref = await page.locator('.logo a, a.nav-logo').first().getAttribute('href');
            const resp = await page.goto(homeHref, { waitUntil: 'commit' });

            expect(resp.status(), `${homeHref} returned ${resp.status()}`).toBe(200);
            expect(
                page.url(),
                `Expected turmeric-lang.com homepage — got ${page.url()}`
            ).toMatch(/^https?:\/\/(www\.)?turmeric-lang\.com\/?$/);
        });

    });
}

// ---------------------------------------------------------------------------
// Homepage
// ---------------------------------------------------------------------------

test.describe('Homepage (turmeric-lang.com)', () => {

    test('loads with HTTP 200', async ({ page }) => {
        await get200(page, `${MAIN}/`);
    });

    test('page title contains "Turmeric"', async ({ page }) => {
        await page.goto(`${MAIN}/`);
        await expect(page).toHaveTitle(/Turmeric/i);
    });

    test('hero heading is visible', async ({ page }) => {
        await page.goto(`${MAIN}/`);
        await expect(page.locator('h1').first()).toBeVisible();
    });

    test('nav contains "Try It", "Guides", and "API Docs" links', async ({ page }) => {
        await page.goto(`${MAIN}/`);
        // Scope to .nav-links to avoid matching "Try it →" in .nav-right
        const navLinks = page.locator('.nav-links');
        await expect(navLinks.getByRole('link', { name: 'Try It' })).toBeVisible();
        await expect(navLinks.getByRole('link', { name: 'Guides' })).toBeVisible();
        await expect(navLinks.getByRole('link', { name: 'API Docs' })).toBeVisible();
    });

    test('hero "Try in browser" button navigates to /try and returns 200', async ({ page }) => {
        await page.goto(`${MAIN}/`);
        const cta = page.locator('.btn-hero-primary').first();
        await expect(cta).toBeVisible();

        const [resp] = await Promise.all([
            page.waitForResponse(r => r.url().includes('/try') && r.status() === 200),
            cta.click(),
        ]);
        expect(resp.status()).toBe(200);
        expect(page.url()).toMatch(/\/try/);
    });

    test('"API Docs" nav link navigates to /docs and returns 200', async ({ page }) => {
        await page.goto(`${MAIN}/`);
        const docsLink = page.locator('nav').first()
            .getByRole('link', { name: /api docs/i });
        const [resp] = await Promise.all([
            page.waitForResponse(r => r.url().includes('/docs') && r.status() === 200),
            docsLink.click(),
        ]);
        expect(resp.status()).toBe(200);
    });

});

// ---------------------------------------------------------------------------
// Try REPL
// ---------------------------------------------------------------------------
replSuite('turmeric-lang.com/try', SUBROUTE);

// ---------------------------------------------------------------------------
// Guides — every generated HTML guide must return 200
// ---------------------------------------------------------------------------

const GUIDES = [
    '/docs/html/guides/',
    '/docs/html/guides/async-await-guide.html',
    '/docs/html/guides/backtracking-guide.html',
    '/docs/html/guides/c-integration-guide.html',
    '/docs/html/guides/cellular-automata-comonad-tutorial.html',
    '/docs/html/guides/checkpointing-guide.html',
    '/docs/html/guides/custom-effects-tutorial.html',
    '/docs/html/guides/effects-system-guide.html',
    '/docs/html/guides/error-handling-guide.html',
    '/docs/html/guides/eval-api.html',
    '/docs/html/guides/hamt-guide.html',
    '/docs/html/guides/hkt-guide.html',
    '/docs/html/guides/logic-programming-guide.html',
    '/docs/html/guides/minikanren-tutorial.html',
    '/docs/html/guides/module-system-guide.html',
    '/docs/html/guides/repl.html',
    '/docs/html/guides/serializable-continuations-guide.html',
    '/docs/html/guides/snake-game-tutorial.html',
    '/docs/html/guides/stm-guide.html',
    '/docs/html/guides/stm-tutorial.html',
    '/docs/html/guides/test-runner-contract.html',
    '/docs/html/guides/threading-guide.html',
];

test.describe('Guides — HTTP 200', () => {
    for (const path of GUIDES) {
        test(`GET ${path}`, async ({ page }) => {
            await get200(page, `${MAIN}${path}`);
        });
    }
});

// ---------------------------------------------------------------------------
// API docs — index must return 200
// ---------------------------------------------------------------------------

test.describe('API Docs — HTTP 200', () => {

    test('GET /docs/html/api/', async ({ page }) => {
        await get200(page, `${MAIN}/docs/html/api/`);
    });

    test('GET /docs/html/api/index.html', async ({ page }) => {
        await get200(page, `${MAIN}/docs/html/api/index.html`);
    });

});
