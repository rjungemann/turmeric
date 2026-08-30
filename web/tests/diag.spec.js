import { test, expect } from '@playwright/test';
import { submitAtPrompt } from './repl-helpers.js';

async function waitForReady(page) {
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

async function rawEval(page, code) {
    // Set code and run, return full console text after result appears
    await page.evaluate((c) => window._turiEditor.setValue(c), code);
    const before = await page.locator('#console').textContent();
    await page.click('#run-btn');
    await page.waitForFunction(
        (prev) => document.getElementById('console').textContent !== prev,
        before,
        { timeout: 10_000 }
    );
    // Wait a bit more for async eval to finish
    await page.waitForTimeout(2000);
    return page.locator('#console').textContent();
}

test('diagnostic — defn then call same eval', async ({ page }) => {
    await page.goto('/try/');
    await waitForReady(page);
    const text = await rawEval(page, '(defn add [a :int b :int] :int (+ a b))\n(add 3 4)');
    console.log('SAME-EVAL RESULT:', JSON.stringify(text));
    // Don't assert — just capture
});

test('diagnostic — defn in editor, call in REPL', async ({ page }) => {
    await page.goto('/try/');
    await waitForReady(page);
    await page.evaluate((c) => window._turiEditor.setValue(c), '(defn add [a :int b :int] :int (+ a b))');
    await page.click('#run-btn');
    await page.waitForTimeout(1000);
    await submitAtPrompt(page, '(add 3 4)');
    await page.waitForTimeout(2000);
    const text = await page.locator('#console').textContent();
    console.log('CROSS-EVAL REPL RESULT:', JSON.stringify(text));
});

test('diagnostic — factorial same eval', async ({ page }) => {
    await page.goto('/try/');
    await waitForReady(page);
    const text = await rawEval(page, [
        '(defn factorial [n :int] :int',
        '  (if (<= n 1) 1 (* n (factorial (- n 1)))))',
        '(factorial 10)',
    ].join('\n'));
    console.log('FACTORIAL RESULT:', JSON.stringify(text));
});

test('diagnostic — case form', async ({ page }) => {
    await page.goto('/try/');
    await waitForReady(page);
    const text = await rawEval(page, [
        '(defn factorial [n :int] :int',
        '  (case n',
        '    0 1',
        '    1 1',
        '    :else (* n (factorial (- n 1)))))',
        '(factorial 5)',
    ].join('\n'));
    console.log('CASE RESULT:', JSON.stringify(text));
});

test('diagnostic — effects with parens', async ({ page }) => {
    await page.goto('/try/');
    await waitForReady(page);
    const text = await rawEval(page, [
        '(defeffect Ask [] :int)',
        '(defn use-ask [] :int',
        '  (+ 1 (perform (Ask))))',
        '(println (handle (use-ask)',
        '  (Ask [] k) (resume k 41)))',
    ].join('\n'));
    console.log('EFFECTS RESULT:', JSON.stringify(text));
});

test('effects diagnostic — Ask perform/resume prints 42', async ({ page }) => {
    await page.goto('/try/');
    await waitForReady(page);
    await rawEval(page, [
        '(defeffect Ask [] :int)',
        '(defn use-ask [] :int',
        '  (+ 1 (perform (Ask))))',
        '(println (handle (use-ask)',
        '  (Ask [] k) (resume k 41)))',
    ].join('\n'));
    await expect(page.locator('#console')).toContainText('42', { timeout: 10_000 });
});

test('diagnostic — math operators', async ({ page }) => {
    await page.goto('/try/');
    await waitForReady(page);
    const text = await rawEval(page, '(+ 1 2 3)\n(- 10 5)\n(* 2 3 4)\n(/ 100 4)');
    console.log('MATH RESULT:', JSON.stringify(text));
});
