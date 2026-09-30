import { test, expect } from '@playwright/test';
import { CONTENT_SECURITY_POLICY } from '../csp.js';

// security-audit-plan WP6: the Content-Security-Policy is enforced on every
// page the site serves, and nothing the site ships trips it.
//
// A violation is collected from `securitypolicyviolation` rather than read off
// the console: the event carries the blocked URI and directive, which is what
// a failure message needs to name.

async function collectViolations(page) {
    await page.addInitScript(() => {
        window.__cspViolations = [];
        document.addEventListener('securitypolicyviolation', (e) => {
            window.__cspViolations.push(
                `${e.effectiveDirective} blocked ${e.blockedURI || '(inline)'}`
                + (e.sourceFile ? ` at ${e.sourceFile}:${e.lineNumber}` : ''));
        });
    });
}

async function violations(page) {
    return page.evaluate(() => window.__cspViolations || []);
}

async function waitForReady(page) {
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

// The pages the build ships (vite.config.js inputs), plus one page of each
// generated kind: a guide, the guides index, an API page and the spices index
// each carry different scripts.
const PAGES = [
    '/',
    '/tour/',
    '/trowel/',
    '/ci/',
    '/docs/html/guides/quickstart.html',
    '/docs/html/guides/index.html',
    '/docs/html/api/index.html',
    '/docs/html/api/arc.html',
    '/docs/html/spices/index.html',
];

test.describe('Content-Security-Policy', () => {
    for (const path of PAGES) {
        test(`${path} is served under the policy and trips none of it`, async ({ page }) => {
            await collectViolations(page);
            const res = await page.goto(path);
            expect(res.ok()).toBe(true);
            expect(res.headers()['content-security-policy']).toBe(CONTENT_SECURITY_POLICY);
            await page.waitForLoadState('networkidle');
            expect(await violations(page)).toEqual([]);
        });
    }

    test('Try Turmeric boots and evaluates under the policy', async ({ page }) => {
        await collectViolations(page);
        const res = await page.goto('/try/');
        expect(res.headers()['content-security-policy']).toBe(CONTENT_SECURITY_POLICY);
        await waitForReady(page);

        // The eval worker compiled the wasm ('wasm-unsafe-eval') and runs it.
        await page.evaluate(() => window._turiEditor.setValue('(println (+ 7.1 1.0))'));
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('8.1', { timeout: 15_000 });

        expect(await violations(page)).toEqual([]);
    });

    test('an injected inline handler does not run', async ({ page }) => {
        // The policy's whole point for an injection that gets past escaping:
        // markup lands, its script does not.
        await page.goto('/try/');
        await waitForReady(page);
        const ran = await page.evaluate(async () => {
            window.__cspProbe = false;
            const div = document.createElement('div');
            div.innerHTML = '<img src="data:," onerror="window.__cspProbe = true">';
            document.body.appendChild(div);
            await new Promise((r) => setTimeout(r, 250));
            div.remove();
            return window.__cspProbe;
        });
        expect(ran).toBe(false);
    });
});

// W-2. The escaping itself, independent of the policy: a value interpolated
// into an attribute stays inside that attribute.
test.describe('attribute escaping', () => {
    test('a #lang line cannot add attributes to the language picker', async ({ page }) => {
        // The buffer's first line is user data -- it arrives from a pasted
        // file, a restored tab or an opened project zip -- and the picker
        // renders a row for whatever base it names. With `"` unescaped, the
        // token closed the value attribute and the rest became attributes of
        // the radio button: event handlers, autofocus, style.
        await page.goto('/try/');
        await waitForReady(page);
        const base = `x"data-injected="1"onfocus="window.__pwned=1"autofocus="`;
        await page.evaluate((b) => window._turiEditor.setValue(`#lang ${b}\n(+ 1 2)\n`), base);

        await expect.poll(() => page.evaluate((b) => Array.from(
            document.querySelectorAll('#lang-bases input[type=radio]'))
            .some((r) => r.value === b), base)).toBe(true);

        const attrs = await page.evaluate(() => Array.from(
            document.querySelectorAll('#lang-bases input, #lang-bases label'))
            .flatMap((el) => Array.from(el.attributes, (a) => a.name)));
        expect(attrs.filter((n) => !['type', 'name', 'value', 'class', 'title'].includes(n)))
            .toEqual([]);
        expect(await page.evaluate(() => window.__pwned)).toBeUndefined();
    });
});

// W-2. The console transcript persisted in localStorage is data, and whatever
// is read back out of storage is rendered as text -- never parsed as markup.
test.describe('console persistence', () => {
    test('the legacy HTML transcript key is discarded, never rendered', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await page.evaluate(() => localStorage.setItem('tur.try.console.v1',
            JSON.stringify(['<img src="data:," class="planted">planted'])));
        await page.reload();
        await waitForReady(page);
        await expect(page.locator('#console img')).toHaveCount(0);
        await expect(page.locator('#console')).not.toContainText('planted');
        expect(await page.evaluate(() => localStorage.getItem('tur.try.console.v1'))).toBeNull();
    });

    test('a hostile stored record renders as text', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await page.evaluate(() => {
            // Nesting far past anything the console writes: rendered to a
            // bounded depth, not recursed into without limit.
            let deep = ['too deep'];
            for (let k = 0; k < 64; k++) deep = [['span', 'nest', deep]];
            localStorage.setItem('tur.try.console.v2', JSON.stringify([
                [['img', 'planted', []], '<b>bold?</b>', ['span', 'console-result', ['kept']]],
                '<img src="data:,">',
                deep,
                { not: 'a line' },
            ]));
        });
        await page.reload();
        await waitForReady(page);
        await expect(page.locator('#console img')).toHaveCount(0);
        await expect(page.locator('#console b')).toHaveCount(0);
        await expect(page.locator('#console')).toContainText('<b>bold?</b>');
        await expect(page.locator('#console .console-result')).toHaveText('kept');
    });

    test('the transcript survives a reload with its classes', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "a<b>&\\"q\'")'));
        await page.click('#run-btn');
        await expect(page.locator('#console .console-output', { hasText: `a<b>&"q'` }))
            .toHaveCount(1, { timeout: 15_000 });
        // The persist is debounced; wait for it to land before reloading.
        await expect.poll(() => page.evaluate(() =>
            localStorage.getItem('tur.try.console.v2') || '')).toContain('a<b>&');
        await page.reload();
        await waitForReady(page);
        await expect(page.locator('#console .console-output', { hasText: `a<b>&"q'` }))
            .toHaveCount(1);
        await expect(page.locator('#console b')).toHaveCount(0);
    });
});

// W-3. A program that never returns cannot take the REPL with it: the eval
// Worker is serial, so without a way to stop it every later Run waited forever.
test.describe('eval watchdog', () => {
    async function runForever(page) {
        await page.evaluate(() => window._turiEditor.setValue('(while true 0)'));
        await page.click('#run-btn');
    }

    async function evaluatesAgain(page) {
        await waitForReady(page);
        await page.evaluate(() => window._turiEditor.setValue('(println (+ 7.1 1.0))'));
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('8.1', { timeout: 15_000 });
        await expect(page.locator('#stop-btn')).toBeHidden();
    }

    test('Stop ends a runaway program and the REPL evaluates again', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await runForever(page);

        // Offered once the run has held the interpreter for a second.
        await expect(page.locator('#stop-btn')).toBeVisible({ timeout: 5_000 });
        await page.click('#stop-btn');

        await expect(page.locator('#console .console-error', { hasText: 'Stopped' }))
            .toHaveCount(1);
        await expect(page.locator('#console')).toContainText('Started a fresh session',
                                                             { timeout: 30_000 });
        await evaluatesAgain(page);
    });

    test('the watchdog stops a runaway program on its own', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await page.evaluate(() => { window._turiEval.timeoutMs = 2000; });
        await runForever(page);

        await expect(page.locator('#console .console-error', { hasText: 'still running after 2 s' }))
            .toHaveCount(1, { timeout: 10_000 });
        await expect(page.locator('#console')).toContainText('Started a fresh session',
                                                             { timeout: 30_000 });
        await evaluatesAgain(page);
    });

    test('a quick run never offers Stop', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await page.evaluate(() => window._turiEditor.setValue('(println (+ 7.1 1.0))'));
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('8.1', { timeout: 15_000 });
        await page.waitForTimeout(1500);
        await expect(page.locator('#stop-btn')).toBeHidden();
    });
});
