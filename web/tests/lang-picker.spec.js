import { test, expect } from '@playwright/test';

// ---------------------------------------------------------------------------
// Language picker (try-turmeric-lang-toggle-plan §5).
//
// The picker edits the #lang line in place; the buffer stays the source of
// truth. These tests exercise the text round-trip, which works before (and
// without) the WASM runtime: the popover renders from the module's registry
// export when available and from the bases-only fallback otherwise. Only the
// layer-checkbox test needs the registry, so it skips itself when no layer
// rows rendered (e.g. WASM build absent in the dev environment).
// ---------------------------------------------------------------------------

async function waitForEditor(page) {
    await page.waitForFunction(() => !!window._turiEditor, { timeout: 30_000 });
}

function editorValue(page) {
    // Monaco normalizes inserted text to the model's EOL, which follows the
    // emulated platform (CRLF under the Desktop Chrome Windows UA) -- compare
    // LF-normalized so the assertions are EOL-agnostic.
    return page.evaluate(() => window._turiEditor.getValue().replace(/\r\n/g, '\n'));
}

async function setCode(page, code) {
    await page.evaluate((c) => window._turiEditor.setValue(c), code);
}

async function openLangMenu(page) {
    await page.click('#lang-btn');
    await expect(page.locator('#lang-menu')).toBeVisible();
}

test.describe('language picker', () => {
    test('picking sweet-exp on an empty buffer inserts the header', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);
        await setCode(page, '');

        await openLangMenu(page);
        await page.check('#lang-bases input[value="turmeric/sweet"]');

        expect(await editorValue(page)).toMatch(/^#lang turmeric\/sweet\n/);
        await expect(page.locator('#lang-btn-label')).toHaveText('sweet');
    });

    test('picking S-expression removes the header again', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);
        await setCode(page, '');

        await openLangMenu(page);
        await page.check('#lang-bases input[value="turmeric/sweet"]');
        expect(await editorValue(page)).toMatch(/^#lang turmeric\/sweet\n/);

        // Back to the default: the header (and the blank line the insert
        // added) disappears rather than leaving `#lang turmeric` residue.
        await page.check('#lang-bases input[value="turmeric"]');
        expect(await editorValue(page)).toBe('');
        await expect(page.locator('#lang-btn-label')).toHaveText('s-expr');
    });

    test('replacing the base preserves the rest of the file', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);
        await setCode(page, '#lang turmeric/sweet\n\nprintln "hi"\n');

        await openLangMenu(page);
        await page.check('#lang-bases input[value="turmeric/neoteric"]');

        expect(await editorValue(page)).toBe('#lang turmeric/neoteric\n\nprintln "hi"\n');
    });

    test('typing a header by hand reconciles the picker', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);

        await setCode(page, '#lang turmeric/curly-infix\n(+ 1 2)\n');
        await expect(page.locator('#lang-btn-label')).toHaveText('curly');
        await openLangMenu(page);
        await expect(page.locator('#lang-bases input[value="turmeric/curly-infix"]')).toBeChecked();

        // The legacy alias reads back as the canonical selection.
        await setCode(page, '#lang sweet-exp\n(+ 1 2)\n');
        await expect(page.locator('#lang-btn-label')).toHaveText('sweet');
        await expect(page.locator('#lang-bases input[value="turmeric/sweet"]')).toBeChecked();
    });

    test('one Ctrl+Z undoes a language switch', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);
        const before = '(println "x")\n';
        await setCode(page, before);

        await openLangMenu(page);
        await page.check('#lang-bases input[value="turmeric/sweet"]');
        expect(await editorValue(page)).toBe('#lang turmeric/sweet\n\n' + before);

        await page.evaluate(() => {
            window._turiEditor.focus();
            window._turiEditor.trigger('test', 'undo');
        });
        expect(await editorValue(page)).toBe(before);
    });

    test('the picker follows the active tab', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);
        await setCode(page, '#lang turmeric/sweet\n\nprintln "a"\n');

        // New tab starts empty => default dialect.
        await page.click('.tab-button.tab-new');
        await expect(page.locator('#lang-btn-label')).toHaveText('s-expr');

        // Back to the first tab => sweet again.
        await page.click('.editor-tabs .tab-button:first-child');
        await expect(page.locator('#lang-btn-label')).toHaveText('sweet');
    });

    test('layer checkboxes toggle the trailing token in place', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);

        await openLangMenu(page);
        const layerBoxes = page.locator('#lang-layers input[type=checkbox]');
        if (await layerBoxes.count() === 0) {
            // Bases-only fallback (no WASM registry in this environment).
            test.skip(true, 'lang registry export unavailable; layer rows not rendered');
            return;
        }

        await setCode(page, '#lang turmeric/sweet\n\nprintln "hi"\n');
        const stringed = page.locator('#lang-layers input[value="stringed"]');
        await stringed.check();
        expect(await editorValue(page)).toMatch(/^#lang turmeric\/sweet stringed\n/);

        // Unchecking removes the token -- the layer set is assigned, not
        // accumulated (§2.2 of the plan).
        await stringed.uncheck();
        expect(await editorValue(page)).toMatch(/^#lang turmeric\/sweet\n/);
    });

    test('mobile overflow menu exposes the Language... entry', async ({ page }) => {
        await page.setViewportSize({ width: 480, height: 800 });
        await page.goto('/try/');
        await waitForEditor(page);

        await page.click('#more-btn');
        await page.click('#more-menu [data-action="language"]');
        await expect(page.locator('#lang-menu')).toBeVisible();
        await expect(page.locator('#more-menu')).toBeHidden();
    });

    test('the dialect button is the same height as its neighbours', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);

        // Every other button in the row holds a 16px SVG; this one holds text
        // against .btn's `line-height: 1`, which used to make it 24px against
        // their 28px -- visibly short in a row of otherwise identical buttons.
        const heights = await page.evaluate(() => {
            const h = (sel) => document.querySelector(sel)?.getBoundingClientRect().height ?? null;
            return { run: h('#run-btn'), lang: h('#lang-btn'), examples: h('#examples-btn') };
        });
        expect(heights.lang).toBe(heights.run);
        expect(heights.lang).toBe(heights.examples);
    });

    test('the dialect label is never clipped, at any width or dialect', async ({ page }) => {
        await page.goto('/try/');
        await waitForEditor(page);

        // 620 is above the 600px breakpoint that moves the picker into the
        // overflow menu, so this is the narrowest the button is ever shown at.
        for (const width of [1280, 900, 700, 620]) {
            await page.setViewportSize({ width, height: 800 });
            const clipped = await page.evaluate(() => {
                const btn = document.querySelector('#lang-btn');
                const label = document.querySelector('#lang-btn-label');
                const before = label.textContent;
                const bad = [];
                // The full set baseShortLabel can produce.
                for (const text of ['s-expr', 'curly', 'neoteric', 'sweet']) {
                    label.textContent = text;
                    if (label.scrollWidth > label.clientWidth ||
                        btn.scrollWidth > btn.clientWidth) bad.push(text);
                }
                label.textContent = before;
                return bad;
            });
            expect(clipped, `clipped at ${width}px`).toEqual([]);
        }
    });
});
