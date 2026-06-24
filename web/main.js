/**
 * Try Turmeric - Web-based REPL
 * Main JavaScript file for the Turmeric WASM REPL
 */

import { TUTORIAL_STEPS } from './tutorials.js';

// ============================================================================
// WASM Module State
// ============================================================================

const WASM_STATE = {
    INITIALIZING: 'initializing',
    LOADING: 'loading',
    READY: 'ready',
    ERROR: 'error'
};

let wasmState = WASM_STATE.INITIALIZING;
let evalWorker = null;
let evalCallId = 0;
const pendingCalls = new Map();
let editor = null;
let monaco = null;
let consoleOutput = [];
let executionQueue = [];
let isExecuting = false;
let replHistory = [];
let replHistoryIndex = -1;
let currentLangMode = 'turmeric'; // tracks active #lang mode

// ============================================================================
// Configuration
// ============================================================================

const CONFIG = {
    DEFAULT_CODE: `#lang sweet-exp

println "Hello, Turmeric!"
`,
    EXECUTION_TIMEOUT: 5000, // 5 seconds
    MAX_OUTPUT_LENGTH: 10000,
};

// ============================================================================
// Code Examples
// ============================================================================

const EXAMPLES = {
    hello: `(println "Hello, World!")`,
    math: `;; Basic arithmetic
(+ 1 2 3)
(- 10 5)
(* 2 3 4)
(/ 100 4)`,
    factorial: `;; Factorial function
(defn factorial [n :int] :int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))

(factorial 10)`,
    fibonacci: `;; Fibonacci sequence
(defn fib [n :int] :int
  (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))

(fib 10)`,
    closure: `;; Closures example
(defn make-adder [x :int] (fn [y :int] (+ x y)))

(let [add5 (make-adder 5)]
  (println (add5 10)))

(let [add10 (make-adder 10)]
  (println (add10 20)))`,
    effects: `(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))`,
    sweet: `#lang sweet-exp
;; Sweet-exp syntax: indentation, curly-infix, neoteric, $

defn square [x : int] : int
  {x * x}

defn sum-squares [a : int b : int] : int
  +(square(a) square(b))

println $ sum-squares 3 4
`
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Escape HTML special characters
 */
function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}


/**
 * Format a console line with prompt and result
 */
function formatConsoleLine(input, result, isError = false) {
    const prompt = '<span class="console-prompt">></span>';
    const escapedInput = escapeHtml(input);
    const escapedResult = escapeHtml(result);
    
    if (isError) {
        return `${prompt} <span class="console-error">${escapedInput}\n${escapedResult}</span>`;
    }
    return `${prompt} ${escapedInput}\n<span class="console-result">${escapedResult}</span>`;
}

/**
 * Append text to the console
 */
function appendToConsole(html) {
    const consoleEl = document.getElementById('console');
    consoleEl.insertAdjacentHTML('beforeend', html + '<br>');
    consoleEl.scrollTop = consoleEl.scrollHeight;
}

/**
 * Clear the console
 */
function clearConsole() {
    const consoleEl = document.getElementById('console');
    consoleEl.innerHTML = '';
    consoleOutput = [];
}

/**
 * Copy console content to clipboard
 */
async function copyConsole() {
    const consoleEl = document.getElementById('console');
    const text = consoleEl.textContent;
    try {
        await navigator.clipboard.writeText(text);
        showStatus('Console copied to clipboard!', 'success');
    } catch (e) {
        showStatus('Failed to copy console', 'error');
    }
}

/**
 * Show status message
 */
function showStatus(message, type = 'info') {
    const statusEl = document.getElementById('wasm-status-text');
    const dotEl = document.querySelector('.dot');
    
    statusEl.textContent = message;
    
    if (dotEl) {
        dotEl.className = 'dot';
        if (type === 'success') dotEl.classList.add('online');
        else if (type === 'error') dotEl.classList.add('offline');
        else dotEl.classList.add('loading');
    }
    
    // Reset after a delay
    setTimeout(() => {
        if (wasmState === WASM_STATE.READY) {
            statusEl.textContent = 'Ready';
            if (dotEl) {
                dotEl.className = 'dot online';
            }
        }
    }, 3000);
}

/**
 * Update cursor position display
 */
function updateCursorPosition() {
    if (!editor) return;
    const cursor = editor.getPosition();
    const posEl = document.getElementById('cursor-pos');
    if (posEl) {
        posEl.textContent = `Line ${cursor.lineNumber}, Column ${cursor.column}`;
    }
}

/**
 * Update execution time display
 */
function updateExecTime(timeMs) {
    const timeEl = document.getElementById('exec-time');
    if (timeEl) {
        timeEl.textContent = timeMs < 1 ? 'Ready' : `${timeMs.toFixed(0)}ms`;
    }
}

/**
 * Encode state to URL hash
 */
function encodeState(code) {
    try {
        const compressed = pako.gzip(code);
        const base64 = btoa(String.fromCharCode(...compressed));
        return base64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    } catch (e) {
        console.error('Failed to encode state:', e);
        return '';
    }
}

/**
 * Decode state from URL hash
 */
function decodeState(hash) {
    try {
        if (!hash) return '';
        const base64 = hash.replace(/-/g, '+').replace(/_/g, '/');
        // Pad with '=' to make length a multiple of 4
        const padLength = (4 - (base64.length % 4)) % 4;
        const padded = base64 + '='.repeat(padLength);
        const binary = atob(padded);
        const compressed = new Uint8Array(binary.split('').map(c => c.charCodeAt(0)));
        return pako.ungzip(compressed, { to: 'string' });
    } catch (e) {
        console.error('Failed to decode state:', e);
        return '';
    }
}

/**
 * Update URL hash with current code
 */
function updateUrlHash() {
    if (!editor) return;
    const code = editor.getValue();
    const encoded = encodeState(code);
    if (encoded) {
        window.location.hash = `code=${encoded}`;
    }
}

/**
 * Load code from URL hash
 */
function loadFromUrlHash() {
    const hash = window.location.hash.slice(1);
    const params = new URLSearchParams(hash);
    const encoded = params.get('code');
    if (encoded) {
        const code = decodeState(encoded);
        if (code && editor) {
            editor.setValue(code);
        }
    }
}

// ============================================================================
// WASM Module Loading
// ============================================================================

/**
 * Initialize the WASM module via the eval Worker.
 * All WASM calls (eval, format, doc lookup, reset) run inside the Worker so
 * that Atomics.wait is permitted and blocking select cannot freeze the tab.
 */
async function initWasm() {
    if (wasmState !== WASM_STATE.INITIALIZING) return;

    wasmState = WASM_STATE.LOADING;

    const loadingOverlay = document.getElementById('loading-overlay');
    const wasmStatus = document.getElementById('wasm-status-text');
    if (loadingOverlay) loadingOverlay.style.display = 'flex';
    if (wasmStatus) wasmStatus.textContent = 'Loading WASM module...';

    try {
        await new Promise((resolve, reject) => {
            evalWorker = new Worker('/eval-worker.js');

            evalWorker.addEventListener('message', (e) => {
                const msg = e.data;

                if (msg.type === 'ready') {
                    resolve();
                    return;
                }
                if (msg.type === 'init-error') {
                    reject(new Error(msg.error));
                    return;
                }

                // Console output forwarded from WASM print/printErr callbacks.
                if (msg.type === 'print') {
                    appendToConsole(`<span class="console-output">${escapeHtml(msg.text)}</span>`);
                    return;
                }
                if (msg.type === 'printErr') {
                    appendToConsole(`<span class="console-error">${escapeHtml(msg.text)}</span>`);
                    return;
                }

                // Resolve pending call promises (eval, format, doc, reset).
                const pending = pendingCalls.get(msg.id);
                if (!pending) return;
                pendingCalls.delete(msg.id);

                if (msg.type === 'eval-result') {
                    const execTime = performance.now() - pending.startTime;
                    updateExecTime(execTime);
                    const isError = msg.result.startsWith('Error:') || msg.result.includes('error');
                    pending.resolve({ result: msg.result, isError, execTime });
                    setTimeout(processQueue, 0);
                } else if (msg.type === 'format-result') {
                    pending.resolve(msg.result);
                } else if (msg.type === 'doc-result') {
                    pending.resolve(msg.result);
                } else if (msg.type === 'reset-done') {
                    pending.resolve();
                } else if (msg.type === 'error') {
                    pending.reject(new Error(msg.error));
                    if (pending.isEval) setTimeout(processQueue, 0);
                }
            });

            evalWorker.addEventListener('error', (e) => {
                reject(new Error(String(e.message || e)));
            });

            evalWorker.postMessage({ type: 'init' });
        });

        console.log('Turmeric WASM runtime initialized');
        wasmState = WASM_STATE.READY;

        const replInput = document.getElementById('repl-input');
        if (replInput) replInput.disabled = false;

        showStatus('Ready', 'success');
        loadFromUrlHash();
        fetchDocNames();

        if (loadingOverlay) loadingOverlay.style.display = 'none';

    } catch (error) {
        console.error('Failed to initialize WASM:', error);
        wasmState = WASM_STATE.ERROR;
        if (loadingOverlay) loadingOverlay.style.display = 'none';

        if (error.message === 'shared-array-buffer-unavailable') {
            showStatus('Browser not supported', 'error');
            appendToConsole(
                '<span class="console-error">The Try Turmeric playground requires a browser with ' +
                '<a href="https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer#browser_compatibility" ' +
                'target="_blank" rel="noopener" style="color:inherit">SharedArrayBuffer</a> support.<br>' +
                'On iOS, please update to iOS 15.4 or later and use Safari.<br>' +
                'On desktop, Chrome, Firefox, and Edge all work.</span>'
            );
        } else {
            showStatus('Failed to load WASM', 'error');
            appendToConsole('<span class="console-error">Error: Failed to load WASM module. Please refresh the page.</span>');
        }
    }
}

/**
 * Strip a leading #lang directive from code and return the detected language
 * name plus the remaining source.  The #lang line must be the first non-
 * blank line (leading spaces/tabs are allowed but not newlines).
 *
 * Returns { lang: string|null, body: string }
 *   lang -- the language name (e.g. "sweet-exp") or null if no directive found
 *   body -- source text with the #lang line removed
 */
function parseLangDirective(code) {
    const m = code.match(/^[ \t]*#lang[ \t]+(\S+)([ \t]*\r?\n?|$)/);
    if (!m) return { lang: null, body: code };
    return { lang: m[1], body: code.slice(m[0].length) };
}

/**
 * Evaluate Turmeric code via the eval Worker.
 */
function evaluateCode(code) {
    return new Promise((resolve, reject) => {
        executionQueue.push({ code, resolve, reject });
        if (!isExecuting) processQueue();
    });
}

/**
 * Process the execution queue, sending one eval at a time to the Worker.
 * The Worker message handler calls setTimeout(processQueue, 0) when a result
 * arrives so the next queued item is dispatched on the next tick.
 */
function processQueue() {
    if (executionQueue.length === 0) {
        isExecuting = false;
        return;
    }

    isExecuting = true;

    const { code, resolve, reject } = executionQueue.shift();

    if (wasmState !== WASM_STATE.READY) {
        reject(new Error('WASM not ready'));
        updateExecTime(0);
        setTimeout(processQueue, 0);
        return;
    }

    // turi_eval_typed detects and strips an inline #lang directive itself, so
    // pass the raw source through. Still parse it locally to keep the UI's
    // mode indicator (currentLangMode) in sync; also forward `lang` to the
    // Worker as a hint for runtimes that export _turi_wasm_set_lang.
    const { lang } = parseLangDirective(code);
    if (lang !== null) currentLangMode = lang;

    const id = ++evalCallId;
    pendingCalls.set(id, { resolve, reject, startTime: performance.now(), isEval: true });
    evalWorker.postMessage({ type: 'eval', id, input: code, lang });
}

/**
 * Reset the WASM environment via the eval Worker.
 */
function resetWasm() {
    if (wasmState !== WASM_STATE.READY) return;

    const id = ++evalCallId;
    pendingCalls.set(id, {
        resolve: () => {
            currentLangMode = 'turmeric'; // turi_env_new() always starts in default mode
            clearConsole();
            showStatus('Environment reset', 'success');
        },
        reject:  (err) => { console.error('Reset error:', err); showStatus('Failed to reset', 'error'); },
        startTime: performance.now(),
        isEval: false,
    });
    evalWorker.postMessage({ type: 'reset', id });
}

// ============================================================================
// Monaco Editor Setup
// ============================================================================

/**
 * Configure Monaco Editor for Turmeric
 */
function configureMonaco() {
    // Define Turmeric language
    monaco.languages.register({
        id: 'turmeric',
        extensions: ['.tur'],
        aliases: ['Turmeric', 'tur'],
        mimetypes: ['text/x-turmeric']
    });
    
    // Define syntax highlighting rules
    monaco.languages.setMonarchTokensProvider('turmeric', {
        tokenizer: {
            root: [
                // Comments
                [/;.*/, 'comment'],
                
                // Strings
                [/"([^"\\]|\\.)*"/, 'string'],
                
                // Numbers
                [/\b\d+\b/, 'number'],
                [/\b\d+\.\d+\b/, 'number.float'],
                
                // Keywords
                [/\b(defn|def|fn|let|if|when|unless|loop|for|while|do|return|break|continue)\b/, 'keyword'],
                [/\b(println|print|read|eval|apply|map|filter|reduce|fold)\b/, 'keyword.operator'],
                
                // Built-in types and functions
                [/\b(int|float|bool|str|char|nil|true|false)\b/, 'type'],
                [/\b(rc\/of|rc\/clone|rc\/drop|rc\/strong-count)\b/, 'keyword.operator'],
                [/\b(defeffect|perform|handle|resume|cont)\b/, 'keyword.operator'],
                [/\b(try|catch|throw|raise)\b/, 'keyword.operator'],
                
                // Symbols
                [/[\+\-\*\/\=\<\>\!\&\|\%\^~]/, 'operator'],
                [/[\(\)\[\]\{\}]/, 'delimiter'],
                
                // Identifiers
                [/[a-zA-Z_][a-zA-Z0-9_\-]*/, 'identifier']
            ]
        }
    });
    
    // Define theme
    monaco.editor.defineTheme('turmeric-light', {
        base: 'vs',
        inherit: true,
        rules: [
            { token: 'comment', foreground: '8292a2' },
            { token: 'string', foreground: '2aa198' },
            { token: 'number', foreground: 'd16969' },
            { token: 'keyword', foreground: '20bbb4', fontStyle: 'bold' },
            { token: 'keyword.operator', foreground: '20bbb4' },
            { token: 'type', foreground: 'dc9656' },
            { token: 'operator', foreground: '89ddff' },
            { token: 'delimiter', foreground: '24292e' },
            { token: 'identifier', foreground: '24292e' }
        ],
        colors: {
            'editor.background': '#ffffff',
            'editor.foreground': '#24292e',
            'editorCursor.foreground': '#0366d6',
            'editor.lineHighlightBackground': '#f8f8f8',
            'editorLineNumber.foreground': '#959da5',
            'editor.selectionBackground': 'rgba(3, 102, 214, 0.3)',
            'editor.inactiveSelectionBackground': 'rgba(3, 102, 214, 0.1)',
            'editorIndentGuide.background': '#e1e4e8',
            'editorIndentGuide.activeBackground': '#959da5'
        }
    });
    
    monaco.editor.defineTheme('turmeric-dark', {
        base: 'vs-dark',
        inherit: true,
        rules: [
            // Dark Spice Market syntax colors — keywords (amber), types (teal),
            // builtins (violet), strings (coral), comments (dark italic),
            // numbers (sage green) per design-notes.md
            { token: 'comment',          foreground: '48433D', fontStyle: 'italic' },
            { token: 'string',           foreground: 'D9735A' },
            { token: 'number',           foreground: 'A8C98A' },
            { token: 'number.float',     foreground: 'A8C98A' },
            { token: 'keyword',          foreground: 'EFA030', fontStyle: 'bold' },
            { token: 'keyword.operator', foreground: 'C4A0E8' },
            { token: 'type',             foreground: '7AC4B8' },
            { token: 'operator',         foreground: '6A5F58' },
            { token: 'delimiter',        foreground: '5A5448' },
            { token: 'identifier',       foreground: 'EAE0D2' },
        ],
        colors: {
            // Editor canvas
            'editor.background':                    '#0C0A08',
            'editor.foreground':                    '#EAE0D2',
            'editorGutter.background':              '#0C0A08',
            'editorLineNumber.foreground':          '#453F39',
            'editorLineNumber.activeForeground':    '#88796C',

            // Cursor & selection
            'editorCursor.foreground':              '#D48B1C',
            'editor.selectionBackground':           'rgba(212,139,28,0.18)',
            'editor.inactiveSelectionBackground':   'rgba(212,139,28,0.08)',
            'editor.selectionHighlightBackground':  'rgba(212,139,28,0.08)',
            'editor.wordHighlightBackground':       'rgba(212,139,28,0.10)',
            'editor.wordHighlightStrongBackground': 'rgba(212,139,28,0.20)',

            // Line highlight
            'editor.lineHighlightBackground':       '#111009',
            'editor.lineHighlightBorder':           '#00000000',

            // Indent guides
            'editorIndentGuide.background1':        '#252119',
            'editorIndentGuide.activeBackground1':  '#3E3830',

            // Bracket matching — gold tint
            'editorBracketMatch.background':        'rgba(212,139,28,0.12)',
            'editorBracketMatch.border':            'rgba(212,139,28,0.50)',

            // Find matches
            'editorFindMatch.background':           'rgba(212,139,28,0.28)',
            'editorFindMatch.border':               'rgba(212,139,28,0.65)',
            'editorFindMatchHighlight.background':  'rgba(212,139,28,0.12)',

            // Autocomplete / hover / suggest widgets
            'editorWidget.background':                       '#181512',
            'editorWidget.border':                           '#302B24',
            'editorWidget.foreground':                       '#EAE0D2',
            'editorSuggestWidget.background':                '#181512',
            'editorSuggestWidget.border':                    '#302B24',
            'editorSuggestWidget.foreground':                '#EAE0D2',
            'editorSuggestWidget.selectedBackground':        'rgba(212,139,28,0.15)',
            'editorSuggestWidget.selectedForeground':        '#EAE0D2',
            'editorSuggestWidget.highlightForeground':       '#EFA030',
            'editorHoverWidget.background':                  '#181512',
            'editorHoverWidget.border':                      '#302B24',
            'editorHoverWidget.foreground':                  '#EAE0D2',

            // Scrollbars — warm dark, gold on active
            'scrollbar.shadow':                     '#00000000',
            'scrollbarSlider.background':           'rgba(62,56,48,0.55)',
            'scrollbarSlider.hoverBackground':      'rgba(88,79,68,0.75)',
            'scrollbarSlider.activeBackground':     'rgba(212,139,28,0.40)',

            // Focus ring — gold instead of VS Code blue
            'focusBorder':                          'rgba(212,139,28,0.40)',
        }
    });

    // Always use dark theme to match Dark Spice Market design
    monaco.editor.setTheme('turmeric-dark');
}

/**
 * Initialize Monaco Editor
 */
async function initEditor() {
    // Hide loading spinner
    const editorLoading = document.getElementById('editor-loading');
    if (editorLoading) editorLoading.style.display = 'none';
    
    // Create editor
    editor = monaco.editor.create(document.getElementById('editor'), {
        value: CONFIG.DEFAULT_CODE,
        language: 'turmeric',
        theme: 'turmeric-dark',
        automaticLayout: true,
        tabSize: 2,
        insertSpaces: true,
        detectIndentation: false,
        minimap: {
            enabled: false
        },
        scrollbar: {
            vertical: 'auto',
            horizontal: 'auto',
            verticalScrollbarSize: 8,
            horizontalScrollbarSize: 8
        },
        lineNumbers: 'on',
        lineDecorationsWidth: 10,
        folding: true,
        wordWrap: 'on',
        autoClosingBrackets: 'beforeWhitespace',
        autoClosingQuotes: 'beforeWhitespace',
        autoSurround: 'never',
        bracketMatching: true,
        colorDecorators: true,
        contextmenu: true,
        cursorBlinking: 'blink',
        cursorStyle: 'line',
        disableLayerHinting: true,
        disableMonospaceOptimizations: false,
        dragAndDrop: true,
        find: {
            seedSearchStringFromSelection: true
        },
        hover: {
            enabled: true
        },
        links: true,
        mouseWheelZoom: false,
        multiCursorModifier: 'ctrlCmd',
        multiCursorPaste: 'full',
        occurrencesHighlight: true,
        overviewRulerBorder: false,
        overviewRulerLanes: 0,
        quickSuggestions: {
            other: true,
            comments: false,
            strings: false
        },
        readOnly: false,
        roundedSelection: true,
        scrollBeyondLastColumn: 5,
        scrollBeyondLastLine: true,
        selectOnLineNumbers: true,
        selectionClipboard: true,
        selectionHighlight: true,
        showFoldingControls: 'mouseover',
        smoothScrolling: true,
        suggest: {
            showWords: true,
            showSnippets: true,
            showFiles: false
        },
        suggestOnTriggerCharacters: true,
        wordBasedSuggestions: true,
        fontFamily: "'Iosevka', 'SF Mono', 'Fira Code', 'Fira Mono', Consolas, monospace",
        fontSize: 13,
        fontLigatures: true,
        fontWeight: '400'
    });
    
    // Expose editor for smoke tests
    window._turiEditor = editor;

    // Update cursor position on cursor change
    editor.onDidChangeCursorPosition(() => updateCursorPosition());
    
    // Update URL hash on content change (debounced)
    let debounceTimer;
    editor.onDidChangeModelContent(() => {
        clearTimeout(debounceTimer);
        debounceTimer = setTimeout(updateUrlHash, 1000);
    });
    
    // Handle Ctrl+Enter to run code
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter,
        () => runCode()
    );
    
    // Handle Ctrl+S to save (update URL hash)
    editor.addCommand(
        monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS,
        (e) => {
            e.preventDefault();
            updateUrlHash();
            showStatus('Code saved to URL', 'success');
        }
    );

    // Handle Alt+Shift+F to format
    editor.addCommand(
        monaco.KeyMod.Alt | monaco.KeyMod.Shift | monaco.KeyCode.KeyF,
        () => formatCode()
    );

    // Register as Monaco document formatter so "Format Document" also works
    monaco.languages.registerDocumentFormattingEditProvider('turmeric', {
        provideDocumentFormattingEdits() {
            return formatCode().then(() => []);
        }
    });
    
    // Initialize cursor position display
    updateCursorPosition();
    
    // Store editor reference for global access
    window.turmericEditor = editor;
}

// ============================================================================
// Event Handlers
// ============================================================================

/**
 * Shared eval + output path used by both the Run button and the REPL input.
 * @param {string} code        Source to evaluate
 * @param {string} promptHtml  HTML for the prompt prefix (e.g. '<span ...>></span>')
 * @param {boolean} showTiming Whether to call updateExecTime and show the loading indicator
 */
async function executeCode(source, promptHtml, showTiming = false, echoSource = true) {
    const consoleLoading = showTiming ? document.getElementById('console-loading') : null;
    if (consoleLoading) consoleLoading.style.display = 'flex';

    if (echoSource) {
        appendToConsole(`${promptHtml} ${escapeHtml(source)}`);
    }

    try {
        const startTime = performance.now();
        const { result, isError } = await evaluateCode(source);
        const execTime = performance.now() - startTime;

        if (consoleLoading) consoleLoading.style.display = 'none';

        if (isError) {
            appendToConsole(`<span class="console-error">${escapeHtml(result)}</span>`);
        } else if (result && result !== 'nil') {
            appendToConsole(`<span class="console-result">${escapeHtml(result)}</span>`);
        }

        if (showTiming) updateExecTime(execTime);
        maybeShowDoc(source.trim());

    } catch (err) {
        if (consoleLoading) consoleLoading.style.display = 'none';
        appendToConsole(`<span class="console-error">Error: ${escapeHtml(err.message)}</span>`);
        if (showTiming) updateExecTime(0);
    }
}

/**
 * Run the current editor contents
 */
async function runCode() {
    if (wasmState !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = editor.getValue();
    if (!code.trim()) {
        appendToConsole('<span class="console-error">Error: No code to evaluate</span>');
        return;
    }
    await executeCode(code, '', true, false);
}

/**
 * Format the editor contents via the eval Worker.
 */
async function formatCode() {
    if (wasmState !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = editor.getValue();
    if (!code.trim()) return;

    try {
        const formatted = await new Promise((resolve, reject) => {
            const id = ++evalCallId;
            pendingCalls.set(id, { resolve, reject, startTime: performance.now(), isEval: false });
            evalWorker.postMessage({ type: 'format', id, input: code });
        });
        editor.setValue(formatted);
        showStatus('Formatted', 'success');
    } catch (err) {
        console.error('Format error:', err);
        showStatus('Format failed', 'error');
    }
}

/**
 * Clear the editor
 */
function clearEditor() {
    if (editor) {
        editor.setValue('');
    }
}

/**
 * Load an example
 */
function loadExample(name) {
    if (EXAMPLES[name]) {
        if (editor) {
            editor.setValue(EXAMPLES[name]);
        }
        updateUrlHash();
        showStatus(`Loaded example: ${name}`, 'success');
    }
}

/**
 * Share the current code
 */
function shareCode() {
    if (!editor) return;
    
    const code = editor.getValue();
    const encoded = encodeState(code);
    
    if (encoded) {
        const url = `${window.location.origin}${window.location.pathname}#code=${encoded}`;
        
        // Copy to clipboard
        navigator.clipboard.writeText(url).then(() => {
            showStatus('URL copied to clipboard!', 'success');
        }).catch(() => {
            // Fallback: show the URL
            prompt('Copy this URL:', url);
        });
    } else {
        showStatus('Failed to encode code', 'error');
    }
}

/**
 * REPL input at the bottom of the console
 */
function initReplInput() {
    const input = document.getElementById('repl-input');
    if (!input) return;

    input.addEventListener('keydown', async (e) => {
        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            const code = input.value.trim();
            if (!code || wasmState !== WASM_STATE.READY) return;

            replHistory.unshift(code);
            replHistoryIndex = -1;
            input.value = '';

            await executeCode(code, '<span class="console-prompt">turi&gt;</span>');

            const consoleEl = document.getElementById('console');
            if (consoleEl) consoleEl.scrollTop = consoleEl.scrollHeight;

        } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            if (replHistoryIndex < replHistory.length - 1) {
                replHistoryIndex++;
                input.value = replHistory[replHistoryIndex];
            }
        } else if (e.key === 'ArrowDown') {
            e.preventDefault();
            if (replHistoryIndex > 0) {
                replHistoryIndex--;
                input.value = replHistory[replHistoryIndex];
            } else {
                replHistoryIndex = -1;
                input.value = '';
            }
        }
    });
}

/**
 * Initialize event listeners
 */
function initEventListeners() {
    // Run button
    document.getElementById('run-btn')?.addEventListener('click', runCode);
    
    // Clear button
    document.getElementById('clear-btn')?.addEventListener('click', clearEditor);

    // Format button
    document.getElementById('format-btn')?.addEventListener('click', formatCode);
    
    // Share button
    document.getElementById('share-btn')?.addEventListener('click', shareCode);
    
    // Clear console button
    document.getElementById('clear-console-btn')?.addEventListener('click', clearConsole);
    
    // Copy console button
    document.getElementById('copy-console-btn')?.addEventListener('click', copyConsole);
    
    // Examples select
    document.getElementById('examples-select')?.addEventListener('change', (e) => {
        if (e.target.value) {
            loadExample(e.target.value);
            e.target.value = '';
        }
    });
    
    // Solve button
    document.getElementById('solve-btn')?.addEventListener('click', solveStep);

    // Tutorial close button
    document.getElementById('tutorial-close')?.addEventListener('click', () => {
        document.getElementById('tutorial-overlay')?.style.setProperty('display', 'none');
    });
    
    // Window resize
    window.addEventListener('resize', () => {
        if (editor) {
            editor.layout();
        }
    });
    
    // Handle hash changes (for sharing)
    window.addEventListener('hashchange', loadFromUrlHash);

    // REPL input
    initReplInput();

    // Horizontal drag-to-scroll on the editor header (mobile / narrow widths)
    initHScrollDrag();
}

function initHScrollDrag() {
    const el = document.querySelector('.editor-header');
    if (!el) return;

    const updateOverflow = () => {
        el.classList.toggle('has-overflow', el.scrollWidth > el.clientWidth + 1);
    };
    updateOverflow();
    if (typeof ResizeObserver !== 'undefined') {
        new ResizeObserver(updateOverflow).observe(el);
    }
    window.addEventListener('resize', updateOverflow);

    let startX = 0;
    let startScroll = 0;
    let dragging = false;
    let pointerId = null;

    const isInteractive = (target) => {
        return !!target.closest('button, select, input, a, [role="button"]');
    };

    el.addEventListener('pointerdown', (e) => {
        if (isInteractive(e.target)) return;
        startX = e.clientX;
        startScroll = el.scrollLeft;
        pointerId = e.pointerId;
        dragging = false;
        try { el.setPointerCapture(pointerId); } catch {}
    });

    el.addEventListener('pointermove', (e) => {
        if (pointerId !== e.pointerId) return;
        const dx = e.clientX - startX;
        if (!dragging && Math.abs(dx) > 6) {
            dragging = true;
            el.dataset.dragging = '1';
        }
        if (dragging) {
            el.scrollLeft = startScroll - dx;
            e.preventDefault();
        }
    });

    const endDrag = (e) => {
        if (pointerId !== null) {
            try { el.releasePointerCapture(pointerId); } catch {}
        }
        pointerId = null;
        if (dragging) {
            const swallow = (ev) => {
                ev.stopPropagation();
                ev.preventDefault();
            };
            el.addEventListener('click', swallow, { capture: true, once: true });
            setTimeout(() => { delete el.dataset.dragging; }, 0);
        }
        dragging = false;
    };
    el.addEventListener('pointerup', endDrag);
    el.addEventListener('pointercancel', endDrag);

    // Keep active tab visible after switching
    document.querySelectorAll('.tab-button').forEach((btn) => {
        btn.addEventListener('click', () => {
            requestAnimationFrame(() => {
                btn.scrollIntoView({ inline: 'nearest', block: 'nearest' });
            });
        });
    });
}

// ============================================================================
// Tutorial System
// ============================================================================

// Tutorial state
let currentTutorial = null;
let currentStep = 0;


/**
 * Load the given step of a tutorial into the editor and update the tutorial bar.
 */
function loadStep(tutorialId, stepIndex) {
    const steps = TUTORIAL_STEPS[tutorialId];
    if (!steps || stepIndex >= steps.length) return;

    const step = steps[stepIndex];
    if (editor) {
        editor.setValue(step.starter);
    }

    // Update tutorial info bar
    const bar = document.getElementById('tutorial-bar');
    const stepEl = document.getElementById('tutorial-bar-step');
    const descEl = document.getElementById('tutorial-bar-desc');
    const solveBtn = document.getElementById('solve-btn');

    if (bar) bar.style.display = 'flex';
    if (stepEl) stepEl.textContent = `${step.title} (${stepIndex + 1}/${steps.length})`;
    if (descEl) descEl.textContent = step.description;
    if (solveBtn) solveBtn.style.display = '';
}

/**
 * Fill in the answer for the current tutorial step without running it.
 */
function solveStep() {
    if (!currentTutorial) return;

    const steps = TUTORIAL_STEPS[currentTutorial];
    if (!steps) return;

    const step = steps[currentStep];
    if (!step) return;

    if (editor) {
        editor.setValue(step.answer);
        showStatus('Answer filled in -- click Run to execute', 'success');
    }
}

/**
 * Start a tutorial
 */
async function startTutorial(tutorialId) {
    // Hide tutorial overlay
    document.getElementById('tutorial-overlay')?.style.setProperty('display', 'none');

    currentTutorial = tutorialId;
    currentStep = 0;

    const steps = TUTORIAL_STEPS[tutorialId];
    if (steps && steps.length > 0) {
        showStatus(`Starting tutorial: ${tutorialId}`, 'success');
        loadStep(tutorialId, 0);
    }
}

/**
 * Show tutorial overlay
 */
function showTutorialOverlay() {
    const overlay = document.getElementById('tutorial-overlay');
    if (overlay) {
        overlay.style.display = 'flex';
    }
}

// ============================================================================
// pako (zlib) for URL compression
// ============================================================================

// We'll use a lightweight implementation or load pako from CDN
// For now, we'll use a simple base64 encoding without compression

// ============================================================================
// Main Initialization
// ============================================================================

/**
 * Main initialization function
 */
async function init() {
    console.log('Initializing Try Turmeric...');
    
    // Load Monaco Editor
    try {
        // Configure Monaco workers BEFORE importing Monaco so workers are
        // available when Monaco initializes during the import itself.
        window.MonacoEnvironment = {
            getWorker: function (_moduleId, label) {
                if (label === 'json') {
                    return new Worker(new URL('monaco-editor/esm/vs/language/json/json.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'css' || label === 'scss' || label === 'less') {
                    return new Worker(new URL('monaco-editor/esm/vs/language/css/css.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'html' || label === 'handlebars' || label === 'razor') {
                    return new Worker(new URL('monaco-editor/esm/vs/language/html/html.worker.js', import.meta.url), { type: 'module' });
                }
                if (label === 'typescript' || label === 'javascript') {
                    return new Worker(new URL('monaco-editor/esm/vs/language/typescript/ts.worker.js', import.meta.url), { type: 'module' });
                }
                return new Worker(new URL('monaco-editor/esm/vs/editor/editor.worker.js', import.meta.url), { type: 'module' });
            }
        };

        monaco = await import('monaco-editor');

        // Configure Monaco (must happen after import)
        configureMonaco();

        // Initialize editor
        await initEditor();
        
        // Initialize event listeners
        initEventListeners();
        
        // Initialize WASM
        await initWasm();
        
        console.log('Try Turmeric initialized successfully');
        
    } catch (error) {
        console.error('Failed to initialize Try Turmeric:', error);
        appendToConsole('<span class="console-error">Error: Failed to initialize editor. Please refresh the page.</span>');
    }
}

// Start initialization when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
} else {
    init();
}

// ============================================================================
// Doc Panel (D5: autodoc integration)
// ============================================================================

// All documented names loaded from /doc-names.json on startup.
let docNames = [];

/**
 * Fetch the doc name list and set up the search bar.
 */
async function fetchDocNames() {
    try {
        const res = await fetch('/doc-names.json');
        if (!res.ok) return;
        docNames = await res.json();
        initDocSearch();
    } catch (_) {
        // Non-fatal — search bar stays empty
    }
}

/**
 * Wire up the doc search input to filter docNames and show results.
 */
function initDocSearch() {
    const input = document.getElementById('doc-search');
    const results = document.getElementById('doc-search-results');
    if (!input || !results) return;

    let activeIndex = -1;

    function renderResults(items) {
        activeIndex = -1;
        if (!items.length) {
            results.innerHTML = '<div class="doc-no-results">No matches</div>';
            results.style.display = 'block';
            return;
        }
        results.innerHTML = items.slice(0, 40).map((item, i) => {
            const shortSummary = item.summary.replace(/^[\w/\-!?*+<>]+\s+--\s+/, '');
            const spiceTag = item.spice
                ? `<span class="doc-result-spice">tur-${escapeHtml(item.spice)}</span>`
                : '';
            return `<div class="doc-result-item" data-name="${escapeHtml(item.name)}" data-index="${i}">
                <span class="doc-result-name">${escapeHtml(item.name)}</span>
                <span class="doc-result-kind">${escapeHtml(item.kind)}</span>
                ${spiceTag}
                <span class="doc-result-summary">${escapeHtml(shortSummary)}</span>
            </div>`;
        }).join('');
        results.style.display = 'block';

        results.querySelectorAll('.doc-result-item').forEach(el => {
            el.addEventListener('mousedown', (e) => {
                e.preventDefault(); // keep focus on input
                selectResult(el.dataset.name);
            });
        });
    }

    function hideResults() {
        results.style.display = 'none';
        results.innerHTML = '';
        activeIndex = -1;
    }

    function selectResult(name) {
        input.value = '';
        hideResults();
        const entry = docNames.find(d => d.name === name) || null;
        wasmDocLookup(name).then(docText => showDocPanel(name, docText, entry));
    }

    function highlightActive() {
        const items = results.querySelectorAll('.doc-result-item');
        items.forEach((el, i) => el.classList.toggle('active', i === activeIndex));
        if (activeIndex >= 0 && items[activeIndex]) {
            items[activeIndex].scrollIntoView({ block: 'nearest' });
        }
    }

    let searchTimer;
    input.addEventListener('input', () => {
        clearTimeout(searchTimer);
        searchTimer = setTimeout(() => {
            const q = input.value.trim().toLowerCase();
            if (!q) { hideResults(); return; }
            const matches = docNames.filter(d =>
                d.name.toLowerCase().includes(q) ||
                d.summary.toLowerCase().includes(q)
            );
            renderResults(matches);
        }, 150);
    });

    input.addEventListener('keydown', (e) => {
        const items = results.querySelectorAll('.doc-result-item');
        if (e.key === 'ArrowDown') {
            e.preventDefault();
            activeIndex = Math.min(activeIndex + 1, items.length - 1);
            highlightActive();
        } else if (e.key === 'ArrowUp') {
            e.preventDefault();
            activeIndex = Math.max(activeIndex - 1, -1);
            highlightActive();
        } else if (e.key === 'Enter') {
            if (activeIndex >= 0 && items[activeIndex]) {
                selectResult(items[activeIndex].dataset.name);
            } else if (input.value.trim()) {
                // Exact lookup
                selectResult(input.value.trim());
            }
        } else if (e.key === 'Escape') {
            hideResults();
            input.blur();
        }
    });

    input.addEventListener('blur', () => {
        // Small delay so mousedown on a result fires first
        setTimeout(hideResults, 150);
    });

    // Open the doc panel when the user starts typing in the search bar
    input.addEventListener('focus', () => {
        const pane = document.getElementById('doc-pane');
        if (pane && pane.style.display === 'none') {
            const container = document.querySelector('.repl-container');
            pane.style.display = 'flex';
            container?.classList.add('doc-open');
        }
        if (input.value.trim()) {
            input.dispatchEvent(new Event('input'));
        }
    });
}

/**
 * Look up documentation for `name` via the eval Worker.
 * Returns a Promise resolving to the doc string, or null if not found.
 */
function wasmDocLookup(name) {
    if (!evalWorker || wasmState !== WASM_STATE.READY) return Promise.resolve(null);
    return new Promise((resolve) => {
        const id = ++evalCallId;
        pendingCalls.set(id, {
            resolve,
            reject: () => resolve(null),
            startTime: performance.now(),
            isEval: false,
        });
        evalWorker.postMessage({ type: 'doc', id, name });
    });
}

/**
 * Show the doc panel with content for `name`.
 */
function showDocPanel(name, docText, entry) {
    const pane = document.getElementById('doc-pane');
    const body = document.getElementById('doc-body');
    const title = document.getElementById('doc-title');
    const link = document.getElementById('doc-full-link');
    const container = document.querySelector('.repl-container');

    if (!pane || !body) return;

    // Fall back to entry if the caller didn't supply one (e.g. (doc name) eval)
    if (entry === undefined) {
        entry = (typeof docNames !== 'undefined')
            ? docNames.find(d => d.name === name) || null
            : null;
    }

    title.textContent = name || 'Documentation';

    if (docText) {
        body.textContent = docText;
    } else if (entry && entry.summary) {
        // wasm doc-lookup table only carries stdlib entries; for spice symbols
        // surface the JSON summary so the panel still has useful content.
        const tag = entry.spice
            ? `<p class="doc-placeholder">From <code>tur-${escapeHtml(entry.spice)}</code>.</p>`
            : '';
        body.innerHTML = tag +
            `<pre class="doc-summary">${escapeHtml(entry.summary)}</pre>`;
    } else {
        body.innerHTML = `<p class="doc-placeholder">No documentation found for <code>${escapeHtml(name)}</code>.</p>`;
    }

    // Link to the per-spice API page for spice symbols, stdlib index otherwise.
    if (entry && entry.spice) {
        link.href = `/docs/html/spices/${entry.spice}/api/`;
        link.textContent = `Open tur-${entry.spice} API \u2197`;
    } else {
        link.href = '/docs/html/api/index.html';
        link.textContent = 'Open full docs \u2197';
    }

    pane.style.display = 'flex';
    container?.classList.add('doc-open');
}

/**
 * Hide the doc panel.
 */
function hideDocPanel() {
    const pane = document.getElementById('doc-pane');
    const container = document.querySelector('.repl-container');
    if (pane) pane.style.display = 'none';
    container?.classList.remove('doc-open');
}

/**
 * If the evaluated code looks like `(doc name)`, trigger the doc panel.
 * Called after each REPL eval / Run.
 */
function maybeShowDoc(code) {
    const m = code.trim().match(/^\(\s*doc\s+([\w/\-!?*+]+)\s*\)$/);
    if (!m) return false;
    const name = m[1];
    wasmDocLookup(name).then(docText => showDocPanel(name, docText));
    return true;
}

// Wire doc panel close button
document.addEventListener('DOMContentLoaded', () => {
    document.getElementById('close-doc-btn')?.addEventListener('click', hideDocPanel);
});

// Export for debugging
window.turmericApp = {
    runCode,
    clearEditor,
    clearConsole,
    formatCode,
    loadExample,
    shareCode,
    resetWasm,
    showDocPanel,
    hideDocPanel,
    wasmDocLookup,
    getState: () => ({
        wasmState,
        hasEditor: !!editor,
        hasWasm: !!evalWorker && wasmState === WASM_STATE.READY,
    })
};
