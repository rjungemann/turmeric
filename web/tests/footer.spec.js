import { test, expect } from '@playwright/test';

/**
 * The C-interpreter link (try-turmeric-navigation-and-minimap-plan, F1).
 *
 * There are two footers on this site, not one. The marketing pages place
 * `<site-footer>`, a web component whose markup lives in site.js; `/try` has
 * its own inline footer and never places that element. A link added to one and
 * not the other is missing from exactly the page where someone already in a
 * playground would look for the other playground.
 *
 * Worth a spec of its own because nothing else here would catch it: a link
 * inside a custom element that silently stops rendering -- a thrown
 * connectedCallback, a customElements.define that never ran -- takes the whole
 * column with it and every other test still passes. Deliberately not folded
 * into deploy-gate.spec.js, which is kept to the one-thing-you-must-not-break
 * contract.
 */

const C_INTERPRETER = 'https://c.turmeric-lang.com';

test.describe('C interpreter link', () => {
    test('is in the site footer on a marketing page', async ({ page }) => {
        await page.goto('/');
        const link = page.locator(`site-footer a[href="${C_INTERPRETER}"]`);
        await expect(link).toHaveCount(1);
        await expect(link).toBeVisible();
        // Same treatment as the sibling Spices link: a plain href to a
        // same-project subdomain, no target and no rel to justify.
        await expect(link).not.toHaveAttribute('target', /.*/);
    });

    test('sits in the Ecosystem column beside Spices', async ({ page }) => {
        await page.goto('/');
        // The two links disagreeing about which column they live in is how
        // "the site says the ecosystem is these four things" stops being true.
        const column = page.locator('site-footer .footer-col', {
            has: page.locator('.footer-col-title', { hasText: 'Ecosystem' }),
        });
        await expect(column.locator(`a[href="${C_INTERPRETER}"]`)).toHaveCount(1);
        await expect(column.locator('a[href*="spices."]')).toHaveCount(1);
    });

    test('is in the /try footer, which is a different footer', async ({ page }) => {
        await page.goto('/try/');
        const link = page.locator(`.footer a[href="${C_INTERPRETER}"]`);
        await expect(link).toHaveCount(1);
        await expect(link).toBeVisible();
    });

    test('is in the sidebar the docs pages use', async ({ page }) => {
        // SiteSidebar mirrors the footer's three groups. If the two lists
        // drift, one of them is telling a visitor the C interpreter does not
        // exist. Rendered here through the component rather than hunting for
        // a page that happens to place it.
        await page.goto('/');
        const hrefs = await page.evaluate(() => {
            const el = document.createElement('site-sidebar');
            document.body.appendChild(el);
            const found = Array.from(el.querySelectorAll('a')).map(a => a.getAttribute('href'));
            el.remove();
            return found;
        });
        expect(hrefs).toContain(C_INTERPRETER);
    });
});
