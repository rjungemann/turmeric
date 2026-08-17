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
});
