/* Shared prompt helpers.
 *
 * The Try Turmeric prompt is a single-line Monaco editor, not an <input>
 * (W1) -- so `fill()` has nothing to fill. Typing goes through the model and
 * Enter goes through the keyboard, which is the split that matters: the model
 * write is a convenience, and the key is the thing under test.
 */

/** Focus the prompt and put `text` in it, without submitting. */
export async function typeAtPrompt(page, text) {
    await page.locator('#repl-input').click();
    await page.evaluate((t) => window._turiRepl.setValue(t), text);
}

/** Type `text` and submit it with a real Enter keypress. */
export async function submitAtPrompt(page, text) {
    await typeAtPrompt(page, text);
    await page.keyboard.press('Enter');
}
