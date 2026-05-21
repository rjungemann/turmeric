import { test, expect } from '@playwright/test';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const guideDir = path.resolve(__dirname, '../../docs/html/guides');

// Load a pre-rendered guide HTML file directly via file:// URL.
function guideUrl(stem) {
    return `file://${path.join(guideDir, `${stem}.html`)}`;
}

// ---------------------------------------------------------------------------
// Toggle visibility
// ---------------------------------------------------------------------------

test('guide toggle: clicking sweet-exp shows sweet-exp block and hides turmeric', async ({ page }) => {
    await page.goto(guideUrl('quickstart'));

    // Find the first toggle card
    const card = page.locator('.code-card.code-toggle').first();
    await expect(card).toBeVisible();

    // turmeric version is visible by default
    const turVersion = card.locator('.turmeric-version');
    const sweetVersion = card.locator('.sweet-exp-version');
    await expect(turVersion).toBeVisible();
    await expect(sweetVersion).toBeHidden();

    // Click the sweet-exp button
    await card.locator('.seg-btn[data-syntax="sweet-exp"]').click();

    // Now sweet-exp is visible, turmeric is hidden
    await expect(sweetVersion).toBeVisible();
    await expect(turVersion).toBeHidden();
});

test('guide toggle: clicking turmeric restores turmeric block', async ({ page }) => {
    await page.goto(guideUrl('quickstart'));

    const card = page.locator('.code-card.code-toggle').first();
    const turVersion = card.locator('.turmeric-version');
    const sweetVersion = card.locator('.sweet-exp-version');

    await card.locator('.seg-btn[data-syntax="sweet-exp"]').click();
    await expect(sweetVersion).toBeVisible();
    await card.locator('.seg-btn[data-syntax="turmeric"]').click();
    await expect(turVersion).toBeVisible();
    await expect(sweetVersion).toBeHidden();
});

// ---------------------------------------------------------------------------
// Active button state
// ---------------------------------------------------------------------------

test('guide toggle: active button updates on switch', async ({ page }) => {
    await page.goto(guideUrl('quickstart'));

    const card = page.locator('.code-card.code-toggle').first();
    const turBtn = card.locator('.seg-btn[data-syntax="turmeric"]');
    const sweetBtn = card.locator('.seg-btn[data-syntax="sweet-exp"]');

    await expect(turBtn).toHaveClass(/active/);
    await expect(sweetBtn).not.toHaveClass(/active/);

    await sweetBtn.click();
    await expect(sweetBtn).toHaveClass(/active/);
    await expect(turBtn).not.toHaveClass(/active/);
});

// ---------------------------------------------------------------------------
// ARIA attributes (ST5.2)
// ---------------------------------------------------------------------------

test('guide toggle: aria-selected reflects active tab', async ({ page }) => {
    await page.goto(guideUrl('quickstart'));

    const card = page.locator('.code-card.code-toggle').first();
    const turBtn = card.locator('.seg-btn[data-syntax="turmeric"]');
    const sweetBtn = card.locator('.seg-btn[data-syntax="sweet-exp"]');

    await expect(turBtn).toHaveAttribute('aria-selected', 'true');
    await expect(sweetBtn).toHaveAttribute('aria-selected', 'false');

    await sweetBtn.click();
    await expect(sweetBtn).toHaveAttribute('aria-selected', 'true');
    await expect(turBtn).toHaveAttribute('aria-selected', 'false');
});

// ---------------------------------------------------------------------------
// Sticky preference (ST1.5)
// ---------------------------------------------------------------------------

test('guide toggle: preference persists to a second card on the same page', async ({ page }) => {
    await page.goto(guideUrl('quickstart'));

    // Switch the first card to sweet-exp
    const firstCard = page.locator('.code-card.code-toggle').first();
    await firstCard.locator('.seg-btn[data-syntax="sweet-exp"]').click();

    // The second card should also have switched (page-wide sync)
    const secondCard = page.locator('.code-card.code-toggle').nth(1);
    await expect(secondCard.locator('.sweet-exp-version')).toBeVisible();
    await expect(secondCard.locator('.turmeric-version')).toBeHidden();
});

test('guide toggle: localStorage restores preference on page reload', async ({ page }) => {
    await page.goto(guideUrl('quickstart'));

    // Switch to sweet-exp
    await page.locator('.code-card.code-toggle').first()
        .locator('.seg-btn[data-syntax="sweet-exp"]').click();

    // Reload and check the preference was restored
    await page.reload();
    const card = page.locator('.code-card.code-toggle').first();
    await expect(card.locator('.sweet-exp-version')).toBeVisible();
    await expect(card.locator('.turmeric-version')).toBeHidden();
});

// ---------------------------------------------------------------------------
// effects-system-guide pilot
// ---------------------------------------------------------------------------

test('effects-system-guide toggle works', async ({ page }) => {
    await page.goto(guideUrl('effects-system-guide'));

    const card = page.locator('.code-card.code-toggle').first();
    await expect(card.locator('.turmeric-version')).toBeVisible();
    await card.locator('.seg-btn[data-syntax="sweet-exp"]').click();
    await expect(card.locator('.sweet-exp-version')).toBeVisible();
    await expect(card.locator('.turmeric-version')).toBeHidden();
});
