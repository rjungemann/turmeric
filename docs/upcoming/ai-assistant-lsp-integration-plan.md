# AI Assistant LSP Integration Plan

**Goal**: Make Turmeric's language server useful to AI coding assistants
(Claude Code, GitHub Copilot, OpenCode) so they can type-check, navigate, and
understand Turmeric source without having to reinvent the wheel.

---

## 1. Status Quo

`tur lsp` is a JSON-RPC 2.0 / LSP server running over stdio.  It currently
handles:

| LSP method | Notes |
|---|---|
| `initialize` / `shutdown` / `exit` | Full lifecycle |
| `textDocument/didOpen`, `didChange`, `didClose` | Full-sync (kind 1) |
| `textDocument/publishDiagnostics` | Type errors, parse errors |
| `textDocument/hover` | Symbol type + `;;;` docstring |
| `textDocument/definition` | Jump to definition in open doc |
| `textDocument/completion` | Symbols in scope + stdlib import paths |

The server is invoked as `tur lsp` and is already documented in
`docs/guides/lsp-guide.md` with configuration snippets for Neovim, Vim, Emacs,
Helix, VS Code, and Zed.

---

## 2. Gap Analysis

AI assistants interact with a codebase in two ways:

1. **As a language client** -- they open files and use LSP to get diagnostics,
   hover, completion, and navigation exactly like a human editor would.
2. **As a tool-caller** -- they invoke discrete tools (`check_file`,
   `look_up_symbol`, `format_file`, ...) to gather structured information
   that they feed into a prompt.

The LSP covers path 1 reasonably well already.  Path 2 requires a separate
**MCP (Model Context Protocol) server** -- the emerging standard for exposing
structured tools to AI models.

### Missing LSP capabilities (affect path 1)

| LSP method | Priority | Notes |
|---|---|---|
| `textDocument/documentSymbol` | High | Gives AI a structural outline of any open file |
| `workspace/symbol` | High | Project-wide symbol search |
| `textDocument/signatureHelp` | Medium | Active parameter hint while AI writes call sites |
| `textDocument/inlayHint` | Medium | Inferred type annotations inline |
| `textDocument/codeAction` | Medium | "Quick fix" suggestions (import missing module, add type annotation) |
| `textDocument/semanticTokens` | Low | Richer token classification; Copilot uses this for context |
| `textDocument/references` | Low | Find all call-sites of a symbol |

### Missing MCP tools (path 2, new work)

A `tur mcp` subcommand would start a minimal MCP server over stdio.  Proposed
tools:

| MCP tool | Inputs | Returns |
|---|---|---|
| `check_file` | `path: string` | Diagnostics array (same data as LSP publishDiagnostics) |
| `symbols` | `path: string` | Full symbol list with name, type, docstring, location |
| `hover` | `path, line, col` | Hover text at cursor position |
| `definition` | `path, line, col` | Definition location |
| `complete` | `path, line, col` | Completion candidates with type signatures |
| `doc` | `name: string` | `;;;` docstring for a named symbol |
| `format` | `path: string` | Formatted source text (delegates to `tur format`) |
| `build` | `dir: string` | Compile project; returns diagnostics + exit status |

MCP is JSON-RPC 2.0 with a small schema layer on top, so the existing
`lsp_io.c` framing and `lsp_json.c` helpers can be reused almost verbatim.
The core logic is already in `src/lsp/lsp.c`; wrapping it as MCP tools is
mostly a dispatch layer.

---

## 3. Phase 1 -- Fill Missing LSP Capabilities

Implement the "High" priority LSP methods so AI clients that act purely as
LSP consumers get better context.

### 3a. `textDocument/documentSymbol`

Walk the symbol index already built by `tur_collect_symbols` and emit an
array of `DocumentSymbol` objects.  The symbol kind mapping:

| Turmeric form | LSP SymbolKind |
|---|---|
| `defn` | `Function` (12) |
| `defstruct` | `Struct` (23) |
| `defmacro` | `Operator` (25) |
| `definstance` | `Interface` (11) |
| `def` (value) | `Variable` (13) |
| `defmodule` | `Module` (2) |

Implementation sits in `src/lsp/lsp.c` alongside `on_hover`.  The `LspDoc`
already stores the symbol list; no new analysis pass needed.

### 3b. `workspace/symbol`

Same symbol index, but across all open documents.  The query string filters
symbols whose name contains the query (case-insensitive prefix match is
sufficient).  Response: `WorkspaceSymbol[]`.

The open-document map is already managed by `lsp_docs.c`; iterate it,
concatenate results, filter, and return.

### 3c. `textDocument/signatureHelp`

When the cursor is inside a function call (detected by walking backwards
through the text to find the opening `(`), look up the callee's type in the
symbol index and emit a `SignatureInformation` with one parameter per
positional argument.  The type string already rendered in `LspSymbol.type_str`
carries parameter types; parse it to extract them.

---

## 4. Phase 2 -- MCP Server (`tur mcp`)

### 4a. Architecture

```
stdin  ──►  lsp_io_read_message()  ──►  mcp_dispatch()  ──►  lsp_io_write_message()  ──►  stdout
                                              │
                              ┌───────────────┼──────────────────────────────────┐
                              ▼               ▼                                  ▼
                      run_doc_analysis()  on_hover()  ...    tur_format()   tur_check_only()
```

The framing layer (`lsp_io.c`) is identical.  Add a new entry point
`mcp_server_run(int fd_in, int fd_out)` in `src/lsp/mcp.c` that handles
the MCP initialize handshake and then dispatches `tools/call` requests.

### 4b. MCP initialize response

```json
{
  "protocolVersion": "2024-11-05",
  "capabilities": { "tools": {} },
  "serverInfo": { "name": "turmeric", "version": "TURMERIC_VERSION_STRING" }
}
```

### 4c. `tools/list` response

Return the table from §2 as MCP `Tool` objects, each with a `name`,
`description`, and `inputSchema` (JSON Schema object).

### 4d. `tools/call` dispatch

Map each tool name to its implementation:

- `check_file` → `tur_check_only(path)` + `diag_lsp_*` collection
- `symbols` → `tur_collect_symbols(path, ...)`
- `hover` / `definition` / `complete` → the existing `on_hover` / `on_definition` / `on_completion` logic, factored out of the LSP path so both share it
- `doc` → `lsp_scan_docs` + lookup
- `format` → shell out to `tur format <path>` or call the internal formatter
- `build` → shell out to `tur build <dir>`, capture stdout/stderr

### 4e. New CLI entry point

Add to `src/main.c` alongside the existing `lsp` subcommand:

```c
} else if (strcmp(argv[1], "mcp") == 0) {
    mcp_server_run(STDIN_FILENO, STDOUT_FILENO);
    return 0;
}
```

---

## 5. Phase 3 -- Per-Tool Configuration

### 5a. Claude Code

Claude Code picks up MCP servers from `.claude/mcp.json` in the project root.
Add this file to the repository so anyone who opens the project in Claude Code
gets Turmeric intelligence automatically:

```json
{
  "mcpServers": {
    "turmeric": {
      "command": "tur",
      "args": ["mcp"],
      "env": {}
    }
  }
}
```

Add a note to `docs/guides/lsp-guide.md` explaining that Claude Code users get
MCP tools out of the box after `just build`.

### 5b. GitHub Copilot (VS Code)

Copilot in VS Code drives intelligence through the LSP, not MCP, so the
priority here is:

1. Ship a first-class `vscode-languageclient` integration inside
   `vscode-syntax-ext/` so the extension activates `tur lsp` automatically
   (no `multi-lsp` shim required).
2. Once the VS Code extension starts the LSP, Copilot's context window
   receives hover, completions, and diagnostics passively.
3. For Copilot **Chat** / agent mode, also wire up the MCP server via VS Code's
   `mcp` configuration block in `.vscode/mcp.json`:

```json
{
  "servers": {
    "turmeric": {
      "type": "stdio",
      "command": "tur",
      "args": ["mcp"]
    }
  }
}
```

The VS Code extension work consists of adding `vscode-languageclient` as a
dependency and calling `new LanguageClient(...)` in `extension.ts`; the
`vscode-guide.md` should be updated accordingly.

### 5c. OpenCode

OpenCode reads MCP server configuration from `opencode.json` at the project
root.  Add:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "turmeric": {
      "type": "local",
      "command": ["tur", "mcp"]
    }
  }
}
```

With this in place, OpenCode's agent can call `check_file`, `symbols`, and
`build` to understand the project state before suggesting edits.

---

## 6. Implementation Order

1. **Phase 1a** (`documentSymbol`) -- lowest risk, highest AI value.  The symbol
   index is already built; this is a serialization task only.
2. **Phase 1b** (`workspace/symbol`) -- one loop over open docs, same output
   format.
3. **Phase 2** (MCP) -- new file `src/lsp/mcp.c`, reuses all existing logic.
   Adds `tur mcp` CLI entry.
4. **Phase 3** -- config files only; no C code required.
5. **Phase 1c** (`signatureHelp`) and subsequent LSP features -- incrementally,
   as MCP adoption validates the approach.

---

## 7. Testing Strategy

- Extend `tests/lsp/` with a new JSON-RPC harness that exercises
  `documentSymbol` and `workspace/symbol` responses against a fixture `.tur`
  file.
- Add a parallel `tests/mcp/` directory with a similar harness for MCP
  `tools/call` round-trips.  The harness sends a `tools/call` request and
  asserts the response JSON matches an expected snapshot.
- The existing `docscanner_test.c` pattern (spin up the server, write requests,
  compare responses) applies directly to both.

---

## 8. Open Questions

- **Multi-file projects**: `tur_collect_symbols` currently analyses one file at
  a time.  For `workspace/symbol` and cross-file go-to-definition, a
  project-wide index is needed.  This overlaps with the planned module graph
  work; defer until that lands.
- **Incremental analysis**: Full-sync LSP (kind 1) re-checks the whole file on
  every keystroke.  For MCP tools this is fine (tools are called on demand).
  For the LSP path, incremental (kind 2) would reduce latency on large files.
- **Security of `build` tool**: `tur mcp` accepting an arbitrary `dir` argument
  and shelling out to `tur build` carries obvious risks if the MCP server is
  exposed to an untrusted caller.  Restrict `build` to paths under the
  project root detected at `initialize` time.
- **`tur mcp` vs a separate process**: Some projects run the MCP server as a
  long-lived daemon.  The simplest approach is to keep `tur mcp` stateless
  (start fresh per session), matching how `tur lsp` works today.

---

## See Also

- `docs/guides/lsp-guide.md` -- current LSP editor configuration
- `src/lsp/lsp.c` -- JSON-RPC dispatcher
- `src/lsp/lsp_sym.h` -- `LspSymbol` structure
- `src/lsp/lsp_docs.c` -- per-document state and `;;;` doc scanner
- [Model Context Protocol spec](https://spec.modelcontextprotocol.io)
- [LSP specification](https://microsoft.github.io/language-server-protocol/)
