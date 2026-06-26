import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
    testDir: './tests',
    timeout: 60_000,
    use: {
        baseURL: 'http://localhost:3000',
    },
    projects: [
        {
            name: 'desktop',
            use: { ...devices['Desktop Chrome'] },
            testIgnore: /mobile\..*\.spec\.js$/,
        },
        {
            name: 'mobile',
            use: { ...devices['iPhone 13'] },
            testMatch: /mobile\..*\.spec\.js$/,
        },
    ],
    webServer: {
        command: 'npm run dev',
        url: 'http://localhost:3000',
        reuseExistingServer: true,
        timeout: 30_000,
    },
});
