/**
 * Try Turmeric - Web-based REPL
 * Main JavaScript file for the Turmeric WASM REPL
 */

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
let turiModule = null;
let editor = null;
let monaco = null;
let consoleOutput = [];
let executionQueue = [];
let isExecuting = false;
let replHistory = [];
let replHistoryIndex = -1;

// ============================================================================
// Configuration
// ============================================================================

const CONFIG = {
    DEFAULT_CODE: `(println "Hello, Turmeric!")
`,
    EXECUTION_TIMEOUT: 5000, // 5 seconds
    MAX_OUTPUT_LENGTH: 10000,
    ANSI_COLORS: {
        0: 'ansi-0', 1: 'ansi-1', 2: 'ansi-2', 3: 'ansi-3',
        4: 'ansi-4', 5: 'ansi-5', 6: 'ansi-6', 7: 'ansi-7',
        8: 'ansi-8', 9: 'ansi-9', 10: 'ansi-10', 11: 'ansi-11',
        12: 'ansi-12', 13: 'ansi-13', 14: 'ansi-14', 15: 'ansi-15'
    }
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
    rc: `;; Reference Counting
defn main [] :int
  (let [r (rc/of 42)]
    (println (rc/strong-count r))
    (let [r2 (rc/clone r)]
      (println (rc/strong-count r)))
    (rc/drop r)
    0)`
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
 * Parse ANSI escape codes and convert to HTML spans
 */
function parseAnsi(text) {
    // Remove ANSI codes for now - we'll add proper parsing later
    // This is a simplified version
    return text.replace(/\x1b\[[0-9;]*m/g, '');
    
    /* Full ANSI parsing (for future use):
    let result = '';
    let currentClass = '';
    let i = 0;
    
    while (i < text.length) {
        if (text[i] === '\x1b' && text[i + 1] === '[') {
            // ANSI escape sequence
            const end = text.indexOf('m', i + 2);
            if (end !== -1) {
                const codes = text.slice(i + 2, end).split(';').map(Number);
                const classList = [];
                
                for (const code of codes) {
                    if (code === 0) {
                        classList.length = 0;
                        classList.push('ansi-0');
                    } else if (code === 1) {
                        classList.push('ansi-bold');
                    } else if (code === 3) {
                        classList.push('ansi-italic');
                    } else if (code === 4) {
                        classList.push('ansi-underline');
                    } else if (code >= 30 && code <= 37) {
                        classList.push(CONFIG.ANSI_COLORS[code - 30]);
                    } else if (code >= 90 && code <= 97) {
                        classList.push(CONFIG.ANSI_COLORS[code - 90 + 8]);
                    }
                }
                
                currentClass = classList.join(' ');
                i = end + 1;
            } else {
                result += text[i];
                i++;
            }
        } else {
            result += text[i];
            i++;
        }
    }
    
    return currentClass ? `<span class="${currentClass}">${result}</span>` : result;
    */
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
 * Initialize the WASM module
 */
async function initWasm() {
    if (wasmState !== WASM_STATE.INITIALIZING) return;
    
    wasmState = WASM_STATE.LOADING;
    
    // Show loading overlay
    const loadingOverlay = document.getElementById('loading-overlay');
    const wasmStatus = document.getElementById('wasm-status-text');
    if (loadingOverlay) loadingOverlay.style.display = 'flex';
    if (wasmStatus) wasmStatus.textContent = 'Loading WASM module...';
    
    try {
        // Load the WASM module.
        // turmeric.js is an Emscripten UMD build (MODULARIZE=1, no EXPORT_ES6)
        // loaded via <script> in index.html — it sets window.TurmericModule.
        if (typeof window.TurmericModule !== 'function') {
            throw new Error('window.TurmericModule not found — did turmeric.js load?');
        }

        turiModule = await window.TurmericModule({
            print: (text) => {
                appendToConsole(`<span class="console-output">${escapeHtml(text)}</span>`);
            },
            printErr: (text) => {
                appendToConsole(`<span class="console-error">${escapeHtml(text)}</span>`);
            }
        });

        console.log('WASM module initialized');

        // Call turi_wasm_init directly via exported function pointer
        const initResult = turiModule._turi_wasm_init();
        if (initResult !== 0) {
            throw new Error(`turi_wasm_init failed with code ${initResult}`);
        }
        
        console.log('Turmeric WASM runtime initialized');
        wasmState = WASM_STATE.READY;

        // Enable the REPL input now that WASM is ready
        const replInput = document.getElementById('repl-input');
        if (replInput) replInput.disabled = false;

        // Update status
        showStatus('Ready', 'success');
        
        // Load code from URL hash
        loadFromUrlHash();
        
        // Hide loading overlay
        if (loadingOverlay) loadingOverlay.style.display = 'none';
        
    } catch (error) {
        console.error('Failed to initialize WASM:', error);
        wasmState = WASM_STATE.ERROR;
        showStatus('Failed to load WASM', 'error');
        appendToConsole('<span class="console-error">Error: Failed to load WASM module. Please refresh the page.</span>');
        if (loadingOverlay) loadingOverlay.style.display = 'none';
    }
}

/**
 * Evaluate Turmeric code using WASM
 */
function evaluateCode(code) {
    return new Promise((resolve, reject) => {
        // Queue the evaluation if one is already in progress
        executionQueue.push({ code, resolve, reject });
        
        if (isExecuting) return;
        
        processQueue();
    });
}

/**
 * Process the execution queue
 */
async function processQueue() {
    if (executionQueue.length === 0) {
        isExecuting = false;
        return;
    }
    
    isExecuting = true;
    
    const { code, resolve, reject } = executionQueue.shift();
    
    try {
        if (wasmState !== WASM_STATE.READY) {
            throw new Error('WASM not ready');
        }
        
        // Write input string into WASM memory using the exported helper
        const inputLen = turiModule.lengthBytesUTF8(code) + 1;
        const inputPtr = turiModule._malloc(inputLen);
        turiModule.stringToUTF8(code, inputPtr, inputLen);

        const startTime = performance.now();

        // Call turi_wasm_eval directly via exported function pointer
        // Signature: char* turi_wasm_eval(const char* input)
        const resultPtr = turiModule._turi_wasm_eval(inputPtr);

        const endTime = performance.now();
        const execTime = endTime - startTime;

        turiModule._free(inputPtr);

        // Read result string back from WASM memory then free it
        const result = resultPtr ? turiModule.UTF8ToString(resultPtr) : '';
        if (resultPtr) turiModule._free(resultPtr);
        
        // Check for errors in the result
        const isError = result.startsWith('Error:') || result.includes('error');
        
        updateExecTime(execTime);
        
        resolve({ result, isError, execTime });
        
    } catch (error) {
        console.error('Evaluation error:', error);
        updateExecTime(0);
        reject(error);
    } finally {
        // Process next item in queue
        setTimeout(processQueue, 0);
    }
}

/**
 * Reset the WASM environment
 */
function resetWasm() {
    if (wasmState !== WASM_STATE.READY) return;
    
    try {
        turiModule._turi_wasm_reset();
        clearConsole();
        showStatus('Environment reset', 'success');
    } catch (error) {
        console.error('Reset error:', error);
        showStatus('Failed to reset', 'error');
    }
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
            { token: 'comment', foreground: '6a9955' },
            { token: 'string', foreground: '98c379' },
            { token: 'number', foreground: 'ce9178' },
            { token: 'keyword', foreground: '569cd6', fontStyle: 'bold' },
            { token: 'keyword.operator', foreground: '569cd6' },
            { token: 'type', foreground: 'dcdcaa' },
            { token: 'operator', foreground: '4ec9b0' },
            { token: 'delimiter', foreground: 'd4d4d4' },
            { token: 'identifier', foreground: 'd4d4d4' }
        ],
        colors: {
            'editor.background': '#1e1e1e',
            'editor.foreground': '#d4d4d4',
            'editorCursor.foreground': '#ffffff',
            'editor.lineHighlightBackground': '#2d2d2d',
            'editorLineNumber.foreground': '#6a6a6a',
            'editor.selectionBackground': 'rgba(255, 255, 255, 0.3)',
            'editor.inactiveSelectionBackground': 'rgba(255, 255, 255, 0.1)',
            'editorIndentGuide.background': '#404040',
            'editorIndentGuide.activeBackground': '#6a6a6a'
        }
    });
    
    // Set default theme
    const isDarkMode = window.matchMedia('(prefers-color-scheme: dark)').matches;
    monaco.editor.setTheme(isDarkMode ? 'turmeric-dark' : 'turmeric-light');
    
    // Listen for theme changes
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
        monaco.editor.setTheme(e.matches ? 'turmeric-dark' : 'turmeric-light');
    });
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
        theme: 'turmeric-light',
        automaticLayout: true,
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
        fontFamily: 'SF Mono, Fira Code, Monaco, Consolas, monospace',
        fontSize: 14,
        fontLigatures: false
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
    
    // Initialize cursor position display
    updateCursorPosition();
    
    // Store editor reference for global access
    window.turmericEditor = editor;
}

// ============================================================================
// Event Handlers
// ============================================================================

/**
 * Run the current code
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
    
    // Show loading indicator
    const consoleLoading = document.getElementById('console-loading');
    if (consoleLoading) consoleLoading.style.display = 'flex';
    
    try {
        // Append input to console
        appendToConsole(`<span class="console-prompt">></span> ${escapeHtml(code)}`);
        
        // Evaluate the code
        const startTime = performance.now();
        const { result, isError } = await evaluateCode(code);
        const execTime = performance.now() - startTime;
        
        // Hide loading indicator
        if (consoleLoading) consoleLoading.style.display = 'none';
        
        // Append result to console
        if (isError) {
            appendToConsole(`<span class="console-error">${escapeHtml(result)}</span>`);
        } else if (result && result !== 'nil') {
            appendToConsole(`<span class="console-result">${escapeHtml(result)}</span>`);
        }
        
        updateExecTime(execTime);
        
    } catch (error) {
        if (consoleLoading) consoleLoading.style.display = 'none';
        appendToConsole(`<span class="console-error">Error: ${escapeHtml(error.message)}</span>`);
        updateExecTime(0);
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

            appendToConsole(`<span class="console-prompt">turi&gt;</span> ${escapeHtml(code)}`);

            try {
                const { result, isError } = await evaluateCode(code);
                if (isError) {
                    appendToConsole(`<span class="console-error">${escapeHtml(result)}</span>`);
                } else if (result && result !== 'nil') {
                    appendToConsole(`<span class="console-result">${escapeHtml(result)}</span>`);
                }
            } catch (err) {
                appendToConsole(`<span class="console-error">Error: ${escapeHtml(err.message)}</span>`);
            }

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
}

// ============================================================================
// Tutorial System
// ============================================================================

// Tutorial state
let currentTutorial = null;
let currentStep = 0;

/**
 * Load tutorials from server
 */
async function loadTutorials() {
    try {
        // For now, tutorials are hardcoded
        // In the future, they will be loaded from YAML files
        return [
            { id: 'basics', title: 'Basics', description: 'Learn the fundamentals of Turmeric syntax' },
            { id: 'data-structures', title: 'Data Structures', description: 'Work with vectors, structs, and pattern matching' },
            { id: 'type-system', title: 'Type System', description: 'Understand type annotations and inference' },
            { id: 'functions', title: 'Functions', description: 'Closures, recursion, and higher-order functions' },
            { id: 'effects', title: 'Effects', description: 'Algebraic effects and handlers' }
        ];
    } catch (e) {
        console.error('Failed to load tutorials:', e);
        return [];
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
    
    // For now, just load a sample tutorial
    const tutorials = await loadTutorials();
    const tutorial = tutorials.find(t => t.id === tutorialId);
    
    if (tutorial) {
        showStatus(`Starting tutorial: ${tutorial.title}`, 'success');
        
        // Load first step
        // In the future, this will load the actual tutorial content
        if (editor) {
            editor.setValue(`;; ${tutorial.title} Tutorial\n;; Step 1: Getting Started\n\n(println "Hello, ${tutorial.title}!")`);
        }
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

// Export for debugging
window.turmericApp = {
    runCode,
    clearEditor,
    clearConsole,
    loadExample,
    shareCode,
    resetWasm,
    getState: () => ({
        wasmState,
        hasEditor: !!editor,
        hasWasm: !!turiModule
    })
};
