import { test, expect } from '@playwright/test';

/**
 * The editor's action row scrolls when its buttons do not fit.
 *
 * The row has carried `overflow-x: auto` and a drag-scroll handler since it
 * shipped, so it *looked* scrollable; it was not. `justify-content: flex-end`
 * is unsafe alignment, and the overflow it produces sits past the row's start
 * edge, where a scroll container cannot reach it -- `scrollWidth` stays equal
 * to `clientWidth` and `scrollLeft` is pinned at 0. The visible symptom was
 * Run and Trace simply disappearing off the left of a narrow editor pane with
 * no way to get them back.
 *
 * So the assertions are about reachability, not about the CSS: every button
 * must be scrollable into view, and the row must still sit right-aligned when
 * everything fits (which is why `flex-end` was there in the first place).
 */

async function waitForReady(page) {
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

/**
 * Narrow the *editor pane*, not the viewport: the case this exists for is a
 * desktop window whose pane is small, which no media query can see. (Below
 * 600px the row is not the mechanism -- most buttons move into the ... menu.)
 */
async function narrowTheEditorPane(page, fraction) {
    await page.evaluate((f) => {
        document.querySelector('.repl-container').style.setProperty('--split-h', String(f));
        window._turiEditor && window._turiEditor.layout();
    }, fraction);
    await page.waitForTimeout(300);
}

test.describe('Editor action row overflow', () => {
    test.beforeEach(async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
    });

    test('sits right-aligned while the buttons fit', async ({ page }) => {
        const fit = await page.evaluate(() => {
            const el = document.querySelector('.editor-actions');
            // The offscreen file input occupies no row space; skip it.
            const shown = [...el.children].filter(
                c => c.getBoundingClientRect().width > 1 && c.id !== 'open-project-input');
            const box = el.getBoundingClientRect();
            const last = shown.slice(-1)[0].getBoundingClientRect();
            return {
                overflows: el.scrollWidth > el.clientWidth + 1,
                gapAtEnd: Math.round(box.right - last.right),
            };
        });

        expect(fit.overflows).toBe(false);
        // Flush against the end, modulo the row's 8px padding.
        expect(fit.gapAtEnd).toBeLessThanOrEqual(9);
    });

    test('scrolls, and reaches both ends, when the buttons do not fit',
         async ({ page }) => {
        await narrowTheEditorPane(page, 0.28);

        const state = await page.evaluate(() => {
            const el = document.querySelector('.editor-actions');
            const shown = () => [...el.children].filter(
                c => c.getBoundingClientRect().width > 1 && c.id !== 'open-project-input');
            const box = () => el.getBoundingClientRect();

            el.scrollLeft = 0;
            const firstReachable =
                shown()[0].getBoundingClientRect().left >= box().left - 1;

            el.scrollLeft = 99999;
            const maxScroll = el.scrollLeft;
            const lastReachable =
                shown().slice(-1)[0].getBoundingClientRect().right <= box().right + 1;

            el.scrollLeft = 0;
            return {
                overflows: el.scrollWidth > el.clientWidth + 1,
                maxScroll,
                firstReachable,
                lastReachable,
                hasOverflowClass: el.classList.contains('has-overflow'),
            };
        });

        // The premise: this pane really is too narrow for the row.
        expect(state.overflows).toBe(true);
        // The bug: overflow existed but `scrollLeft` could not leave 0.
        expect(state.maxScroll).toBeGreaterThan(0);
        expect(state.firstReachable).toBe(true);
        expect(state.lastReachable).toBe(true);
        // The affordance -- the edge fade that says "this row moves".
        expect(state.hasOverflowClass).toBe(true);
    });

    test('buttons keep their size instead of compressing', async ({ page }) => {
        const runWidth = () => page.evaluate(
            () => Math.round(document.querySelector('#run-btn').getBoundingClientRect().width));

        const wide = await runWidth();
        await narrowTheEditorPane(page, 0.28);
        const narrow = await runWidth();

        // A squashed row that fits is not a fix; it is the same information
        // loss with the icons clipped instead of the buttons.
        expect(narrow).toBe(wide);
    });
});
