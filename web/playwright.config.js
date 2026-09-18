import { defineConfig, devices } from '@playwright/test';
import { execFileSync } from 'node:child_process';

// Port 3000 is the developer's own dev server (`just web-dev`). The suite must
// never use it: pointed at a running server, it silently tests whatever
// checkout that server was started from, and the developer's session with it.
// So the suite always starts its OWN server, on a free port picked here, and
// never reuses one.
//
// The port is picked once, in the runner, and handed down through the
// environment: every worker re-evaluates this file, and a fresh random pick
// there would disagree with the server the runner started. Set TRY_TEST_PORT
// to pin it (any free port except 3000).
if (!process.env.TRY_TEST_PORT) {
    process.env.TRY_TEST_PORT = execFileSync(process.execPath, [
        '-e',
        "const s = require('net').createServer();" +
        "s.listen(0, '127.0.0.1', () => { process.stdout.write(String(s.address().port)); s.close(); });",
    ]).toString().trim();
}
const PORT = process.env.TRY_TEST_PORT;
if (PORT === '3000') {
    throw new Error('TRY_TEST_PORT=3000 is the developer dev server; pick any other port');
}
const ORIGIN = `http://localhost:${PORT}`;

export default defineConfig({
    testDir: './tests',
    timeout: 60_000,
    use: {
        baseURL: ORIGIN,
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
        command: `npm run dev -- --port ${PORT} --strictPort`,
        url: ORIGIN,
        reuseExistingServer: false,
        timeout: 30_000,
    },
});
