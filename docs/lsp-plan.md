# LSP Support Plan for Turmeric

## Overview

Add Language Server Protocol (LSP) support to the Turmeric compiler. The
implementation has three parts:

1. `tur check --json` -- structured diagnostic output for tooling
2. `tur lsp` -- embedded LSP server (JSON-RPC over stdio)
3. VSCode extension update -- language client that launches `tur lsp`

---

## What Already Exists

- `diag_set_json_output(bool)` / `diag_emit_json()` in `src/compiler/diag.c`
  -- prints each diagnostic as a JSON object to **stderr**, 1-based line/col,
  one object per line (not an array). Activated by `--json` or
  `--json-diagnostics` global flags.
- `src/compiler/diag.h` -- `JsonDiag` struct stub (unused by current code).
- VSCode extension (`vscode-syntax-ext/`) -- syntax highlighting + formatter
  via `tur format`. No LSP client yet.

---

## Phase 1: `tur check --json` (LSP-compatible diagnostic output)

### Problem with existing JSON output

The existing `diag_emit_json` writes to stderr, uses 1-based coordinates, and
emits one JSON object per diagnostic rather than an array. LSP needs:

- 0-based line/col (LSP spec)
- `range: { start: {line, character}, end: {line, character} }` shape
- A single JSON array flushed to **stdout** after compilation finishes
- Exit code 0 (no errors) or 1 (errors found)

### Changes to `src/compiler/diag.h`

Add an accumulation mode alongside the existing immediate-print JSON mode:

```c
// Enable LSP collection mode: diagnostics are buffered instead of printed.
// Resets the internal list. Incompatible with diag_set_json_output.
void diag_lsp_begin(void);

// Flush buffered diagnostics as an LSP-shaped JSON array to `out`.
// Format: {"diagnostics": [{severity, code, message, range, source}, ...]}
// Uses 0-based line/col. Caller responsible for fclose if needed.
void diag_lsp_flush(FILE *out);

// Discard buffered diagnostics and disable collection mode.
void diag_lsp_end(void);
```

### Changes to `src/compiler/diag.c`

Add a static `DiagLspEntry` array (capacity-doubled, initial 32):

```c
typedef struct DiagLspEntry {
    DiagLevel   level;
    DiagCode    code;
    uint16_t    file_id;
    uint32_t    line0;         // 0-based
    uint32_t    col_start0;    // 0-based
    uint32_t    col_end0;      // 0-based
    char        message[512];  // truncated message
} DiagLspEntry;
```

When `lsp_collect_` is true, all `diag_emit*` variants append to this array
instead of printing. `diag_lsp_flush` serializes the array using the existing
`Buf` + `json_escape_string` helpers, then writes to `out`.

The existing `json_output_` / `diag_emit_json` path is **not changed** --
it remains the stderr-per-line mode for `--json-diagnostics`.

### Changes to `src/main.c`

The existing `check` subcommand already calls `compile_to_c`. Detect `--json`
inside the check handler (it is already stripped from argv by the global
flag-parsing loop as `use_json_output`):

```c
if (strcmp(cmd, "check") == 0) {
    // ...existing --help check...
    if (argc != 3) return usage_check();
    if (use_json_output) {
        diag_lsp_begin();
        Buf out; buf_init(&out);
        compile_to_c(argv[2], &out, NULL, 0);
        buf_free(&out);
        diag_lsp_flush(stdout);
        diag_lsp_end();
        return diag_had_error() ? 1 : 0;
    }
    // ...existing path...
}
```

Usage: `tur --json check myfile.tur`

---

## Phase 2: `tur lsp` -- Embedded LSP Server

### Directory layout

```
src/lsp/
  lsp.h / lsp.c          -- server entry point, main loop
  lsp_io.h / lsp_io.c    -- JSON-RPC Content-Length framing (stdin/stdout)
  lsp_json.h / lsp_json.c -- minimal JSON builder + key extractor
  lsp_docs.h / lsp_docs.c -- document store (URI -> text + Expr tree)
```

No external JSON library dependency. Use the existing `Buf` for building
JSON; use `strstr`-based key extraction for parsing incoming messages (LSP
messages are structured enough that a full parser is not needed for MVP).

### `src/lsp/lsp_io.h`

```c
// Read one JSON-RPC message from fd_in (blocks).
// Returns heap-allocated null-terminated body, or NULL on EOF/error.
char *lsp_read_message(int fd_in);

// Write one JSON-RPC message to fd_out with Content-Length framing.
void  lsp_write_message(int fd_out, const char *json, size_t len);
```

Implementation: accumulate bytes until `\r\n\r\n`, parse `Content-Length: N`,
malloc N+1 bytes, read exactly N bytes for body.

### `src/lsp/lsp_json.h`

```c
// Extract a string value for a key in a flat JSON object.
// Returns pointer into `json` (not NUL-terminated); fills *len.
// Returns NULL if key is absent.
const char *lsp_json_str(const char *json, const char *key, size_t *len);

// Extract an integer value for a key. Returns -1 if absent.
int64_t lsp_json_int(const char *json, const char *key);

// Extract the "params" object as a raw substring.
const char *lsp_json_params(const char *json, size_t *len);
```

For **building** JSON, use `Buf` directly (already used by `diag.c`).

### `src/lsp/lsp_docs.h`

```c
typedef struct LspDoc {
    char   *uri;       // heap-allocated
    char   *path;      // URI decoded, heap-allocated
    char   *text;      // current source text, heap-allocated
    size_t  text_len;
} LspDoc;

// Find or insert document by URI.
LspDoc *lsp_doc_open(const char *uri, size_t uri_len,
                     const char *text, size_t text_len);

// Update text for existing document.
void    lsp_doc_change(const char *uri, size_t uri_len,
                       const char *text, size_t text_len);

void    lsp_doc_close(const char *uri, size_t uri_len);
LspDoc *lsp_doc_get(const char *uri, size_t uri_len);
```

Open-addressing hash map (FNV-1a on URI, initial capacity 16, resize at 0.75).

### `src/lsp/lsp.h` -- server capabilities and main loop

```c
void lsp_server_run(int fd_in, int fd_out);
```

Main loop:
1. `lsp_read_message` -- get raw JSON body
2. Extract `method` and `id` from body
3. Dispatch to handler function
4. If handler returns non-NULL Buf: `lsp_write_message`

### LSP methods (MVP)

| Method | Handler action |
|---|---|
| `initialize` | Respond with ServerCapabilities: textDocumentSync=1, hoverProvider=false (MVP), definitionProvider=false (MVP) |
| `initialized` | No-op notification |
| `shutdown` | Set shutdown flag, respond null |
| `exit` | `exit(shutdown_seen ? 0 : 1)` |
| `textDocument/didOpen` | `lsp_doc_open`, run check, push `publishDiagnostics` |
| `textDocument/didChange` | `lsp_doc_change`, run check, push `publishDiagnostics` |
| `textDocument/didClose` | `lsp_doc_close` |

**Diagnostics push** -- `textDocument/publishDiagnostics` is a server
notification (no `id`):

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": { "uri": "...", "diagnostics": [...] }
}
```

**Running the compiler on in-memory text:**

The compiler's `compile_to_c` reads a file from disk. For the LSP server,
write the in-memory text to a temp file, run `compile_to_c` on it, then
collect diagnostics via `diag_lsp_begin/flush/end`. The file path in
diagnostics is replaced with the original URI path before serialization.

Alternatively (more correct long-term): factor out a
`compile_source_to_c(const char *src, size_t len, const char *display_path, Buf *out)`
function in `main.c` that accepts in-memory source. This avoids temp files.

For MVP: temp file approach is fine.

### Adding `tur lsp` to `src/main.c`

```c
if (strcmp(cmd, "lsp") == 0) {
    diag_init(false);   // no color -- stdout is reserved for JSON-RPC
    lsp_server_run(STDIN_FILENO, STDOUT_FILENO);
    return 0;
}
```

---

## Phase 3: VSCode Extension Update

### `vscode-syntax-ext/package.json` changes

```json
{
  "engines": { "vscode": "^1.82.0" },
  "dependencies": {
    "vscode-languageclient": "^9.0.1"
  },
  "contributes": {
    "configuration": {
      "title": "Turmeric",
      "properties": {
        "turmeric.serverPath": {
          "type": "string",
          "default": "tur",
          "description": "Path to the tur executable"
        }
      }
    }
  }
}
```

### `vscode-syntax-ext/extension.js` changes

Keep the existing formatter. Add a `LanguageClient` that launches `tur lsp`:

```javascript
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
    // --- keep existing formatter ---

    // --- LSP client ---
    const config = vscode.workspace.getConfiguration('turmeric');
    const serverPath = config.get('serverPath', 'tur');

    const serverOptions = {
        command: serverPath,
        args: ['lsp'],
        transport: TransportKind.stdio,
    };
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'turmeric' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.tur'),
        },
    };

    client = new LanguageClient(
        'turmeric-lsp', 'Turmeric Language Server',
        serverOptions, clientOptions,
    );
    client.start();
    context.subscriptions.push({ dispose: () => client.stop() });
}

function deactivate() {
    if (client) return client.stop();
}
module.exports = { activate, deactivate };
```

The `vscode-languageclient` library handles all JSON-RPC framing and error
surfaces failures in the Output panel automatically.

---

## Phase 4: Build System (`src/CMakeLists.txt`)

Add the LSP sources to `TUR_CORE_SOURCES` (they become part of the `tur_core`
OBJECT library, which is linked into the `tur` executable):

```cmake
# LSP server (stdio JSON-RPC -- part of the tur binary)
set(LSP_SOURCES
    lsp/lsp_io.c
    lsp/lsp_json.c
    lsp/lsp_docs.c
    lsp/lsp.c
)
list(APPEND TUR_CORE_SOURCES ${LSP_SOURCES})
```

Add `${CMAKE_CURRENT_SOURCE_DIR}/lsp` to `TUR_INCLUDE_DIRS` so
`#include "lsp.h"` resolves from `main.c`.

---

## Implementation Order

1. **Build system** -- add `src/lsp/` to CMakeLists.txt with stub `.c` files
   that compile clean. Keeps the build green throughout.
2. **`diag_lsp_*` API** -- add to `diag.h`/`diag.c`. Unit-testable
   independently.
3. **`tur check --json`** -- wire `use_json_output` into `diag_lsp_begin/flush`
   inside the `check` handler in `main.c`. Verify with a test file.
4. **LSP I/O layer** -- `lsp_io.c`. Test with a hardcoded response to
   `initialize`.
5. **LSP document store** -- `lsp_docs.c`.
6. **LSP server + handlers** -- `lsp.c`. Wire up `initialize`,
   `didOpen`/`didChange` (diagnostics), `didClose`.
7. **VSCode extension** -- last, after `tur lsp` responds to `initialize`
   correctly.

---

## Files Changed / Created

| File | Status | Purpose |
|---|---|---|
| `src/compiler/diag.h` | Modified | Add `diag_lsp_begin/flush/end` |
| `src/compiler/diag.c` | Modified | Implement LSP accumulation mode |
| `src/main.c` | Modified | `tur check --json` fix + `tur lsp` subcommand |
| `src/CMakeLists.txt` | Modified | Add `src/lsp/` sources |
| `src/lsp/lsp_io.h/c` | New | JSON-RPC Content-Length framing |
| `src/lsp/lsp_json.h/c` | New | Minimal JSON builder + key extractor |
| `src/lsp/lsp_docs.h/c` | New | URI -> document hash map |
| `src/lsp/lsp.h/c` | New | LSP server main loop + handlers |
| `vscode-syntax-ext/extension.js` | Modified | Add LanguageClient |
| `vscode-syntax-ext/package.json` | Modified | Add vscode-languageclient dep |
