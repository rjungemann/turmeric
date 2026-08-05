import { test, expect } from '@playwright/test';

/**
 * Editor intelligence in Try Turmeric (try-turmeric-lsp-plan, L2-L4).
 *
 * Split into two describes on purpose.
 *
 * The first holds unconditionally, and it is the one that must never break:
 * the playground has worked without any analysis for its entire life, and a
 * language server that fails to boot -- old cached wasm, no SharedArrayBuffer,
 * a worker that throws -- must leave it exactly as usable as it was. That is
 * the contract the shipped bundle is tested against here, because the wasm
 * committed under web/public/ predates the LSP exports.
 *
 * The second runs only when the bundle actually carries them. Skipping is the
 * honest outcome for a stale artifact; asserting nothing at all would not be.
 */

async function waitForReady(page) {
    await expect(page.locator('#wasm-status-text')).toHaveText('Ready', { timeout: 30_000 });
}

async function setCode(page, code) {
    await page.evaluate((c) => window._turiEditor.setValue(c), code);
}

/** Focus the editor, which is what boots the language server, and wait for the
 *  boot to settle either way. Resolves to whether it came up. */
async function bootLsp(page) {
    // Monaco's input is a 1px, transparent textarea, so a Playwright click on
    // it is not actionable. Focus the editor through its own API -- the boot
    // hook listens for onDidFocusEditorText either way.
    await page.evaluate(() => window._turiEditor.focus());
    return page.evaluate(async () => {
        // startLspClient runs on the focus event; give it a turn to install.
        for (let i = 0; i < 100 && !window._turiLsp; i++) {
            await new Promise(r => setTimeout(r, 50));
        }
        if (!window._turiLsp) return false;
        return window._turiLsp.ready;
    });
}

async function markers(page) {
    return page.evaluate(() =>
        (window._turiLsp && window._turiLsp.markers) ? window._turiLsp.markers() : []);
}

// ---------------------------------------------------------------------------
// Holds whether or not the server is available
// ---------------------------------------------------------------------------

test.describe('LSP integration — degradation', () => {
    test('the playground works whether or not analysis boots', async ({ page }) => {
        const jsErrors = [];
        page.on('pageerror', err => jsErrors.push(String(err)));

        await page.goto('/try/');
        await waitForReady(page);
        await bootLsp(page);

        // Evaluation is untouched by anything the language server does or
        // fails to do -- separate worker, separate wasm instance.
        await setCode(page, '(+ 20 22)');
        await page.click('#run-btn');
        await expect(page.locator('#console')).toContainText('42', { timeout: 15_000 });

        expect(jsErrors).toHaveLength(0);
    });

    test('a server that never boots shows no error to the user', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        const available = await bootLsp(page);

        const indicator = page.locator('#lsp-status');
        if (available) {
            await expect(indicator).toBeVisible();
        } else {
            // Not "an error state" -- no state at all. A permanent red dot for
            // a feature the user never asked for reads as a fault.
            await expect(indicator).toBeHidden();
        }
    });

    test('formatting does not depend on the language server', async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        await bootLsp(page);

        await setCode(page, '(defn   f  [ ]  :  int    1)');
        await page.click('#format-btn');
        await page.waitForTimeout(1500);
        const text = await page.evaluate(() => window._turiEditor.getValue());
        expect(text).not.toContain('(defn   f');
    });
});

// ---------------------------------------------------------------------------
// Only meaningful against a bundle built with the LSP exports
// ---------------------------------------------------------------------------

test.describe('LSP integration — providers', () => {
    test.beforeEach(async ({ page }) => {
        await page.goto('/try/');
        await waitForReady(page);
        const available = await bootLsp(page);
        test.skip(!available,
                  'web/public/turmeric.wasm has no LSP exports; rebuild with ' +
                  'cmake --build build --target tur_wasm');
    });

    test('an undefined symbol becomes a marker', async ({ page }) => {
        await setCode(page, '(defn f [] : int (no-such-function 1))\n');
        await expect.poll(() => markers(page), { timeout: 20_000 })
            .not.toHaveLength(0);

        const found = await markers(page);
        expect(found.some(m => /no-such-function/.test(m.message))).toBe(true);
    });

    test('markers clear when the error is fixed', async ({ page }) => {
        await setCode(page, '(defn f [] : int (no-such-function 1))\n');
        await expect.poll(() => markers(page), { timeout: 20_000 })
            .not.toHaveLength(0);

        await setCode(page, '(defn f [] : int 1)\n');
        await expect.poll(() => markers(page), { timeout: 20_000 })
            .toHaveLength(0);
    });

    test('completion offers the buffer\'s own definitions first', async ({ page }) => {
        await setCode(page, '(defn zorkle [x : int] : int x)\n');
        await page.waitForTimeout(600);

        // Type into the real editor and open the real suggest widget: the
        // provider is only half the path, and the half that breaks silently is
        // the other one (ranges, kinds, trigger characters).
        await page.evaluate(() => {
            const ed = window._turiEditor;
            ed.setPosition({ lineNumber: ed.getModel().getLineCount(), column: 1 });
            ed.focus();
        });
        await page.keyboard.type('zork');
        await page.keyboard.press('Control+Space');

        const widget = page.locator('.suggest-widget');
        await expect(widget).toBeVisible({ timeout: 20_000 });
        await expect(widget).toContainText('zorkle');
    });

    test('hover reports a type', async ({ page }) => {
        await setCode(page, '(defn twice [x : int] : int (* x 2))\n(twice 21)\n');
        await page.waitForTimeout(800);

        await page.evaluate(() => {
            window._turiEditor.setPosition({ lineNumber: 2, column: 3 });
        });
        // Monaco shows hover on mouse position, so drive the provider through
        // the editor action rather than simulating a hover that may land on a
        // different glyph in a different font.
        await page.evaluate(() => window._turiEditor.trigger('test', 'editor.action.showHover', {}));
        await expect(page.locator('.monaco-hover')).toContainText('twice', { timeout: 15_000 });
    });

    test('every tab is an open document', async ({ page }) => {
        await page.evaluate(() => {
            window._turiTabs.create({ name: 'helper.tur',
                                      content: '(defn helper [] : int 7)\n',
                                      activate: false });
        });
        await expect.poll(() => page.evaluate(() => window._turiLsp.openDocuments()),
                          { timeout: 20_000 })
            .toBeGreaterThanOrEqual(2);
    });
});

// ---------------------------------------------------------------------------
// The adapter, against a scripted server
//
// The describe above can only run once someone rebuilds the wasm, which means
// ~600 lines of browser code would otherwise ship with no coverage at all
// until then. The server side is already tested in C
// (tests/lsp/session_test.c, tests/lsp/wasm_backend_test.c); what is untested
// is the half in between -- marker application, range conversion, provider
// registration, tab synchronisation.
//
// So: intercept the worker script and serve a scripted one that speaks the
// same protocol with canned answers. Nothing is stubbed on the page side; the
// real lsp-client.js drives the real Monaco providers. Only the language
// server is a script, and it is a script whose answers are copied from what
// the C tests assert the real one produces.
// ---------------------------------------------------------------------------

const SCRIPTED_WORKER = `
self.addEventListener('message', function (e) {
    const msg = e.data;
    if (msg.type === 'init') { self.postMessage({ type: 'ready' }); return; }
    if (msg.type === 'lsp-reset' || msg.type === 'lsp-flush') {
        self.postMessage({ type: 'lsp-result', id: msg.id, messages: [] });
        return;
    }
    if (msg.type !== 'lsp') return;

    const req = JSON.parse(msg.request);
    const p = req.params || {};
    const uri = (p.textDocument && p.textDocument.uri) || '';
    const out = [];
    const respond = (result) => out.push({ jsonrpc: '2.0', id: req.id, result });

    if (req.method === 'initialize') {
        respond({ capabilities: { positionEncoding: 'utf-8' } });

    } else if (req.method === 'textDocument/didOpen' ||
               req.method === 'textDocument/didChange') {
        const text = req.method === 'textDocument/didOpen'
            ? p.textDocument.text
            : p.contentChanges[p.contentChanges.length - 1].text;
        const idx = text.indexOf('no-such-function');
        const diagnostics = [];
        if (idx >= 0) {
            const before = text.slice(0, idx);
            const line = before.split('\\n').length - 1;
            // Byte offset, not code units -- the real server negotiates
            // positionEncoding utf-8, and emulating that is the whole point of
            // the non-ASCII case below.
            const lineStart = before.lastIndexOf('\\n') + 1;
            const col = new TextEncoder().encode(text.slice(lineStart, idx)).length;
            diagnostics.push({
                range: { start: { line, character: col },
                         end: { line, character: col + 16 } },
                severity: 1,
                code: 'TUR-E0001',
                message: "unknown function or operator 'no-such-function'",
            });
        }
        out.push({ jsonrpc: '2.0', method: 'textDocument/publishDiagnostics',
                   params: { uri, diagnostics } });

    } else if (req.method === 'textDocument/completion') {
        respond({ isIncomplete: false, items: [
            { label: 'zorkle', kind: 3, detail: '(fn [int] : int)' },
            { label: 'vec-new', kind: 3, detail: '(fn [] : Vec)' },
        ]});

    } else if (req.method === 'textDocument/hover') {
        respond({ contents: { kind: 'markdown',
                              value: '\`\`\`\\n(twice : (fn [int] : int))\\n\`\`\`' } });

    } else if (req.method === 'textDocument/signatureHelp') {
        respond({ signatures: [{ label: '(cons : (fn [int int] : int))',
                                 parameters: [{ label: 'int' }, { label: 'int' }] }],
                  activeSignature: 0, activeParameter: 1 });

    } else if (req.method === 'textDocument/documentSymbol') {
        respond([{ name: 'twice', kind: 12,
                   range: { start: { line: 0, character: 6 },
                            end: { line: 0, character: 11 } },
                   selectionRange: { start: { line: 0, character: 6 },
                                     end: { line: 0, character: 11 } } }]);

    } else if (req.method === 'textDocument/definition') {
        respond({ uri: 'file:///project/helper.tur',
                  range: { start: { line: 0, character: 6 },
                           end: { line: 0, character: 12 } } });

    } else if (req.id !== undefined) {
        out.push({ jsonrpc: '2.0', id: req.id,
                   error: { code: -32601, message: 'Method not found' } });
    }

    self.postMessage({ type: 'lsp-result', id: msg.id, messages: out });
});
`;

test.describe('LSP integration — adapter against a scripted server', () => {
    test.beforeEach(async ({ page, context }) => {
        // Context-level, not page-level: a dedicated worker's script request
        // is made by the browser, not the page, and page.route does not see it.
        await context.route('**/lsp-worker.js', route => route.fulfill({
            status: 200,
            contentType: 'application/javascript',
            // The page is cross-origin isolated (COOP/COEP are what make
            // SharedArrayBuffer available for the pthreads build), so a
            // synthesised response without CORP is refused as a worker script.
            headers: {
                'Cross-Origin-Resource-Policy': 'same-origin',
                'Cross-Origin-Embedder-Policy': 'require-corp',
            },
            body: SCRIPTED_WORKER,
        }));
        await page.goto('/try/');
        await waitForReady(page);
        const available = await bootLsp(page);
        expect(available).toBe(true);
    });

    test('diagnostics become Monaco markers', async ({ page }) => {
        await setCode(page, '(defn f [] : int (no-such-function 1))\n');

        await expect.poll(() => markers(page), { timeout: 15_000 })
            .not.toHaveLength(0);
        const found = await markers(page);
        expect(found[0].message).toContain('no-such-function');
        // MarkerSeverity.Error === 8 in Monaco; LSP severity 1 must map to it,
        // not be passed through as 1 (which is Hint).
        expect(found[0].severity).toBe(8);
        expect(found[0].startLineNumber).toBe(1);
    });

    test('markers clear when the error goes away', async ({ page }) => {
        await setCode(page, '(defn f [] : int (no-such-function 1))\n');
        await expect.poll(() => markers(page), { timeout: 15_000 })
            .not.toHaveLength(0);

        await setCode(page, '(defn f [] : int 1)\n');
        await expect.poll(() => markers(page), { timeout: 15_000 })
            .toHaveLength(0);
    });

    test('a marker on a line with non-ASCII text lands on the right column',
         async ({ page }) => {
        // The server counts `character` in bytes (it negotiates utf-8);
        // Monaco counts UTF-16 code units. An accented character before the
        // error shifts every byte offset after it, and the failure is silent:
        // the squiggle just sits two characters to the right.
        await setCode(page, '(def café (no-such-function 1))\n');
        await expect.poll(() => markers(page), { timeout: 15_000 })
            .not.toHaveLength(0);

        const found = await markers(page);
        const text = await page.evaluate(() => window._turiEditor.getValue());
        // 1-based UTF-16 column of the offending name. The server reported a
        // *byte* offset; getting the same number back means the conversion ran.
        expect(found[0].startColumn).toBe(text.indexOf('no-such-function') + 1);
    });

    test('completion items reach the suggest widget', async ({ page }) => {
        await setCode(page, '(defn zorkle [x : int] : int x)\n');
        await page.evaluate(() => {
            const ed = window._turiEditor;
            ed.setPosition({ lineNumber: ed.getModel().getLineCount(), column: 1 });
            ed.focus();
        });
        await page.keyboard.type('zork');
        await page.keyboard.press('Control+Space');

        const widget = page.locator('.suggest-widget');
        await expect(widget).toBeVisible({ timeout: 15_000 });
        await expect(widget).toContainText('zorkle');
    });

    test('hover renders the server\'s markdown', async ({ page }) => {
        await setCode(page, '(defn twice [x : int] : int (* x 2))\n(twice 21)\n');
        await page.evaluate(() => {
            window._turiEditor.focus();
            window._turiEditor.setPosition({ lineNumber: 2, column: 3 });
            window._turiEditor.trigger('test', 'editor.action.showHover', {});
        });
        await expect(page.locator('.monaco-hover')).toContainText('twice',
                                                                 { timeout: 15_000 });
    });

    test('every tab is opened as a document, and closing one closes it',
         async ({ page }) => {
        const before = await page.evaluate(() => window._turiLsp.openDocuments());

        const id = await page.evaluate(() => window._turiTabs.create({
            name: 'helper.tur',
            content: '(defn helper [] : int 7)\n',
            activate: false,
        }).id);
        await expect.poll(() => page.evaluate(() => window._turiLsp.openDocuments()),
                          { timeout: 10_000 })
            .toBe(before + 1);

        await page.evaluate((tabId) => window._turiTabs.close(tabId), id);
        await expect.poll(() => page.evaluate(() => window._turiLsp.openDocuments()),
                          { timeout: 10_000 })
            .toBe(before);
    });

    test('go-to-definition switches to the tab that holds the target',
         async ({ page }) => {
        await page.evaluate(() => window._turiTabs.create({
            name: 'helper.tur',
            content: '(defn helper [] : int 7)\n',
            activate: false,
        }));
        await page.waitForTimeout(500);

        await setCode(page, '(helper)\n');
        await page.evaluate(() => {
            window._turiEditor.focus();
            window._turiEditor.setPosition({ lineNumber: 1, column: 3 });
            window._turiEditor.trigger('test', 'editor.action.revealDefinition', {});
        });

        await expect.poll(async () => {
            const activeId = await page.evaluate(() => window._turiTabs.activeId());
            const tabs = await page.evaluate(() => window._turiTabs.tabs());
            const active = tabs.find(t => t.id === activeId);
            return active ? active.name : null;
        }, { timeout: 15_000 }).toBe('helper.tur');
    });
});
