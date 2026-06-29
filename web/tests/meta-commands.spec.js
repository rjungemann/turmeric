import { test, expect } from '@playwright/test';

async function waitForReady(page) {
    page.on('console', msg => console.log(`PAGE CONSOLE: ${msg.text()}`));
    page.on('pageerror', err => console.log(`PAGE ERROR: ${err.stack}`));
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

test.describe('Try Turmeric REPL meta-commands', () => {
    test('handles :help locally and displays web commands help', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        const replInput = page.locator('#repl-input');
        await replInput.click();
        await replInput.fill(':help');
        await replInput.press('Enter');

        await expect(page.locator('#console')).toContainText('Turmeric REPL Help', { timeout: 10_000 });
        await expect(page.locator('#console')).toContainText(':type <expr>', { timeout: 10_000 });
        await expect(page.locator('#console')).toContainText(':doc  <sym>', { timeout: 10_000 });
        await expect(page.locator('#console')).toContainText(':explain [code]', { timeout: 10_000 });
    });

    test('handles :doc locally and displays docstrings', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        const replInput = page.locator('#repl-input');
        await replInput.click();
        await replInput.fill(':doc +');
        await replInput.press('Enter');

        await expect(page.locator('#console')).toContainText('(+ a b ...) -- add numbers', { timeout: 10_000 });
    });

    test('handles :type locally and displays type', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        const replInput = page.locator('#repl-input');
        await replInput.click();
        await replInput.fill(':type 42');
        await replInput.press('Enter');

        await expect(page.locator('#console')).toContainText(': int', { timeout: 10_000 });
    });

    test('handles :explain locally with code and from history', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        const replInput = page.locator('#repl-input');
        await replInput.click();
        
        // 1. Specific explain
        await replInput.fill(':explain TUR-E0001');
        await replInput.press('Enter');
        await expect(page.locator('#console')).toContainText('Type mismatch', { timeout: 10_000 });

        // 2. Clear console
        await page.click('#clear-console-btn');
        await expect(page.locator('#console')).not.toContainText('Type mismatch', { timeout: 10_000 });

        // 3. Trigger error in REPL to populate last diagnostic code
        await replInput.fill('(defn f [] invalid-name)');
        await replInput.press('Enter');
        await expect(page.locator('#console')).toContainText('unbound symbol', { timeout: 10_000 });

        // 4. Bare explain
        await replInput.fill(':explain');
        await replInput.press('Enter');
        await expect(page.locator('#console')).toContainText('Unbound symbol', { timeout: 10_000 });
    });

    test('handles :nonsense with unknown meta-command', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);

        const replInput = page.locator('#repl-input');
        await replInput.click();
        await replInput.fill(':nonsense');
        await replInput.press('Enter');

        await expect(page.locator('#console')).toContainText("unknown meta-command ':nonsense'", { timeout: 10_000 });
    });
});
