// eval-worker.js — Classic Worker
// Loads the Emscripten WASM module and handles eval/format/doc/reset requests
// from the main thread via postMessage.  Running eval inside a Worker means
// Atomics.wait (used by Emscripten pthreads) is allowed and blocking select
// cannot freeze the browser tab.

let turiModule = null;

function postPrint(text) {
    self.postMessage({ type: 'print', text });
}

function postPrintErr(text) {
    self.postMessage({ type: 'printErr', text });
}

// Queue messages that arrive before the module is ready.
const queue = [];
let ready = false;

function drainQueue() {
    const drained = queue.splice(0);
    drained.forEach(msg => handleMessage(msg));
}

function handleMessage(msg) {
    const id = msg.id;

    if (msg.type === 'eval') {
        try {
            const inputLen = turiModule.lengthBytesUTF8(msg.input) + 1;
            const inputPtr = turiModule._malloc(inputLen);
            turiModule.stringToUTF8(msg.input, inputPtr, inputLen);
            const resultPtr = turiModule._turi_wasm_eval(inputPtr);
            turiModule._free(inputPtr);
            const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : '';
            if (resultPtr) turiModule._free(resultPtr);
            self.postMessage({ type: 'eval-result', id, result });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'format') {
        try {
            const inputLen = turiModule.lengthBytesUTF8(msg.input) + 1;
            const inputPtr = turiModule._malloc(inputLen);
            turiModule.stringToUTF8(msg.input, inputPtr, inputLen);
            const resultPtr = turiModule._turi_wasm_format(inputPtr);
            turiModule._free(inputPtr);
            if (!resultPtr) {
                self.postMessage({ type: 'error', id, error: 'format failed' });
                return;
            }
            const result = turiModule.UTF8ToString(resultPtr);
            turiModule._free(resultPtr);
            self.postMessage({ type: 'format-result', id, result });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }

    } else if (msg.type === 'doc') {
        try {
            const fn = turiModule._turi_doc_lookup;
            if (!fn) { self.postMessage({ type: 'doc-result', id, result: null }); return; }
            const nameLen = turiModule.lengthBytesUTF8(msg.name) + 1;
            const inputPtr = turiModule._malloc(nameLen);
            turiModule.stringToUTF8(msg.name, inputPtr, nameLen);
            const resultPtr = fn(inputPtr);
            turiModule._free(inputPtr);
            const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : null;
            self.postMessage({ type: 'doc-result', id, result });
        } catch (_) {
            self.postMessage({ type: 'doc-result', id, result: null });
        }

    } else if (msg.type === 'reset') {
        try {
            turiModule._turi_wasm_reset();
            self.postMessage({ type: 'reset-done', id });
        } catch (err) {
            self.postMessage({ type: 'error', id, error: String(err) });
        }
    }
}

self.addEventListener('message', function (e) {
    const msg = e.data;

    if (msg.type === 'init') {
        importScripts('/turmeric.js');

        const factory = self.TurmericModule;
        if (typeof factory !== 'function') {
            self.postMessage({ type: 'init-error', error: 'TurmericModule not found after importScripts' });
            return;
        }

        factory({ print: postPrint, printErr: postPrintErr }).then(function (mod) {
            turiModule = mod;
            const initResult = turiModule._turi_wasm_init();
            if (initResult !== 0) {
                self.postMessage({ type: 'init-error', error: 'turi_wasm_init failed: ' + initResult });
                return;
            }
            ready = true;
            self.postMessage({ type: 'ready' });
            drainQueue();
        }).catch(function (err) {
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
