/**
 * lsp-client.js — a Monaco adapter for the Turmeric language server.
 *
 * The server on the other end of this is `tur lsp`, unmodified, running inside
 * the WASM bundle (see src/web/wasm_lsp.c). Nothing here implements language
 * intelligence; it translates between Monaco's provider APIs and JSON-RPC.
 *
 * Hand-rolled rather than `monaco-languageclient`, which drags in
 * vscode-jsonrpc, vscode-languageclient, and a `vscode` shim to bridge six
 * methods whose Monaco-side providers are each a dozen lines. Revisit if the
 * method count grows past roughly a dozen.
 *
 * Degradation is the load-bearing property: if the worker never boots, or the
 * bundle predates the LSP exports, every provider returns empty and markers
 * stay clear. The playground must never become *less* usable than it was
 * because analysis failed.
 */

// LSP CompletionItemKind (1-25) -> Monaco. The two enums do not agree on
// numbering, so this is a table rather than a cast.
const COMPLETION_KIND_NAMES = [
    null, 'Text', 'Method', 'Function', 'Constructor', 'Field', 'Variable',
    'Class', 'Interface', 'Module', 'Property', 'Unit', 'Value', 'Enum',
    'Keyword', 'Snippet', 'Color', 'File', 'Reference', 'Folder', 'EnumMember',
    'Constant', 'Struct', 'Event', 'Operator', 'TypeParameter',
];

// LSP SymbolKind (1-26) -> Monaco. Same story.
const SYMBOL_KIND_NAMES = [
    null, 'File', 'Module', 'Namespace', 'Package', 'Class', 'Method',
    'Property', 'Field', 'Constructor', 'Enum', 'Interface', 'Function',
    'Variable', 'Constant', 'String', 'Number', 'Boolean', 'Array', 'Object',
    'Key', 'Null', 'EnumMember', 'Struct', 'Event', 'Operator',
    'TypeParameter',
];

const DEFAULT_DEBOUNCE_MS = 150;

const encoder = new TextEncoder();

/**
 * The server negotiates `positionEncoding: "utf-8"`, so a `character` on the
 * wire is a **byte** offset into the line. Monaco counts UTF-16 code units.
 * They agree for ASCII and diverge for everything else, and the divergence is
 * silent: an accented character earlier in the line shifts every position
 * after it, so a hover lands on the wrong word and a diagnostic underlines the
 * wrong span. Convert rather than hope.
 */
const NON_ASCII = /[^\x00-\x7F]/;

function utf16ColumnToUtf8(lineText, column /* 1-based UTF-16 */) {
    const prefix = lineText.slice(0, column - 1);
    // Fast path: an all-ASCII prefix has byte length === code-unit length.
    if (!NON_ASCII.test(prefix)) return prefix.length;
    return encoder.encode(prefix).length;
}

function utf8ColumnToUtf16(lineText, byteOffset) {
    if (!NON_ASCII.test(lineText)) {
        return Math.min(byteOffset, lineText.length) + 1;
    }
    let bytes = 0;
    for (let i = 0; i < lineText.length; i++) {
        if (bytes >= byteOffset) return i + 1;
        const code = lineText.codePointAt(i);
        if (code > 0xffff) {
            bytes += 4;
            i++;  // surrogate pair consumes two code units
        } else if (code > 0x7ff) {
            bytes += 3;
        } else if (code > 0x7f) {
            bytes += 2;
        } else {
            bytes += 1;
        }
    }
    return lineText.length + 1;
}

function toLspPosition(model, position) {
    const lineText = model.getLineContent(position.lineNumber);
    return {
        line: position.lineNumber - 1,
        character: utf16ColumnToUtf8(lineText, position.column),
    };
}

function fromLspRange(model, range) {
    const startLine = Math.min(range.start.line + 1, model.getLineCount());
    const endLine = Math.min(range.end.line + 1, model.getLineCount());
    return {
        startLineNumber: startLine,
        startColumn: utf8ColumnToUtf16(model.getLineContent(startLine),
                                       range.start.character),
        endLineNumber: endLine,
        endColumn: utf8ColumnToUtf16(model.getLineContent(endLine),
                                     range.end.character),
    };
}

/**
 * Create the adapter. Nothing is instantiated until start() is called — the
 * second WASM instance roughly doubles the playground's wasm memory, and a
 * visitor who only reads code should not pay for it.
 *
 * @param {object}   opts
 * @param {object}   opts.monaco        the Monaco namespace
 * @param {string}   opts.languageId    language id to register providers for
 * @param {Function} opts.getTabs       () => [{ id, name, _model }]
 * @param {Function} [opts.onNavigate]  (tab, monacoRange) => void — go-to-def
 *                                      landing in a different tab
 * @param {Function} [opts.onOpenExternal] (lspUri) => Promise<tab|null> —
 *                                      go-to-def landing outside the tab
 *                                      strip; the host opens a read-only
 *                                      buffer and returns its tab, or null
 * @param {Function} [opts.onBeforeNavigate] (model, position) => void —
 *                                      called with the origin of a jump that
 *                                      is about to happen, for a back stack
 * @param {Function} [opts.onStatus]    (state) => void — 'booting' | 'idle' |
 *                                      'analyzing' | 'unavailable'
 * @param {Function} [opts.lookupDoc]   (name) => {summary, kind} | null — the
 *                                      documentation table, consulted only
 *                                      when the server has nothing to say
 * @param {number}   [opts.debounceMs]  trailing debounce on didChange
 */
export function createLspClient(opts) {
    const monaco = opts.monaco;
    const languageId = opts.languageId || 'turmeric';
    const getTabs = opts.getTabs;
    const onNavigate = opts.onNavigate || function () {};
    const onOpenExternal = opts.onOpenExternal || function () { return null; };
    const onBeforeNavigate = opts.onBeforeNavigate || function () {};
    const onStatus = opts.onStatus || function () {};
    const lookupDoc = opts.lookupDoc || function () { return null; };
    const debounceMs = typeof opts.debounceMs === 'number'
        ? opts.debounceMs : DEFAULT_DEBOUNCE_MS;

    let worker = null;
    let startPromise = null;
    let available = false;
    let disposed = false;
    let nextId = 1;
    let inFlight = 0;

    const pending = new Map();          // request id -> {resolve, reject}
    const openDocs = new Map();         // lsp uri -> { tabId, version, listener }
    const dirtyTimers = new Map();      // lsp uri -> timeout handle
    const disposables = [];

    function uriForTab(tab) {
        return 'file:///project/' + tab.name;
    }

    function tabForUri(uri) {
        const tabs = getTabs() || [];
        return tabs.find(t => uriForTab(t) === uri) || null;
    }

    function setBusy(delta) {
        inFlight += delta;
        if (!available) return;
        onStatus(inFlight > 0 ? 'analyzing' : 'idle');
    }

    // ---------------------------------------------------------------------
    // Transport
    // ---------------------------------------------------------------------

    function handleServerMessage(msg) {
        if (!msg || typeof msg !== 'object') return;
        if (msg.method === 'textDocument/publishDiagnostics') {
            applyDiagnostics(msg.params);
        }
    }

    function onWorkerMessage(e) {
        const msg = e.data;
        if (msg.type === 'lsp-result' || msg.type === 'lsp-error') {
            const entry = pending.get(msg.id);
            pending.delete(msg.id);
            const messages = msg.messages || [];
            // Notifications ride along in the same batch as the response, so
            // route them before resolving — a completion request that
            // triggered an analysis carries that analysis's diagnostics.
            let response = null;
            for (const m of messages) {
                if (m && m.id !== undefined && m.id === msg.id) response = m;
                else handleServerMessage(m);
            }
            if (entry) {
                if (msg.type === 'lsp-error') entry.reject(new Error(msg.error));
                else entry.resolve(entry.raw ? msg : response);
            }
            setBusy(-1);
        }
    }

    /* The worker-message id and the JSON-RPC id are deliberately the same
     * number. The server's reply batch carries the response *and* whatever
     * notifications the handler produced, and telling them apart means
     * matching `id` -- which only works if the two id spaces are one.
     *
     * `raw` resolves with the whole worker message instead of the JSON-RPC
     * response inside it. Only the file reader needs that: it is not a
     * JSON-RPC method, so there is no response to match and its payload rides
     * on the envelope. */
    function post(payload, id, raw) {
        return new Promise((resolve, reject) => {
            if (!worker) { reject(new Error('lsp worker not started')); return; }
            const msgId = id === undefined ? nextId++ : id;
            pending.set(msgId, { resolve, reject, raw: !!raw });
            setBusy(1);
            worker.postMessage(Object.assign({ id: msgId }, payload));
        });
    }

    /** Read a stdlib source file out of the module's virtual filesystem.
     *  Resolves to null for anything the export refuses, which is every kind
     *  of failure -- see the worker's read-file branch. */
    async function readStdlibFile(path) {
        if (!available) return null;
        try {
            const msg = await post({ type: 'read-file', path }, undefined, true);
            return msg && typeof msg.text === 'string' ? msg.text : null;
        } catch {
            return null;
        }
    }

    /** Send a request and resolve with its `result` (null on any failure). */
    async function request(method, params) {
        if (!available) return null;
        const id = nextId++;
        try {
            const response = await post({
                type: 'lsp',
                request: JSON.stringify({ jsonrpc: '2.0', id, method, params }),
            }, id);
            if (!response || response.error) return null;
            return response.result;
        } catch (err) {
            return null;
        }
    }

    /** Send a notification. Nothing to wait for, but the reply batch may carry
     *  publishDiagnostics, so it is still round-tripped. */
    function notify(method, params) {
        if (!available) return Promise.resolve();
        return post({
            type: 'lsp',
            request: JSON.stringify({ jsonrpc: '2.0', method, params }),
        }).catch(() => {});
    }

    // ---------------------------------------------------------------------
    // Diagnostics
    // ---------------------------------------------------------------------

    function applyDiagnostics(params) {
        if (!params || !params.uri) return;
        const tab = tabForUri(params.uri);
        if (!tab || !tab._model || tab._model.isDisposed()) return;
        const model = tab._model;

        const markers = (params.diagnostics || []).map(d => {
            const r = fromLspRange(model, d.range);
            return {
                severity: lspSeverityToMonaco(d.severity),
                message: d.message || '',
                code: d.code ? String(d.code) : undefined,
                source: d.source || 'turmeric',
                startLineNumber: r.startLineNumber,
                startColumn: r.startColumn,
                endLineNumber: r.endLineNumber,
                endColumn: r.endColumn,
            };
        });
        monaco.editor.setModelMarkers(model, 'turmeric', markers);
    }

    function lspSeverityToMonaco(sev) {
        const S = monaco.MarkerSeverity;
        switch (sev) {
            case 1:  return S.Error;
            case 2:  return S.Warning;
            case 3:  return S.Info;
            case 4:  return S.Hint;
            default: return S.Error;
        }
    }

    function clearMarkers(tab) {
        if (tab && tab._model && !tab._model.isDisposed()) {
            monaco.editor.setModelMarkers(tab._model, 'turmeric', []);
        }
    }

    // ---------------------------------------------------------------------
    // Document lifecycle
    // ---------------------------------------------------------------------

    function scheduleChange(uri) {
        const existing = dirtyTimers.get(uri);
        if (existing) clearTimeout(existing);
        dirtyTimers.set(uri, setTimeout(() => sendChange(uri), debounceMs));
    }

    function sendChange(uri) {
        const timer = dirtyTimers.get(uri);
        if (timer) clearTimeout(timer);
        dirtyTimers.delete(uri);

        const entry = openDocs.get(uri);
        if (!entry) return Promise.resolve();
        const tab = tabForUri(uri);
        if (!tab || !tab._model || tab._model.isDisposed()) return Promise.resolve();

        entry.version++;
        return notify('textDocument/didChange', {
            textDocument: { uri, version: entry.version },
            contentChanges: [{ text: tab._model.getValue() }],
        });
    }

    /** Push any debounced edit before a request that reads the symbol index.
     *  The debounce is allowed to delay work; it is not allowed to make an
     *  answer describe the buffer as it was two keystrokes ago. */
    function flushPendingChanges() {
        const uris = Array.from(dirtyTimers.keys());
        if (uris.length === 0) return Promise.resolve();
        return Promise.all(uris.map(sendChange));
    }

    function openDocument(tab) {
        const uri = uriForTab(tab);
        if (openDocs.has(uri)) return;
        const model = tab._model;
        if (!model || model.isDisposed()) return;

        const listener = model.onDidChangeContent(() => scheduleChange(uri));
        openDocs.set(uri, { tabId: tab.id, version: 1, listener });

        notify('textDocument/didOpen', {
            textDocument: {
                uri, languageId, version: 1, text: model.getValue(),
            },
        });
    }

    function closeDocument(uri) {
        const entry = openDocs.get(uri);
        if (!entry) return;
        if (entry.listener) entry.listener.dispose();
        const timer = dirtyTimers.get(uri);
        if (timer) clearTimeout(timer);
        dirtyTimers.delete(uri);
        openDocs.delete(uri);
        notify('textDocument/didClose', { textDocument: { uri } });
    }

    /**
     * Reconcile the server's open-document set with the tab strip.
     *
     * Driven off the tab list rather than hooked into every mutation site:
     * tabs are created, closed, renamed, reordered, and replaced wholesale by
     * a project load, and a client that has to be told about each of those is
     * a client that will miss one.
     */
    function sync() {
        if (!available) return;
        const tabs = (getTabs() || []).filter(t => t._model && !t._model.isDisposed());
        const live = new Set(tabs.map(uriForTab));

        for (const uri of Array.from(openDocs.keys())) {
            if (!live.has(uri)) closeDocument(uri);
        }
        for (const tab of tabs) openDocument(tab);
    }

    // ---------------------------------------------------------------------
    // Providers
    // ---------------------------------------------------------------------

    function registerProviders() {
        disposables.push(monaco.languages.registerCompletionItemProvider(languageId, {
            triggerCharacters: ['('],
            async provideCompletionItems(model, position, context, token) {
                const uri = uriForModel(model);
                if (!uri) return { suggestions: [] };
                await flushPendingChanges();
                const result = await request('textDocument/completion', {
                    textDocument: { uri },
                    position: toLspPosition(model, position),
                });
                if (!result || token.isCancellationRequested) return { suggestions: [] };

                const items = Array.isArray(result) ? result : (result.items || []);
                const word = model.getWordUntilPosition(position);
                const range = {
                    startLineNumber: position.lineNumber,
                    endLineNumber: position.lineNumber,
                    startColumn: word.startColumn,
                    endColumn: word.endColumn,
                };
                return {
                    // `isIncomplete` is how the server says "the cap cut this
                    // list"; honouring it makes Monaco re-query as the prefix
                    // narrows instead of showing a silently truncated menu.
                    incomplete: !!result.isIncomplete,
                    suggestions: items.map(item => ({
                        label: item.label,
                        kind: completionKind(item.kind),
                        detail: item.detail,
                        documentation: markupToMonaco(item.documentation),
                        insertText: item.insertText || item.label,
                        range,
                    })),
                };
            },
        }));

        disposables.push(monaco.languages.registerHoverProvider(languageId, {
            async provideHover(model, position, token) {
                const uri = uriForModel(model);
                if (!uri) return null;
                await flushPendingChanges();
                const result = await request('textDocument/hover', {
                    textDocument: { uri },
                    position: toLspPosition(model, position),
                });
                if (token.isCancellationRequested) return null;

                const value = result ? hoverText(result.contents) : '';
                if (value) {
                    return {
                        contents: [{ value }],
                        range: result.range
                            ? fromLspRange(model, result.range) : undefined,
                    };
                }

                // The server answered nothing. That is not the same as "there
                // is nothing" -- the index is built from bindings, so a name
                // with no binding (a compiler builtin) and a name the analysis
                // never reached both land here. Fall back to the docs table
                // the page already has loaded for its docs pane.
                return docFallbackHover(model, position);
            },
        }));

        disposables.push(monaco.languages.registerSignatureHelpProvider(languageId, {
            signatureHelpTriggerCharacters: ['('],
            signatureHelpRetriggerCharacters: [' '],
            async provideSignatureHelp(model, position, token) {
                const uri = uriForModel(model);
                if (!uri) return null;
                await flushPendingChanges();
                const result = await request('textDocument/signatureHelp', {
                    textDocument: { uri },
                    position: toLspPosition(model, position),
                });
                if (!result || !result.signatures || token.isCancellationRequested) {
                    return null;
                }
                return {
                    value: {
                        signatures: result.signatures.map(s => ({
                            label: s.label,
                            documentation: markupToMonaco(s.documentation),
                            parameters: (s.parameters || []).map(p => ({
                                label: p.label,
                                documentation: markupToMonaco(p.documentation),
                            })),
                        })),
                        activeSignature: result.activeSignature || 0,
                        activeParameter: result.activeParameter || 0,
                    },
                    dispose() {},
                };
            },
        }));

        disposables.push(monaco.languages.registerDefinitionProvider(languageId, {
            async provideDefinition(model, position, token) {
                const uri = uriForModel(model);
                if (!uri) return null;
                await flushPendingChanges();
                const result = await request('textDocument/definition', {
                    textDocument: { uri },
                    position: toLspPosition(model, position),
                });
                if (!result || token.isCancellationRequested) return null;

                const loc = Array.isArray(result) ? result[0] : result;
                if (!loc || !loc.uri) return null;

                let targetTab = tabForUri(loc.uri);
                if (!targetTab || !targetTab._model || targetTab._model.isDisposed()) {
                    // A definition outside the tab strip -- in practice the
                    // stdlib, which the bundle carries at /stdlib. The host
                    // opens it as a read-only buffer and hands the tab back;
                    // it answers null for anything it cannot or will not open,
                    // which is then the same "no definition" it was before.
                    targetTab = await onOpenExternal(loc.uri);
                    if (token.isCancellationRequested) return null;
                    if (!targetTab || !targetTab._model ||
                        targetTab._model.isDisposed()) {
                        return null;
                    }
                }
                const range = fromLspRange(targetTab._model, loc.range);

                // Tell the host where we are leaving *from* before it moves.
                // Doing it here rather than from a keybinding is what makes
                // the jump-back stack cover Cmd+click and the context menu
                // too: this is the one place every route to a definition
                // passes through, and it has the origin position in hand.
                onBeforeNavigate(model, position);

                // Monaco's standalone editor cannot reliably switch models on
                // its own, so the tab switch is done here rather than hoped
                // for. The Location is still returned: if the host editor can
                // follow it, the extra reveal is a no-op.
                if (targetTab._model !== model) onNavigate(targetTab, range);

                return { uri: targetTab._model.uri, range };
            },
        }));

        disposables.push(monaco.languages.registerDocumentSymbolProvider(languageId, {
            provideDocumentSymbols,
        }));

        // Occurrence highlight. Without a provider Monaco falls back to its
        // own word-based selection highlight, which is textual: it matches
        // inside strings and comments, and it cannot tell `total` from the
        // `total` in `subtotal`. The server scans with the reader's regions
        // skipped, so this replaces a plausible answer with a correct one --
        // and with the minimap on, the marks are painted down the whole file,
        // where a wrong answer is a visible wrong answer.
        //
        // A server that predates the handler answers "Method not found",
        // `request` turns that into null, and Monaco goes back to the textual
        // fallback -- which is where it was before this existed.
        disposables.push(monaco.languages.registerDocumentHighlightProvider(languageId, {
            provideDocumentHighlights,
        }));
    }

    async function provideDocumentHighlights(model, position, token) {
        const uri = uriForModel(model);
        if (!uri) return null;
        const result = await request('textDocument/documentHighlight', {
            textDocument: { uri },
            position: toLspPosition(model, position),
        });
        if (!Array.isArray(result)) return null;
        if (token && token.isCancellationRequested) return null;
        return result.map(h => ({
            range: fromLspRange(model, h.range),
            kind: highlightKind(h.kind),
        }));
    }

    /**
     * Shared by Monaco's provider registration and by the page's own Symbols
     * dropdown, which has no other way in: `monaco.languages` registers
     * providers but exposes no way to *invoke* one, and re-implementing the
     * request beside it would be a second call site to keep in step with the
     * range conversion and the pending-edit flush.
     *
     * `token` is optional so a caller outside Monaco does not have to
     * manufacture a cancellation token to ask a question.
     */
    async function provideDocumentSymbols(model, token) {
        const uri = uriForModel(model);
        if (!uri) return [];
        await flushPendingChanges();
        const result = await request('textDocument/documentSymbol', {
            textDocument: { uri },
        });
        if (!Array.isArray(result)) return [];
        if (token && token.isCancellationRequested) return [];
        return result.map(s => ({
            name: s.name,
            detail: s.detail || '',
            kind: symbolKind(s.kind),
            // The spoken name of the kind ("function", "type"), carried
            // alongside Monaco's numeric enum because the dropdown renders a
            // label and Monaco offers no way back from the number.
            kindName: SYMBOL_KIND_NAMES[s.kind] || 'Variable',
            tags: [],
            range: fromLspRange(model, s.range),
            selectionRange: fromLspRange(model, s.selectionRange || s.range),
        }));
    }

    /**
     * Hover built from the documentation table rather than from analysis.
     *
     * Marked as such in the text. A hover that silently mixes "the checker
     * says this" with "the manual says this" is worse than either: the first
     * is a fact about this buffer and the second is a fact about the library,
     * and they can disagree.
     *
     * Everything here is best-effort and synchronous. If the table has not
     * loaded, `lookupDoc` returns null and hover behaves exactly as it did
     * before this existed -- no new fetch happens on the hover path.
     */
    function docFallbackHover(model, position) {
        let word = null;
        try {
            word = model.getWordAtPosition(position);
        } catch {
            return null;
        }
        if (!word || !word.word) return null;

        let entry = null;
        try {
            entry = lookupDoc(word.word);
        } catch {
            return null;
        }
        if (!entry || !entry.summary) return null;

        const lines = ['```\n' + word.word + '\n```', entry.summary];
        lines.push(entry.spice
            ? `_from the \`tur-${entry.spice}\` docs_`
            : '_from the stdlib docs_');
        return {
            contents: [{ value: lines.join('\n\n') }],
            range: {
                startLineNumber: position.lineNumber,
                endLineNumber: position.lineNumber,
                startColumn: word.startColumn,
                endColumn: word.endColumn,
            },
        };
    }

    function uriForModel(model) {
        const tabs = getTabs() || [];
        const tab = tabs.find(t => t._model === model);
        return tab ? uriForTab(tab) : null;
    }

    function completionKind(kind) {
        const name = COMPLETION_KIND_NAMES[kind] || 'Text';
        const K = monaco.languages.CompletionItemKind;
        return K[name] !== undefined ? K[name] : K.Text;
    }

    function highlightKind(kind) {
        const K = monaco.languages.DocumentHighlightKind;
        // LSP DocumentHighlightKind: 1 Text, 2 Read, 3 Write. Monaco names the
        // same three, but its numbering starts at 0, so this is a table too.
        if (kind === 3) return K.Write;
        if (kind === 2) return K.Read;
        return K.Text;
    }

    function symbolKind(kind) {
        const name = SYMBOL_KIND_NAMES[kind] || 'Variable';
        const K = monaco.languages.SymbolKind;
        return K[name] !== undefined ? K[name] : K.Variable;
    }

    function markupToMonaco(doc) {
        if (!doc) return undefined;
        if (typeof doc === 'string') return { value: doc };
        return { value: doc.value || '' };
    }

    function hoverText(contents) {
        if (!contents) return '';
        if (typeof contents === 'string') return contents;
        if (Array.isArray(contents)) return contents.map(hoverText).join('\n\n');
        return contents.value || '';
    }

    // ---------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------

    function start() {
        if (startPromise) return startPromise;

        startPromise = new Promise((resolve) => {
            let settled = false;
            const finish = (ok, reason) => {
                if (settled) return;
                settled = true;
                available = ok;
                onStatus(ok ? 'idle' : 'unavailable');
                if (!ok && reason) {
                    console.info('Turmeric LSP unavailable:', reason);
                }
                resolve(ok);
            };

            onStatus('booting');
            try {
                worker = new Worker('/lsp-worker.js');
            } catch (err) {
                finish(false, String(err));
                return;
            }

            worker.addEventListener('message', (e) => {
                const msg = e.data;
                if (msg.type === 'ready') {
                    finish(true);
                    registerProviders();
                    sync();
                    return;
                }
                if (msg.type === 'init-error') {
                    finish(false, msg.error);
                    return;
                }
                onWorkerMessage(e);
            });
            worker.addEventListener('error', (e) => {
                finish(false, String(e.message || e));
            });
            worker.postMessage({ type: 'init' });
        });

        return startPromise;
    }

    /** Drop every open document server-side and re-open from the tab strip.
     *  Used after a project load, where re-opening tabs into a session that
     *  still holds the previous project's files would leave workspace/symbol
     *  answering with names from a workspace the user closed. */
    async function resetWorkspace() {
        if (!available) return;
        for (const tab of getTabs() || []) clearMarkers(tab);
        for (const uri of Array.from(openDocs.keys())) {
            const entry = openDocs.get(uri);
            if (entry && entry.listener) entry.listener.dispose();
            const timer = dirtyTimers.get(uri);
            if (timer) clearTimeout(timer);
            dirtyTimers.delete(uri);
            openDocs.delete(uri);
        }
        await post({ type: 'lsp-reset' }).catch(() => {});
        await request('initialize', { processId: null, rootUri: null, capabilities: {} });
        sync();
    }

    /** Analyze anything still pending and publish its diagnostics. The session
     *  flushes at the end of each message, so this is normally a no-op; it
     *  exists for the case where a batch of opens left something behind. */
    function flush() {
        if (!available) return Promise.resolve();
        return post({ type: 'lsp-flush' }).catch(() => {});
    }

    function dispose() {
        if (disposed) return;
        disposed = true;
        for (const d of disposables) {
            try { d.dispose(); } catch (_) { /* provider already gone */ }
        }
        disposables.length = 0;
        for (const timer of dirtyTimers.values()) clearTimeout(timer);
        dirtyTimers.clear();
        for (const entry of openDocs.values()) {
            if (entry.listener) entry.listener.dispose();
        }
        openDocs.clear();
        if (worker) { worker.terminate(); worker = null; }
        available = false;
    }

    return {
        start,
        sync,
        flush,
        resetWorkspace,
        dispose,
        // The outline surface (M2). Monaco registers providers but never lets
        // anything call one, so the page asks through here.
        documentSymbols: (model) => provideDocumentSymbols(model, null),
        // Stdlib source, for the read-only buffers onOpenExternal fills (M4).
        readFile: readStdlibFile,
        // Test surface: the provider's own answer, so a spec can assert what
        // the server said without depending on Monaco's decoration class
        // names for the rendering of it.
        documentHighlights: (model, position) =>
            provideDocumentHighlights(model, position, null),
        isAvailable: () => available,
        isBusy: () => inFlight > 0,
        // Test surface: lets a spec wait for the server rather than sleeping.
        _openDocumentCount: () => openDocs.size,
    };
}
