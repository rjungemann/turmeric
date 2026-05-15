import { defineConfig } from '@playwright/test';

/**
 * Playwright config for production smoke tests.
 * Targets the live turmeric-lang.com and try.turmeric-lang.com domains.
 * No webServer — tests hit real prod, so keep timeouts generous.
 *
 * Run with:
 *   npm run test:prod
 */
export default defineConfig({
    testDir: './tests',
    testMatch: '**/prod-smoke.spec.js',
    timeout: 15_000,
    retries: 1,
    use: {
        // No baseURL — tests use absolute prod URLs throughout.
        // Helps make each test self-documenting about which domain it hits.
        ignoreHTTPSErrors: false,
        actionTimeout: 15_000,
        navigationTimeout: 15_000,
    },
    reporter: [['list'], ['html', { open: 'never' }]],
});
