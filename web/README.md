# Try Turmeric Web

A web-based REPL for the [Turmeric programming language](https://github.com/turmeric-lang/turmeric) that runs entirely in the browser using WebAssembly.

## Features

- **WASM-powered Turmeric runtime** - Full Turmeric evaluation in the browser
- **Monaco Editor** - Professional code editing with syntax highlighting
- **Interactive Console** - ANSI-colored output with execution history
- **Code Examples** - Pre-loaded examples covering key language features
- **Language picker** - Dialect radio group + `#lang` layer toggles that edit
  the `#lang` line in place (the buffer stays the source of truth); options
  are rendered from the runtime's registry export, never hardcoded in JS
- **URL Sharing** - Share your code via compressed URL hash
- **Editor intelligence** - Diagnostics, completion, hover, signature help,
  go-to-definition, and document symbols, served by the real `tur lsp` running
  in a WebAssembly worker
- **Tutorial System** - Guided learning with interactive tutorials (CLI only for now)
- **Responsive Design** - Works on desktop and mobile devices

## Quick Start

### Development

1. Install dependencies:
   ```bash
   cd web
   npm install
   ```

2. Build the WASM module (requires [Emscripten](https://emscripten.org)):
   ```bash
   just configure-wasm
   just wasm
   ```

3. Run the development server:
   ```bash
   just web-dev
   ```

4. Open [http://localhost:3000](http://localhost:3000) in your browser.

#### The service worker is off on localhost

`sw.js` is cache-first for same-origin static assets, and its precache names
`/main.js` and `/styles.css` -- the files a dev server exists to re-serve. A
worker left installed by one session hands the next one the previous session's
JS and CSS, which shows up as an unstyled page running code you already
changed, and needs a hard reload every time.

So on a loopback host the app does not register the worker, and unregisters
any it finds (dropping their caches) before reloading once to pick up the real
files. Nothing to do -- the first load after this change cleans up whatever
was already installed.

To work on the PWA itself, append `?sw=1`:

```
http://localhost:3000/try/?sw=1
```

That is also how `tests/mobile.split-and-pwa.spec.js` and
`tests/docs-offline.spec.js` still exercise the real worker. Production is
unaffected: the opt-out keys off the hostname.

### Production Build

```bash
just web
```

This will:
1. Build the WASM module
2. Install web dependencies
3. Create a production-optimized bundle in `web/dist/`

### Deploy

```bash
just deploy-web
```

This will deploy the web app to GitHub Pages.

## Project Structure

```
web/
├── index.html          # Main HTML page
├── main.js             # Main JavaScript with WASM integration
├── styles.css          # Styling
├── examples.js         # Example code snippets
├── package.json        # Node dependencies
├── vite.config.js      # Vite build configuration
├── lsp-client.js       # Monaco adapter for the language server
└── public/
    ├── turmeric.js      # Compiled WASM module (generated)
    ├── eval-worker.js   # Worker hosting the interpreter
    └── lsp-worker.js    # Worker hosting the language server
```

## Configuration

The web REPL can be configured via the `CONFIG` object in `main.js`:

```javascript
const CONFIG = {
    DEFAULT_CODE: `(println "Hello, Turmeric!")`,
    EXECUTION_TIMEOUT: 5000,    // 5 seconds
    MAX_OUTPUT_LENGTH: 10000
};
```

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Enter` | Run code |
| `Ctrl+S` | Save to URL hash |

## Tutorial System

The tutorial system provides interactive learning. In the CLI REPL, use:

```
:tutorial              # List available tutorials
:tutorial basics      # Start the basics tutorial
:next                 # Go to next step
:prev                 # Go to previous step
:hint                 # Show hint for current step
:skip                 # Skip current step
:quit-tutorial        # Exit tutorial mode
```

The web version will have tutorial integration in a future update.

## Technical Details

### WASM API

The WASM module exposes the following functions:

- `turi_wasm_init()` - Initialize the runtime
- `turi_wasm_reset()` - Reset the evaluation environment
- `turi_wasm_shutdown()` - Shutdown the runtime
- `turi_wasm_eval(input)` - Evaluate code and return result as string
- `turi_wasm_eval_ex(input, out_result, out_error)` - Evaluate with separate result/error
- `turi_wasm_version()` - Get version string
- `turi_wasm_set_lang(name)` - Set the reader dialect + layer set from a full
  `#lang` directive tail (e.g. `"turmeric/sweet stringed"`); assigns the
  layer set and resets the session when it changes
- `turi_wasm_get_lang()` - Current reader name (canonical spelling)
- `turi_wasm_lang_registry()` - JSON registry of base dialects and curated
  `#lang` layers, exported from the C tables for the language picker
- `turi_wasm_lsp_request(json)` - Feed one JSON-RPC message to the language
  server; returns a JSON array of the messages it produced
- `turi_wasm_lsp_flush()` - Analyze documents with pending edits
- `turi_wasm_lsp_reset()` - Drop every open document and start a fresh session

### Language server

The playground runs the same `tur lsp` the native editors use. No language
intelligence is implemented in JavaScript; `lsp-client.js` translates between
Monaco's provider APIs and JSON-RPC, and that is all it does.

It lives in its own worker with its own WASM instance, separate from the
evaluator. Analysis and evaluation must not queue behind each other -- a
three-second loop should not block completion -- and `turi_wasm_reset` tears
the eval environment back to prelude, which a server sharing that instance
would notice. Instantiation is lazy, on first editor focus: it is the same
`/turmeric.js` URL so it is a cache hit rather than a second download, but the
memory is real and a visitor who only reads code should not pay for it.

Every open tab is an open document, which is what makes `workspace/symbol` and
cross-tab go-to-definition work here without the filesystem crawl a native
client needs.

**If the server does not come up** -- an old cached `turmeric.wasm` without the
LSP exports, a browser with no `SharedArrayBuffer`, a worker that throws --
every provider returns empty, markers stay clear, and the footer indicator
hides itself. The playground is exactly as usable as it was before any of this
existed, which is the contract `web/tests/lsp.spec.js` checks first.

Rebuilding the bundle with the LSP exports:

```bash
cmake -S .. -B ../build-wasm -DTUR_WASM=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ../build-wasm --target tur_wasm     # copies into web/public/
```

### Monaco Editor Configuration

The editor is configured with:
- Turmeric language support with custom syntax highlighting
- Light and dark themes that match system preferences
- Auto-closing brackets and quotes
- Line numbers and cursor position display
- Word wrap and smooth scrolling

## License

MIT License - see the main [Turmeric LICENSE](https://github.com/turmeric-lang/turmeric/blob/main/LICENSE) for details.
