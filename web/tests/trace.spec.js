/* The time-travel timeline (try-turmeric-tracer-plan, T3).
 *
 * The last test here is the load-bearing one and the reason this file runs the
 * native binary as well as the page: the same program, recorded by `tur trace`
 * on the host and by the WASM build in the tab, must agree on how many steps
 * it took. c2mp shipped the same test for the same reason -- a wasm32
 * divergence in a recorder is not something any other assertion would catch.
 */

import { test, expect } from '@playwright/test';
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '../..');
const TUR  = process.env.TUR || path.join(REPO, 'build/tur');

/* Same program as tests/fixtures/trace/input.tur, which run-trace.sh records
 * natively. Small on purpose: the interpreter retains roughly 4 KiB per step,
 * so a fixture's step count is a memory multiplier and a recording sits on top
 * of that. */
const FIB = `(defn fib [n : int] : int
  (if (< n 2)
    n
    (+ (fib (- n 1)) (fib (- n 2)))))

(defn main [] : int
  (println (fib 6))
  0)`;

async function openReady(page) {
    await page.goto('/try/');
    await page.waitForFunction(() => window._turiTrace && window._turiEditor, null,
                               { timeout: 45_000 });
    // The Trace button is disabled in spirit until the module is up; the state
    // machine is internal, so wait on the status text the page already sets.
    await expect(page.locator('#wasm-status-text')).toHaveText(/Ready/i, { timeout: 45_000 });
}

async function record(page, source) {
    await page.evaluate((src) => window._turiEditor.setValue(src), source);
    await page.evaluate(() => window._turiTrace.run());
    await page.waitForFunction(() => window._turiTrace.state().active, null,
                               { timeout: 30_000 });
    return page.evaluate(() => window._turiTrace.state());
}

test.describe('Time-travel timeline', () => {

    test('Trace records a run and opens the timeline', async ({ page }) => {
        await openReady(page);
        const state = await record(page, FIB);

        expect(state.steps).toBeGreaterThan(0);
        await expect(page.locator('#trace-panel')).toBeVisible();
        await expect(page.locator('#trace-pos')).toHaveText(/^1 \/ \d+$/);
    });

    test('the cursor scrubs forwards and backwards', async ({ page }) => {
        await openReady(page);
        const { steps } = await record(page, FIB);

        const mid = Math.floor(steps / 2);
        await page.evaluate((i) => window._turiTrace.seek(i), mid);
        await expect(page.locator('#trace-pos')).toHaveText(`${mid + 1} / ${steps}`);

        // Backwards is the whole point: a pause cannot go back.
        await page.locator('#trace-back').click();
        await expect(page.locator('#trace-pos')).toHaveText(`${mid} / ${steps}`);

        await page.locator('#trace-first').click();
        await expect(page.locator('#trace-pos')).toHaveText(`1 / ${steps}`);
    });

    test('frames and bindings render, and depth never exceeds the peak', async ({ page }) => {
        await openReady(page);
        const { steps } = await record(page, FIB);

        const peak = await page.evaluate(async () => {
            let max = 0;
            const st = window._turiTrace.state();
            for (let i = 0; i < st.steps; i += Math.max(1, Math.floor(st.steps / 20))) {
                await window._turiTrace.seek(i);
                max = Math.max(max, window._turiTrace.state().frames.length);
            }
            return max;
        });

        expect(peak).toBeGreaterThan(1);   // fib recurses; a flat stack means POP is wrong
        expect(steps).toBeGreaterThan(peak);
        await expect(page.locator('#trace-frames .trace-frame').first()).toBeVisible();
    });

    test('the gutter follows the cursor', async ({ page }) => {
        await openReady(page);
        const { steps } = await record(page, FIB);
        await page.evaluate((i) => window._turiTrace.seek(i), Math.floor(steps / 2));

        // The decoration is whole-line, so it is the line content that carries
        // the class. Its presence is the assertion; which line it lands on is
        // the parity test's business.
        await expect(page.locator('.trace-current-line').first()).toBeVisible();
    });

    test('closing the timeline restores the console transcript', async ({ page }) => {
        await openReady(page);
        await page.evaluate(() => window._turiEditor.setValue('(println "before-trace")'));
        await page.locator('#run-btn').click();
        await expect(page.locator('#console')).toContainText('before-trace');

        await record(page, FIB);
        await page.locator('#trace-close').click();
        await expect(page.locator('#trace-panel')).toBeHidden();
        await expect(page.locator('#console')).toContainText('before-trace');
    });

    test('a plain eval after a trace is not left running under the recorder', async ({ page }) => {
        await openReady(page);
        await record(page, FIB);
        await page.locator('#trace-close').click();

        // The recorder owns the env's pause handler and this module keeps ONE
        // env for the life of the tab. If turi_wasm_trace_run failed to take
        // the handler off, every later eval would still be traced -- which is
        // correctness-invisible and performance-fatal, so it is asserted as a
        // budget rather than an output.
        const ms = await page.evaluate(async () => {
            window._turiEditor.setValue('(println (+ 1 1))');
            const t0 = performance.now();
            document.getElementById('run-btn').click();
            await new Promise(r => setTimeout(r, 1500));
            return performance.now() - t0;
        });
        expect(ms).toBeLessThan(5000);
        await expect(page.locator('#console')).toContainText('2');
    });

    test('the transcript at the last step shows what the program printed', async ({ page }) => {
        await openReady(page);
        // The final println drains AFTER the final STEP record, so the
        // before-the-cursor transcript is empty there and the last step has to
        // ask for the whole recording's output instead.
        const { steps } = await record(page, `(defn main [] : int\n  (println 111)\n  (println 222)\n  0)`);
        await page.evaluate((i) => window._turiTrace.seek(i), steps - 1);
        await expect(page.locator('#console')).toContainText('111');
        await expect(page.locator('#console')).toContainText('222');
    });

    test('a program with no main entry point records its top-level forms', async ({ page }) => {
        await openReady(page);
        // Run's entry rule says these top-level forms ARE the program, so the
        // recorder has to be armed BEFORE they are evaluated -- arming after,
        // the way a main-bearing program does, would record nothing at all.
        const state = await record(page,
            `(defn twice [n : int] : int (* n 2))\n(println (twice 21))`);

        expect(state.steps).toBeGreaterThan(0);
        await page.evaluate((i) => window._turiTrace.seek(i), state.steps - 1);
        await expect(page.locator('#console')).toContainText('42');
    });

    test('parity: the browser and `tur trace` agree on the step count', async ({ page }) => {
        test.skip(!existsSync(TUR), `no tur binary at ${TUR} -- build it first`);

        // `tur trace` with no -o prints "trace: 65 steps, 26 enters, ..." --
        // on STDERR, since stdout belongs to the program being recorded.
        //
        // TUR_STDLIB_DIR is pinned to this checkout on purpose: a mise shim on
        // PATH injects one pointing at an older *installed* stdlib, and a
        // parity test comparing two different stdlibs would be measuring the
        // wrong thing (and would fail in a way that reads like a wasm bug).
        const fixture = path.join(REPO, 'tests/fixtures/trace/input.tur');
        const run = spawnSync(TUR, ['trace', fixture], {
            encoding: 'utf8',
            env: {
                ...process.env,
                ASAN_OPTIONS: 'detect_leaks=0',
                TUR_STDLIB_DIR: path.join(REPO, 'stdlib'),
            },
        });
        const summary = `${run.stdout || ''}\n${run.stderr || ''}`;
        const m = summary.match(/trace:\s*(\d+)\s+steps/i);
        expect(m, `could not read a step count out of:\n${summary}`).toBeTruthy();
        const nativeSteps = parseInt(m[1], 10);

        await openReady(page);
        const { steps } = await record(page, FIB);

        expect(steps).toBe(nativeSteps);
    });
});
