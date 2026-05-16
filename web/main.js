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

        // Load doc name list for the search bar (non-blocking)
        fetchDocNames();

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
        fontFamily: "'JetBrains Mono', 'SF Mono', 'Fira Code', 'Fira Mono', Consolas, monospace",
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

        // If the code was a (doc name) call, populate the doc panel
        maybeShowDoc(code.trim());

    } catch (error) {
        if (consoleLoading) consoleLoading.style.display = 'none';
        appendToConsole(`<span class="console-error">Error: ${escapeHtml(error.message)}</span>`);
        updateExecTime(0);
    }
}

/**
 * Format the editor contents using turi_wasm_format
 */
async function formatCode() {
    if (wasmState !== WASM_STATE.READY) {
        showStatus('WASM not ready', 'error');
        return;
    }
    const code = editor.getValue();
    if (!code.trim()) return;

    const inputLen = turiModule.lengthBytesUTF8(code) + 1;
    const inputPtr = turiModule._malloc(inputLen);
    turiModule.stringToUTF8(code, inputPtr, inputLen);

    const resultPtr = turiModule._turi_wasm_format(inputPtr);
    turiModule._free(inputPtr);

    if (!resultPtr) {
        showStatus('Format failed', 'error');
        return;
    }

    const formatted = turiModule.UTF8ToString(resultPtr);
    turiModule._free(resultPtr);

    editor.setValue(formatted);
    showStatus('Formatted', 'success');
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
                // Populate doc panel for (doc name) expressions
                maybeShowDoc(code);
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
}

// ============================================================================
// Tutorial System
// ============================================================================

// Tutorial state
let currentTutorial = null;
let currentStep = 0;

// Tutorial step data: each step has a title, description, starter code, and answer
const TUTORIAL_STEPS = {
    'basics': [
        {
            title: 'Step 1: Hello World',
            description: 'Print "Hello, Turmeric!" to the console using println.',
            starter: ';; Print "Hello, Turmeric!" to the console\n',
            answer: '(println "Hello, Turmeric!")'
        },
        {
            title: 'Step 2: Arithmetic',
            description: 'Add 1, 2, and 3 together using the + operator.',
            starter: ';; Add 1, 2, and 3 together\n',
            answer: '(+ 1 2 3)'
        },
        {
            title: 'Step 3: Variables',
            description: 'Bind the value 42 to the name x using let, then print it.',
            starter: ';; Bind 42 to x and print it\n',
            answer: '(let [x 42]\n  (println x))'
        },
        {
            title: 'Step 4: Conditionals',
            description: 'Use if to print "big" when 10 > 5, otherwise "small".',
            starter: ';; Print "big" if 10 > 5, otherwise "small"\n',
            answer: '(println (if (> 10 5) "big" "small"))'
        }
    ],
    'data-structures': [
        {
            title: 'Step 1: Vectors',
            description: 'Create a vector [1 2 3] and print its first element using get.',
            starter: ';; Create a vector and access its first element\n',
            answer: '(let [v [1 2 3]]\n  (println (get v 0)))'
        },
        {
            title: 'Step 2: Structs',
            description: 'Define a Point struct with x and y fields, create a Point at (3, 4), and print its fields.',
            starter: ';; Define a Point struct and create an instance at (3, 4)\n',
            answer: '(defstruct Point [x :int y :int])\n\n(let [p (Point 3 4)]\n  (println (.x p))\n  (println (.y p)))'
        },
        {
            title: 'Step 3: Pattern Matching',
            description: 'Use match to print "zero", "one", or "other" for the value of n.',
            starter: ';; Match n and print "zero", "one", or "other"\n(let [n 1]\n  )',
            answer: '(let [n 1]\n  (println (match n\n    0 "zero"\n    1 "one"\n    _ "other")))'
        },
        {
            title: 'Step 4: Map and Filter',
            description: 'Double each number in [1 2 3 4], then keep only those greater than 4.',
            starter: ';; Double [1 2 3 4], then filter to keep elements > 4\n',
            answer: '(let [doubled (map (fn [x] (* x 2)) [1 2 3 4])]\n  (println (filter (fn [x] (> x 4)) doubled)))'
        }
    ],
    'type-system': [
        {
            title: 'Step 1: Type Annotations',
            description: 'Define an "add" function with :int annotations on both parameters and the return type.',
            starter: ';; Define add with type annotations, then call it with 3 and 4\n',
            answer: '(defn add [a :int b :int] :int\n  (+ a b))\n\n(println (add 3 4))'
        },
        {
            title: 'Step 2: Polymorphic Functions',
            description: 'Write an "identity" function using a type variable :a that works on any type.',
            starter: ';; Define a polymorphic identity function and test it on an int and a string\n',
            answer: '(defn identity [x :a] :a x)\n\n(println (identity 42))\n(println (identity "hello"))'
        },
        {
            title: 'Step 3: Struct Types',
            description: 'Define a Point struct and a "get-x" function that takes a :Point and returns its x field.',
            starter: ';; Define Point and a function that extracts its x field\n',
            answer: '(defstruct Point [x :int y :int])\n\n(defn get-x [p :Point] :int (.x p))\n\n(println (get-x (Point 10 20)))'
        }
    ],
    'functions': [
        {
            title: 'Step 1: Defining Functions',
            description: 'Define a "square" function that multiplies its argument by itself.',
            starter: ';; Define square and call it with 7\n',
            answer: '(defn square [n :int] :int (* n n))\n\n(println (square 7))'
        },
        {
            title: 'Step 2: Recursion',
            description: 'Write a recursive "factorial" function.',
            starter: ';; Define a recursive factorial function and compute (factorial 5)\n',
            answer: '(defn factorial [n :int] :int\n  (if (<= n 1) 1 (* n (factorial (- n 1)))))\n\n(println (factorial 5))'
        },
        {
            title: 'Step 3: Closures',
            description: 'Write a "make-adder" function that returns a closure adding x to its argument.',
            starter: ';; Define make-adder, create add5, and call it with 10\n',
            answer: '(defn make-adder [x :int] (fn [y :int] (+ x y)))\n\n(let [add5 (make-adder 5)]\n  (println (add5 10)))'
        },
        {
            title: 'Step 4: Higher-Order Functions',
            description: 'Use map to double every element in [1 2 3 4 5].',
            starter: ';; Use map to double every element in [1 2 3 4 5]\n',
            answer: '(println (map (fn [x] (* x 2)) [1 2 3 4 5]))'
        }
    ],
    'effects': [
        {
            title: 'Step 1: Defining Effects',
            description: 'Declare an Ask effect that yields an int, perform it, and handle it by resuming with 41.',
            starter: ';; Define Ask, perform it in a function, and handle with resume k 41\n',
            answer: '(defeffect Ask [] :int)\n\n(defn use-ask [] :int\n  (+ 1 (perform (Ask))))\n\n(println (handle (use-ask)\n  (Ask [] k) (resume k 41)))'
        },
        {
            title: 'Step 2: Effects with Logging',
            description: 'Define a Logger effect that prints a message, then returns a value from the computation.',
            starter: ';; Define Logger, log "hello", return 42, and handle by printing the message\n',
            answer: '(defeffect Logger [msg :str] :nil)\n\n(defn logged-value [] :int\n  (perform (Logger "hello"))\n  42)\n\n(println (handle (logged-value)\n  (Logger [msg k] (println msg) (resume k nil))))'
        },
        {
            title: 'Step 3: Multiple Effects',
            description: 'Handle two different effects -- Ask and Log -- in a single handle block.',
            starter: ';; Define Ask and Log effects, use both in a program, handle both together\n',
            answer: '(defeffect Ask [] :int)\n(defeffect Log [msg :str] :nil)\n\n(defn program [] :int\n  (perform (Log "starting"))\n  (+ 1 (perform (Ask))))\n\n(println (handle (program)\n  (Ask [] k) (resume k 10)\n  (Log [msg k] (println msg) (resume k nil))))'
        }
    ]
};

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
            return `<div class="doc-result-item" data-name="${escapeHtml(item.name)}" data-index="${i}">
                <span class="doc-result-name">${escapeHtml(item.name)}</span>
                <span class="doc-result-kind">${escapeHtml(item.kind)}</span>
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
        const docText = wasmDocLookup(name);
        showDocPanel(name, docText);
        input.value = '';
        hideResults();
    }

    function highlightActive() {
        const items = results.querySelectorAll('.doc-result-item');
        items.forEach((el, i) => el.classList.toggle('active', i === activeIndex));
        if (activeIndex >= 0 && items[activeIndex]) {
            items[activeIndex].scrollIntoView({ block: 'nearest' });
        }
    }

    input.addEventListener('input', () => {
        const q = input.value.trim().toLowerCase();
        if (!q) { hideResults(); return; }
        const matches = docNames.filter(d =>
            d.name.toLowerCase().includes(q) ||
            d.summary.toLowerCase().includes(q)
        );
        renderResults(matches);
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
 * Look up documentation for `name` via the WASM turi_doc_lookup export.
 * Returns the doc string, or null if not found or WASM not ready.
 */
function wasmDocLookup(name) {
    if (!turiModule || wasmState !== WASM_STATE.READY) return null;
    try {
        const fn = turiModule._turi_doc_lookup;
        if (!fn) return null;
        const inputPtr = turiModule.allocate(
            turiModule.intArrayFromString(name), turiModule.ALLOC_NORMAL);
        const resultPtr = fn(inputPtr);
        turiModule._free(inputPtr);
        if (!resultPtr) return null;
        return turiModule.UTF8ToString(resultPtr);
    } catch (_) {
        return null;
    }
}

/**
 * Show the doc panel with content for `name`.
 */
function showDocPanel(name, docText) {
    const pane = document.getElementById('doc-pane');
    const body = document.getElementById('doc-body');
    const title = document.getElementById('doc-title');
    const link = document.getElementById('doc-full-link');
    const container = document.querySelector('.repl-container');

    if (!pane || !body) return;

    title.textContent = name || 'Documentation';

    if (docText) {
        body.textContent = docText;
    } else {
        body.innerHTML = `<p class="doc-placeholder">No documentation found for <code>${escapeHtml(name)}</code>.</p>`;
    }

    // Build a link to the module page: guess the module from the function name
    // The function table is in docs/api/index.html; link there as fallback
    link.href = '/docs/html/api/index.html';
    link.textContent = 'Open full docs \u2197';

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
    const docText = wasmDocLookup(name);
    showDocPanel(name, docText);
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
        hasWasm: !!turiModule
    })
};
