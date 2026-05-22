# LSP Hover, Go-to-Definition, and Completion -- Plan (LD0--LD4)

> **Status:** Not started. The existing LSP server handles diagnostics
> (textDocument/didOpen, textDocument/didChange) but advertises
> `hoverProvider: false`, `definitionProvider: false`, and no
> completionProvider. This plan adds all three.
>
> **Prerequisites:** Working LSP server (Phases 1--4 of the original
> lsp-plan, already complete). The `Binding` struct already carries a `Span`
> (file_id, line, col_start, col_end, byte offsets); the infrastructure for
> go-to-definition is largely present.
>
> **Last updated:** 2026-05-22

---

## Motivation

The current LSP server surfaces parse/type errors as diagnostics but offers no
editorial intelligence. Three capabilities -- hover, go-to-definition, and
completion -- account for the bulk of day-to-day editor value and are the
natural next step after diagnostics.

- **Hover** (`textDocument/hover`): show a symbol's type and docstring when
  the user mouses over it. Removes the need to look up function signatures in
  the docs browser.
- **Go-to-definition** (`textDocument/definition`): jump to where a name is
  defined. Indispensable for navigating stdlib imports and multi-file projects.
- **Completion** (`textDocument/completion`): suggest symbol names as the user
  types. Reduces typos and makes the stdlib discoverable.

All three share the same prerequisite: a **symbol index** built after each
compilation that maps names to their type, source location, and docstring.

---

## Architecture

### Symbol index (prerequisite for all three features)

After every successful (or partially-successful) elaboration pass, the
compiler exports a flat array of `LspSymbol` records stored inside `LspDoc`:

```c
typedef struct {
    char   name[128];       /* interned name */
    char   type_str[256];   /* rendered type, e.g. "(-> :int :int)" */
    char   doc[512];        /* first ;;; block above definition, or "" */
    char   file_path[512];  /* filesystem path of defining file */
    int    line;            /* 1-based line of the `defn`/`defstruct`/... */
    int    col_start;       /* 1-based column */
    int    col_end;
} LspSymbol;
```

The index is rebuilt on every `textDocument/didOpen` and
`textDocument/didChange` notification, immediately after the existing
diagnostics pass. It is stored as a heap-allocated `LspSymbol *symbols` +
`int symbol_count` inside the existing `LspDoc` struct.

### Docstring extraction

Docstrings are not currently preserved past parsing. Two extraction strategies
are required depending on context:

1. **Re-scan approach (LD0)**: After writing the temp file, scan it linearly
   for `;;;` comment blocks immediately preceding a `(defn`, `(defmacro`,
   `(defstruct`, or `(definstance` token. Build a `char*` array indexed by
   definition start-line. This keeps all docstring logic in the LSP layer and
   requires no changes to the parser.

2. **Compiler-assisted approach (future)**: Thread docstrings through the
   elaborator into `Binding.doc_comment`. This is cleaner long-term but is
   deferred -- the re-scan approach is sufficient for LD0--LD4.

### Word-under-cursor

Hover and definition both need to identify the symbol name at a given
(line, character) position. A helper `lsp_word_at_pos(text, line, col,
out_name, name_cap)` scans the document text for the word boundaries around
the given offset. Turmeric identifiers include alphanumerics plus
`-`, `?`, `!`, `*`, `/`, `>`, `<`, `=`. The helper returns the extracted
name and its span within the source.

---

## Phase LD0 -- Docstring Re-scanner

**Goal:** Implement `lsp_scan_docs(text, text_len, out_table)` -- a standalone
pass over raw source text that associates `;;;` blocks with the definition
names they precede.

### Tasks

- [ ] In `src/lsp/lsp_docs.c`, add `lsp_scan_docs(const char *text, size_t
  len, LspDocTable *out)`.
- [ ] `LspDocTable` is a fixed-capacity array (initial 256) of
  `{ char name[128]; char doc[512]; }` pairs. When full, double capacity with
  `realloc`.
- [ ] The scanner is a simple line-by-line loop:
  - Accumulate consecutive lines that start with `;;;` into a rolling
    `doc_buf[512]`.
  - On a line that matches `(def(n|macro|struct|instance) NAME`, record
    `{ NAME, doc_buf }` into the table.
  - Any non-`;;;`, non-blank line that is not a definition resets `doc_buf`.
- [ ] Expose `lsp_scan_docs_lookup(LspDocTable*, const char *name, char *out,
  size_t cap)` -- copies the doc string for `name` into `out`, returns 1 on
  hit, 0 on miss.
- [ ] Unit-test the scanner against a fixture file in
  `tests/lsp/docscanner_test.c` covering: no docstring, single-line docstring,
  multi-section docstring, reset on non-`;;;` gap.

---

## Phase LD1 -- Symbol Index

**Goal:** After each diagnostics pass, build a `LspSymbol[]` index and attach
it to `LspDoc`.

### Tasks

- [ ] Add `LspSymbol *symbols; int symbol_count; int symbol_cap;` fields to
  `LspDoc` in `src/lsp/lsp_docs.h`.
- [ ] Add `lsp_doc_free_symbols(LspDoc*)` to zero and free the symbol array.
  Call it at the start of each diagnostics pass so stale data never lingers.
- [ ] Export a new compiler entry point `tur_collect_symbols(const char
  *source_path, LspSymbol *out, int cap, int *count_out)` in
  `src/compiler/check.c`. This runs the same elaboration pipeline as
  `tur_check_only()` but, on completion, iterates the top-level binding table
  and fills `out[]`. Include:
  - Name (from `Binding.name->name`)
  - Type string (render via existing `type_to_str()` or equivalent)
  - Span fields (from `Binding.span`)
  - Doc string (look up in a `LspDocTable` populated by `lsp_scan_docs`)
- [ ] In `lsp.c`, after the existing `diag_lsp_*` call sequence in
  `on_did_open()` and `on_did_change()`, call `tur_collect_symbols()` and store
  the result in the `LspDoc`. The temp-file path already available from the
  diagnostics pass is reused -- no second temp file needed.
- [ ] Update `lsp_doc_close()` to call `lsp_doc_free_symbols()`.

---

## Phase LD2 -- Hover

**Goal:** Handle `textDocument/hover`; return type + docstring as Markdown.

### Tasks

- [ ] In `on_initialize()`, set `"hoverProvider": true`.
- [ ] Add handler `on_hover(id, params_json)` in `lsp.c`.
  - Extract `textDocument.uri` and `position.{line, character}` from params.
  - Look up the `LspDoc` for the URI.
  - Call `lsp_word_at_pos(doc->text, doc->text_len, line+1, character+1,
    name, sizeof name)` (converts from 0-based LSP coords to 1-based).
  - Search `doc->symbols[0..symbol_count]` for an entry whose `name` matches.
  - If found, format the response body:
    ```json
    {
      "contents": {
        "kind": "markdown",
        "value": "```\n(name : type)\n```\n\ndocstring here"
      }
    }
    ```
  - If not found, respond with `"contents": ""` (hover with no content).
- [ ] Add `lsp_word_at_pos()` helper in `src/lsp/lsp_util.c` (new file, added
  to CMakeLists.txt). Turmeric identifier characters: `[A-Za-z0-9\-?!*/><+=_]`.
- [ ] Manual test: open a `.tur` file in a hover-capable editor (e.g. VS Code
  with a generic LSP client), hover over `cons`, `map`, `filter` -- confirm
  type + docstring appear.

---

## Phase LD3 -- Go-to-Definition

**Goal:** Handle `textDocument/definition`; return the source location of a
binding.

### Tasks

- [ ] In `on_initialize()`, set `"definitionProvider": true`.
- [ ] Add handler `on_definition(id, params_json)` in `lsp.c`.
  - Extract URI and cursor position identically to `on_hover`.
  - Resolve `name` with `lsp_word_at_pos`.
  - Search `doc->symbols` for a match.
  - If found, build an LSP `Location` response:
    ```json
    {
      "uri": "file:///absolute/path/to/file.tur",
      "range": {
        "start": { "line": LINE_0BASED, "character": COL_0BASED },
        "end":   { "line": LINE_0BASED, "character": COL_END_0BASED }
      }
    }
    ```
    Convert `LspSymbol.line`/`col_start`/`col_end` (1-based) to 0-based.
    Reconstruct the `file://` URI from `LspSymbol.file_path`.
  - If not found, respond with `null` (no definition).
- [ ] Handle cross-file definitions: `LspSymbol.file_path` is the path from
  `Binding.span.file_id` resolved via the compiler's file-id table. The symbol
  index phase (LD1) must store the resolved path string, not just the numeric
  `file_id`.
- [ ] Test: go-to-definition within a single file, and across a `(import
  stdlib/list)` boundary.

---

## Phase LD4 -- Completion

**Goal:** Handle `textDocument/completion`; return all known symbols as
completion items.

### Tasks

- [ ] In `on_initialize()`, add:
  ```json
  "completionProvider": {
    "triggerCharacters": ["(", " "]
  }
  ```
- [ ] Add handler `on_completion(id, params_json)` in `lsp.c`.
  - Extract URI and cursor position.
  - Optionally extract the partial word being typed with `lsp_word_at_pos`
    (for prefix filtering -- see below).
  - Build a JSON array of `CompletionItem` objects, one per `LspSymbol`:
    ```json
    {
      "label":         "map",
      "kind":          3,
      "detail":        "(-> (-> :a :b) (List :a) (List :b))",
      "documentation": { "kind": "markdown", "value": "map -- apply f to each element..." }
    }
    ```
    Item kind 3 = Function; kind 6 = Variable. Determine by checking whether
    `type_str` starts with `(->`.
  - For large stdlib imports the list may be long; cap at 200 items (LSP
    clients handle pagination).
- [ ] Prefix filtering: if the word under cursor is non-empty, only emit items
  whose `label` starts with that prefix (case-insensitive). This keeps the
  list relevant without server-side ranking.
- [ ] Completion for import paths: when the cursor is inside a `(import ...)` 
  form, emit known stdlib module names (`stdlib/list`, `stdlib/option`, etc.)
  as kind-9 (Module) items. A static list of known module names is sufficient
  for now.
- [ ] Test: trigger completion after `(` in a `.tur` file and confirm stdlib
  names appear with correct types and docstrings.

---

## File Changes Summary

| File | Change |
|------|--------|
| `src/lsp/lsp.c` | Add `on_hover`, `on_definition`, `on_completion` handlers; wire into dispatch; update `on_initialize` capabilities |
| `src/lsp/lsp_docs.h` | Add `symbols`, `symbol_count`, `symbol_cap` fields to `LspDoc` |
| `src/lsp/lsp_docs.c` | Add `lsp_doc_free_symbols`; call it in `lsp_doc_close` and at start of diagnostics pass |
| `src/lsp/lsp_util.c` (new) | `lsp_word_at_pos` helper |
| `src/lsp/lsp_util.h` (new) | Header for `lsp_util.c` |
| `src/lsp/lsp_docs.c` | Add `lsp_scan_docs`, `lsp_scan_docs_lookup`, `LspDocTable` |
| `src/compiler/check.c` | Add `tur_collect_symbols` entry point |
| `src/CMakeLists.txt` | Add `lsp_util.c` to LSP source list |
| `tests/lsp/docscanner_test.c` (new) | Unit tests for docstring re-scanner |
| `docs/guides/lsp-guide.md` | Update to document new capabilities and test procedure |

---

## Open Questions

1. **Incremental re-scan cost**: `tur_collect_symbols` re-runs elaboration on
   every keystroke (same as diagnostics today). This is acceptable for small
   files but may lag on very large ones. A future optimisation is to cache the
   symbol index and skip re-elaboration when the edit is outside a definition
   boundary.

2. **Multi-file projects**: The current LSP temp-file approach gives the
   compiler only one file at a time; imported modules are resolved from disk.
   Symbol index entries for imported names come from their on-disk spans, which
   is correct for go-to-definition but means edits in imported files won't
   reflect in the index until the importing file is re-checked. Full workspace
   indexing is a separate project.

3. **Renamed / shadowed bindings**: Turmeric allows shadowing. The symbol index
   stores all top-level names; local shadowing is not tracked. Hover and
   go-to-definition will always resolve to the top-level binding, which may
   surprise users inside `let` blocks. Deferred to a future incremental-
   elaboration phase.
