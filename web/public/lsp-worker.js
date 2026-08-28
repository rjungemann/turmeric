// lsp-worker.js — Classic Worker hosting the Turmeric language server.
//
// Deliberately a *second* WASM instance, separate from /eval-worker.js.
//
// Eval and analysis must not queue behind each other: a user running a
// three-second loop should still get completion, and `turi_wasm_reset` tears
// the eval environment back to prelude, which a language server sharing that
// instance would notice. Separate instances mean separate linear memories and
// no shared mutable global state.
//
// The cost is a second instantiation. It is not a second download — the same
// /turmeric.js URL, so a cache hit — but it is real memory, which is why the
// page instantiates this lazily on first editor focus rather than at load. A
// visitor who only reads code never pays for it.

let mod = null;
let ready = false;
const queue = [];

// Marshal a JS string into the module heap, call `fn`, and read the malloc'd
// C string back. Every export here has that shape.
function callStringToString(fn, input) {
    const len = mod.lengthBytesUTF8(input) + 1;
    const ptr = mod._malloc(len);
    mod.stringToUTF8(input, ptr, len);
    let outPtr = 0;
    try {
        outPtr = fn(ptr);
    } finally {
        mod._free(ptr);
    }
    if (!outPtr) return '[]';
    const out = mod.UTF8ToString(outPtr);
    mod._free(outPtr);
    return out;
}

function callVoidToString(fn) {
    const outPtr = fn();
    if (!outPtr) return '[]';
    const out = mod.UTF8ToString(outPtr);
    mod._free(outPtr);
    return out;
}

// The server answers with a JSON array of messages: the response, plus any
// publishDiagnostics notifications the analysis produced along the way. A
// malformed reply is reported as an empty batch rather than thrown — a
// language server failing must never be able to break the editor.
function parseBatch(json, id) {
    try {
        const parsed = JSON.parse(json);
        return Array.isArray(parsed) ? parsed : [];
    } catch (err) {
        self.postMessage({ type: 'lsp-error', id, error: 'bad reply: ' + String(err) });
        return [];
    }
}

function handleMessage(msg) {
    const id = msg.id;
    try {
        if (msg.type === 'lsp') {
            const raw = callStringToString(mod._turi_wasm_lsp_request, msg.request);
            self.postMessage({ type: 'lsp-result', id, messages: parseBatch(raw, id) });

        } else if (msg.type === 'lsp-flush') {
            const raw = callVoidToString(mod._turi_wasm_lsp_flush);
            self.postMessage({ type: 'lsp-result', id, messages: parseBatch(raw, id) });

        } else if (msg.type === 'lsp-reset') {
            mod._turi_wasm_lsp_reset();
            self.postMessage({ type: 'lsp-result', id, messages: [] });

        } else if (msg.type === 'read-file') {
            // Stdlib source, for a go-to-definition that lands outside the tab
            // strip. The export refuses anything that is not a .tur file under
            // the stdlib mount, so `text: null` covers "not allowed", "not
            // there", and "could not read" alike -- the page's answer to all
            // three is the same, and telling them apart would only report
            // which paths exist.
            //
            // Not routed through callStringToString: that helper substitutes
            // '[]' for a null return, which is exactly the distinction that
            // matters here.
            let text = null;
            if (typeof mod._turi_wasm_read_file === 'function') {
                const len = mod.lengthBytesUTF8(msg.path) + 1;
                const ptr = mod._malloc(len);
                mod.stringToUTF8(msg.path, ptr, len);
                let outPtr = 0;
                try {
                    outPtr = mod._turi_wasm_read_file(ptr);
                } finally {
                    mod._free(ptr);
                }
                if (outPtr) {
                    text = mod.UTF8ToString(outPtr);
                    mod._free(outPtr);
                }
            }
            self.postMessage({ type: 'lsp-result', id, messages: [], text });
        }
    } catch (err) {
        self.postMessage({ type: 'lsp-error', id, error: String(err) });
    }
}

self.addEventListener('message', function (e) {
    const msg = e.data;

    if (msg.type === 'init') {
        // Same SharedArrayBuffer requirement as the eval worker: the bundle is
        // built with pthreads, which needs cross-origin isolation.
        if (typeof SharedArrayBuffer === 'undefined') {
            self.postMessage({ type: 'init-error', error: 'shared-array-buffer-unavailable' });
            return;
        }

        importScripts('/turmeric.js');
        const factory = self.TurmericModule;
        if (typeof factory !== 'function') {
            self.postMessage({ type: 'init-error', error: 'TurmericModule not found' });
            return;
        }

        // print/printErr are swallowed rather than forwarded. The analysis
        // compiles the buffer on every quiet window; routing its stderr to the
        // page's console would fill the user's output pane with diagnostics
        // they are already seeing as squiggles.
        factory({ print: function () {}, printErr: function () {} })
            .then(function (m) {
                mod = m;
                // A build without the LSP exports is a valid build — the
                // playground worked without them for its whole life. Say so
                // clearly instead of failing on the first request.
                if (typeof mod._turi_wasm_lsp_request !== 'function') {
                    self.postMessage({
                        type: 'init-error',
                        error: 'lsp-export-missing',
                    });
                    return;
                }
                // No turi_wasm_init(): that boots the *interpreter* environment
                // and preloads the stdlib into it. The language server compiles
                // from source and needs none of it.
                ready = true;
                self.postMessage({ type: 'ready' });
                queue.splice(0).forEach(handleMessage);
            })
            .catch(function (err) {
                self.postMessage({ type: 'init-error', error: String(err) });
            });
        return;
    }

    if (!ready) {
        queue.push(msg);
        return;
    }
    handleMessage(msg);
});
