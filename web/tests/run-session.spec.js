import { test, expect } from '@playwright/test';
import { submitAtPrompt } from './repl-helpers.js';

/* playground-session-hygiene-plan: what Run means, checked in the page.
 *
 * Run used to append the editor's program to a session that still held the
 * previous run, so a single doc-panel lookup, or simply pressing Run twice on
 * the effects example, left the button failing for the rest of the session.
 * These drive the real page: Run, the doc panel's lookup, tabs, the prompt.
 * The native twins of the session mechanics are tur_wasm_glue_session_unit and
 * tur_session_redefinition; what only the page can show is here -- replayed
 * tabs staying silent, and the button's own rewind. */

async function gotoTry(page) {
    await page.goto('/try/');
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
    await page.waitForFunction(() => !!window._turiTabs && !!window._turiEditor);
}

async function run(page, code) {
    await page.evaluate((c) => window._turiEditor.setValue(c), code);
    await page.evaluate(() => window.turmericApp.runCode());
}

const consoleEl = (page) => page.locator('#console');

async function clearConsole(page) {
    await page.evaluate(() => window.turmericApp.clearConsole());
}

const EFFECTS = `(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))`;

test.describe('Run runs this program', () => {
    test('the effects example runs twice, around a doc-panel lookup', async ({ page }) => {
        await gotoTry(page);

        await run(page, EFFECTS);
        await expect(consoleEl(page)).toContainText('42');
        await expect(consoleEl(page)).not.toContainText('error');

        // The lookup that used to poison the session -- and to find nothing.
        const doc = await page.evaluate(() => window.turmericApp.wasmDocLookup('vec-map'));
        expect(doc).toContain('vec-map -- ');
        expect(doc).toContain('Parameters:');

        await clearConsole(page);
        await run(page, EFFECTS);
        await expect(consoleEl(page)).toContainText('42');
        await expect(consoleEl(page)).not.toContainText('error');
        await expect(consoleEl(page)).not.toContainText('auto-loaded stdlib');
    });

    test('a definition deleted from the program stops resolving', async ({ page }) => {
        await gotoTry(page);

        await run(page, '(defn helper [] : int 7)\n(println (helper))');
        await expect(consoleEl(page)).toContainText('7');

        await clearConsole(page);
        await run(page, '(println (helper))');
        await expect(consoleEl(page)).toContainText("'helper'");
    });

    test("another tab's definitions survive a Run, without re-printing", async ({ page }) => {
        await gotoTry(page);

        await run(page, '(defn shared [] : int 5)\n(println "tab-one-ran")');
        await expect(consoleEl(page)).toContainText('tab-one-ran');

        await page.evaluate(() => window._turiTabs.create());
        await clearConsole(page);
        await run(page, '(println (+ (shared) 1))');
        await expect(consoleEl(page)).toContainText('6');
        await expect(consoleEl(page)).not.toContainText('tab-one-ran');

        // Again: the replayed tab is replayed afresh, not accumulated.
        await clearConsole(page);
        await page.evaluate(() => window.turmericApp.runCode());
        await expect(consoleEl(page)).toContainText('6');
        await expect(consoleEl(page)).not.toContainText('tab-one-ran');
        await expect(consoleEl(page)).not.toContainText('error');
    });

    // -----------------------------------------------------------------------
    // Output that stops mid-line.
    //
    // stdout reaches the page through two line-buffering layers -- libc's FILE
    // buffer and Emscripten's TTY device -- and both hold a last line with no
    // newline behind it. Scheme's `display` writes no newline, so
    // `(display "Hello, world!")` printed NOTHING at all, and then arrived
    // glued to the front of the next run that happened to emit one. The C side
    // flushes on the way out of every eval (wasm_flush_program_output) and the
    // worker assembles the lines itself (eval-worker.js).
    // -----------------------------------------------------------------------

    test('a run whose output has no trailing newline still prints it', async ({ page }) => {
        await gotoTry(page);

        await run(page, '#lang r7rs\n\n(display "Hello, world!")');
        await expect(consoleEl(page)).toContainText('Hello, world!');
    });

    test('a partial line belongs to its own run, not the next one', async ({ page }) => {
        await gotoTry(page);

        await run(page, '#lang r7rs\n\n(display "first")');
        await expect(consoleEl(page)).toContainText('first');

        await clearConsole(page);
        await run(page, '#lang r7rs\n\n(display "second")\n(newline)');
        await expect(consoleEl(page)).toContainText('second');
        await expect(consoleEl(page)).not.toContainText('first');
    });

    test("(doc 'name) at the prompt prints the docstring", async ({ page }) => {
        await gotoTry(page);

        await submitAtPrompt(page, "(doc 'vec-map)");
        await expect(consoleEl(page)).toContainText('vec-map -- apply f');
        await expect(consoleEl(page)).not.toContainText('doc-lookup');
    });
});
