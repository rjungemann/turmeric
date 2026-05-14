# Turmeric Auto-Formatter Plan

## Overview

A `turformat` formatter for Turmeric source code, exposed via the WASM module so
the web REPL can offer a Format button alongside Run / Clear / Share.

Turmeric is a Lisp-family language whose entire surface syntax is S-expressions,
so formatting is well-understood: the reader already produces a full `Form` tree
with exact span information, and `form_print` already exists as a skeleton
printer. The formatter is a pretty-printer pass that walks the same `Form` tree.

---

## Architecture

```
source text
    │
    ▼
read_all()          ← existing reader (src/reader.c)
    │
    ▼
Form** tree         ← existing AST (src/forms.h)
    │
    ▼
fmt_print()         ← NEW pretty-printer (src/fmt.c / src/fmt.h)
    │
    ▼
formatted text      ← returned to caller
```

The formatter never touches the elaborator, type checker, or code generator.  It
is a pure text-in / text-out transform on the reader output, so it works on
syntactically valid code regardless of type errors.

---

## Phase 1 — C pretty-printer (`src/fmt.c`)

### 1.1 New files

| File | Purpose |
|------|---------|
| `src/fmt.h` | Public API: `fmt_print`, `fmt_options` |
| `src/fmt.c` | Pretty-printer implementation |

### 1.2 Public API

```c
typedef struct FmtOptions {
    uint32_t indent_width;       /* spaces per indent level (default 2) */
    uint32_t line_width;         /* max line length before wrapping (default 80) */
    bool     align_let_bindings; /* align binding names in let blocks */
} FmtOptions;

/* Format all top-level forms in `forms` and write the result into `buf`.
 * Returns 0 on success, -1 if any form could not be printed. */
int fmt_print(Buf *buf, Form **forms, uint32_t count, FmtOptions opts);
```

### 1.3 Formatting rules

#### Indentation model

Two-space indentation by default (configurable).  A form fits "inline" (on one
line) if its rendered width ≤ `line_width - current_column`.  Otherwise it is
broken into a "block" layout.

#### Special forms

These forms have well-known argument roles and get custom layouts:

| Head symbol | Layout rule |
|---|---|
| `defn` | `(defn name params :ret\n  body…)` |
| `defmacro` | same as `defn` |
| `fn` | `(fn params\n  body…)` |
| `let` | `(let [bindings…]\n  body…)` — each binding pair on its own line |
| `if` | `(if test\n  then\n  else)` |
| `when` / `unless` | `(when test\n  body…)` |
| `do` | `(do\n  form…)` |
| `case` | `(case expr\n  pat result…)` — each arm on its own line |
| `loop` | `(loop [bindings]\n  body…)` |
| `handle` | `(handle expr\n  arm…)` |
| `defclass` | `(defclass Name [params]\n  method…)` |
| `definstance` | `(definstance Name Class Type\n  impl…)` |
| `defeffect` | `(defeffect Name [] :ret)` |

#### Regular function calls

- If the whole form fits on the current line, emit it inline.
- Otherwise emit the head and first argument on the same line, then indent
  remaining arguments by `indent_width` from the opening paren.

#### Vectors `[…]`

- Inline if total width fits.
- Otherwise one element per line, indented by 1 space from the opening bracket.

#### Maps `#{…}` and sets `#s(…)`

- Inline if total width fits.
- Otherwise key-value pairs (maps) or elements (sets) each on their own line.

#### Comments

- `;; top-level comments` are emitted on their own line with no leading spaces.
- `; inline comments` after an expression are preserved on the same line.
- A blank line before a top-level `defn` / `defclass` is preserved (one blank
  line max between top-level forms; two before a comment section header that
  starts with `;;`).

#### Type annotations

- `:keyword` type annotations stay on the same line as the expression they
  annotate and are never line-wrapped independently.

#### Quote / quasiquote sugar

- `'x` → `'x` (not `(quote x)`)
- `` `x `` → `` `x ``
- `,x` → `,x`
- `,@x` → `,@x`

#### C code blocks

- ` ```c … ``` ` blocks are emitted verbatim; the formatter does not reformat
  embedded C.

---

## Phase 2 — WASM entry point

Add a new exported function to `src/wasm_glue.c`:

```c
/* Format a Turmeric source string.
 * Returns a malloc'd string with the formatted output, or NULL on parse error.
 * The error message (if any) is written to the existing print/printErr callbacks.
 * Caller must free the result with turi_wasm_free_string(). */
char *turi_wasm_format(const char *input);
```

The implementation:
1. Calls `read_all()` with a scratch arena.
2. Calls `fmt_print()` into a `Buf`.
3. Returns `turi_wasm_strdup(buf.data)`.

Add the function signature to `src/wasm_glue.h` and export it in
`CMakeLists.txt` (the `EXPORTED_FUNCTIONS` list for the WASM target).

---

## Phase 3 — Web UI button

### 3.1 HTML (`web/index.html`)

Add a Format button in `.editor-actions`, between the Clear and Share buttons:

```html
<button class="btn btn-icon" id="format-btn" title="Format (Alt+Shift+F)">
  <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
    <!-- Magic wand / sparkle icon -->
    <path d="M19 3H5c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0
             2-.9 2-2V5c0-1.1-.9-2-2-2zm-7 3l1.5 3H17l-2.5 1.8
             1 3L13 12.2l-2.5 1.8 1-3L9 9h3.5L13 6zm-6 10h12v2H6v-2z"/>
  </svg>
  Format
</button>
```

### 3.2 JavaScript (`web/main.js`)

Add a `formatCode()` function:

```js
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
```

Wire the button and keyboard shortcut in `initEventListeners()`:

```js
document.getElementById('format-btn')?.addEventListener('click', formatCode);

editor.addCommand(
    monaco.KeyMod.Alt | monaco.KeyMod.Shift | monaco.KeyCode.KeyF,
    () => formatCode()
);
```

### 3.3 Monaco document formatter (optional, nice-to-have)

Register a `DocumentFormattingEditProvider` so that the built-in
`editor.action.formatDocument` command (the same one triggered by the Monaco
context menu "Format Document") also calls `formatCode`:

```js
monaco.languages.registerDocumentFormattingEditProvider('turmeric', {
    provideDocumentFormattingEdits(model) {
        // formatCode() is async; for the provider we return a promise
        // Monaco 0.34+ accepts a thenable from this method.
        return formatCode().then(() => []);
        // (formatCode updates the editor value directly via setValue,
        //  so we return an empty edit list — Monaco sees no diff to apply.)
    }
});
```

---

## Phase 4 — CLI flag

Add a `--format` / `-f` flag to the `tur` CLI (`src/main.c`) that reads stdin
or a file path, formats it, and writes to stdout.  This lets editors and CI
pipelines call the formatter without a WASM build.

```
tur --format myfile.tur          # format file in place (writes to stdout)
tur --format --check myfile.tur  # exit 1 if file is not already formatted
cat myfile.tur | tur --format    # read from stdin
```

---

## Phase 5 — VS Code extension integration

The VS Code syntax extension (`vscode-syntax-ext/`) can register the CLI as a
document formatter for `.tur` files:

```json
// package.json (contributes)
"contributes": {
  "commands": [
    { "command": "turmeric.formatDocument", "title": "Format Turmeric Document" }
  ]
}
```

```ts
// extension.ts
vscode.languages.registerDocumentFormattingEditProvider('turmeric', {
    provideDocumentFormattingEdits(document) {
        const formatted = child_process.execSync(
            `tur --format`, { input: document.getText() }
        ).toString();
        const fullRange = new vscode.Range(
            document.positionAt(0),
            document.positionAt(document.getText().length)
        );
        return [vscode.TextEdit.replace(fullRange, formatted)];
    }
});
```

---

## Test plan

### Unit tests (C, `tests/`)

- Round-trip property: `format(format(src)) == format(src)` for all stdlib files.
- Idempotency test script: `tests/test-format.sh` runs `tur --format` on each
  `stdlib/*.tur` and `examples/*.tur` file and asserts the output is stable.
- Specific layout tests for each special form listed in §1.3.

### Integration tests

- Playwright / smoke test in `web/`: click Format button, assert editor content
  changes for a known unformatted input.

---

## Implementation order

1. `src/fmt.h` + `src/fmt.c` — pure C pretty-printer, tested via CLI.
2. `--format` CLI flag in `src/main.c`.
3. `turi_wasm_format` in `src/wasm_glue.c`, rebuild WASM.
4. `formatCode()` in `web/main.js` + Format button in `web/index.html`.
5. Monaco `DocumentFormattingEditProvider`.
6. VS Code extension formatter registration.
7. `tests/test-format.sh` idempotency suite.

---

## Open questions

- **Comment attachment**: Comments are whitespace in the reader and don't appear
  in the `Form` tree.  The formatter must either (a) re-scan the original source
  alongside the form tree using span offsets to re-insert comments, or (b) extend
  the reader to produce comment nodes.  Option (a) is simpler and avoids touching
  the reader; option (b) is cleaner long-term.  Start with (a).
- **Trailing newline**: Always emit exactly one trailing newline at end of file.
- **Max blank lines**: Collapse runs of more than two blank lines to two.
- **Align `let` bindings**: Whether to vertically align the value column in `let`
  blocks is a stylistic choice; make it opt-in via `FmtOptions.align_let_bindings`.
