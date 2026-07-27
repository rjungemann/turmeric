---
title: Language Server (LSP) Guide
category: Editor and IDE
description: Configuring editors to use the Turmeric language server for diagnostics
---

# Language Server Protocol (LSP) Guide

Turmeric ships a built-in language server that speaks
[Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
over stdio (JSON-RPC 2.0). The server is invoked as:

```sh
tur lsp
```

Editors launch it as a subprocess and communicate via stdin/stdout.

## Current capabilities

| Capability | Status |
|---|---|
| Diagnostics (`textDocument/publishDiagnostics`) | Supported |
| Document symbols (`textDocument/documentSymbol`) | Supported |
| Workspace symbols (`workspace/symbol`) | Supported (open documents only) |
| Hover documentation | Supported |
| Go-to-definition | Supported |
| Completion (`textDocument/completion`) | Supported |
| Signature help / formatting / semantic tokens | Not supported |

When you open or edit a `.tur` file, the server compiles it in check-only mode
and publishes any parse or type errors back to the editor as diagnostics
(red underlines, error panel entries, etc.).

Two behaviours worth knowing about:

- **Analysis is debounced.** Compiling is not free and runs on the same thread
  that serves requests, so a changed file is not analyzed until the editor has
  been quiet for ~200ms. Requests that need symbols (hover, completion,
  definition, document symbols) force any pending analysis to run first, so
  this delays diagnostics slightly but never returns a stale answer.
- **Positions are byte offsets.** The server advertises
  `"positionEncoding": "utf-8"` (LSP 3.17), so `character` in a `Position`
  counts UTF-8 bytes rather than the UTF-16 code units the specification
  defaults to. Clients that honour the negotiated encoding need no special
  handling; a client that assumes UTF-16 regardless will be off on lines
  containing non-ASCII.

Completion is driven by the symbols the last successful compile produced. A
buffer that does not parse — for example one with an unclosed paren, which is
the normal state mid-keystroke — yields no symbols, and therefore no
completions.

## Editor configuration

### Neovim (nvim-lspconfig)

Add a custom server entry -- `nvim-lspconfig` does not bundle Turmeric by
default, so you register it manually:

```lua
local lspconfig = require("lspconfig")
local configs   = require("lspconfig.configs")

if not configs.turmeric then
  configs.turmeric = {
    default_config = {
      cmd        = { "tur", "lsp" },
      filetypes  = { "tur" },
      root_dir   = lspconfig.util.root_pattern("build.tur", ".git"),
      single_file_support = true,
    },
  }
end

lspconfig.turmeric.setup({})
```

If you use `lazy.nvim` and want this to load alongside `nvim-lspconfig`:

```lua
{
  "neovim/nvim-lspconfig",
  config = function()
    -- paste the block above here
  end,
}
```

Neovim does not auto-detect `.tur` files as `tur` filetype. Add this to your
config or to `~/.config/nvim/ftdetect/tur.lua`:

```lua
vim.filetype.add({ extension = { tur = "tur" } })
```

### Neovim (built-in LSP, no plugin)

```lua
vim.api.nvim_create_autocmd("FileType", {
  pattern = "tur",
  callback = function(ev)
    vim.lsp.start({
      name    = "turmeric",
      cmd     = { "tur", "lsp" },
      root_dir = vim.fs.dirname(
        vim.fs.find({ "build.tur", ".git" }, { upward = true })[1]
      ),
    })
  end,
})
```

### Vim (vim-lsp)

Install [vim-lsp](https://github.com/prabirsheth/vim-lsp), then add:

```vim
if executable('tur')
  au User lsp_setup call lsp#register_server({
    \ 'name': 'turmeric',
    \ 'cmd': {server_info -> ['tur', 'lsp']},
    \ 'allowlist': ['tur'],
    \ })
endif
```

Add filetype detection if not already present:

```vim
augroup TurmericFt
  autocmd!
  autocmd BufRead,BufNewFile *.tur setfiletype tur
augroup END
```

### Vim (ALE)

[ALE](https://github.com/dense-analysis/ale) can drive an LSP binary directly.
Add to your `~/.vim/after/ftplugin/tur.vim` (or equivalent):

```vim
let g:ale_linters = { 'tur': ['turmeric_lsp'] }
```

Then register the linter in your vimrc or in
`~/.vim/ale_linters/tur/turmeric_lsp.vim`:

```vim
call ale#linter#Define('tur', {
\   'name':            'turmeric_lsp',
\   'lsp':             'stdio',
\   'executable':      'tur',
\   'command':         '%e lsp',
\   'project_root':    function('ale#util#FindProjectRoot'),
\   'language':        'tur',
\ })
```

### VS Code

The bundled VS Code extension (`vscode-syntax-ext/`) provides syntax
highlighting only. To add LSP diagnostics, install a generic LSP client such as
[vscode-glspc](https://marketplace.visualstudio.com/items?itemName=coc-extensions.coc-glspc)
or configure `vscode-languageclient` in your own extension wrapper.

A minimal `settings.json` entry using the
[multi-lsp](https://marketplace.visualstudio.com/items?itemName=iehiehieh.multi-lsp)
extension:

```json
{
  "multi-lsp.servers": [
    {
      "language": "tur",
      "command": "tur",
      "args": ["lsp"]
    }
  ]
}
```

The bundled extension in `vscode-syntax-ext/` already speaks LSP natively via
`vscode-languageclient`, spawning `tur lsp` over stdio; the `turmeric.serverPath`
setting overrides which `tur` it uses. The multi-lsp recipe above is only needed
if you would rather drive the server yourself.

### Emacs (eglot)

[Eglot](https://github.com/joaotavora/eglot) is built into Emacs 29+. Add a
major mode for Turmeric first (or use `lisp-mode` as a fallback), then register
the server:

```emacs-lisp
;; Simple major mode derived from lisp-mode
(define-derived-mode turmeric-mode lisp-mode "Turmeric"
  "Major mode for Turmeric source files.")
(add-to-list 'auto-mode-alist '("\\.tur\\'" . turmeric-mode))

;; Register the LSP server with eglot
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(turmeric-mode . ("tur" "lsp"))))

;; Auto-start eglot when opening .tur files
(add-hook 'turmeric-mode-hook #'eglot-ensure)
```

### Emacs (lsp-mode)

```emacs-lisp
(with-eval-after-load 'lsp-mode
  (lsp-register-client
   (make-lsp-client
    :new-connection (lsp-stdio-connection '("tur" "lsp"))
    :activation-fn  (lsp-activate-on "tur")
    :server-id      'turmeric)))

(add-hook 'turmeric-mode-hook #'lsp)
```

### Helix

Add to `~/.config/helix/languages.toml`:

```toml
[[language]]
name              = "turmeric"
scope             = "source.tur"
file-types        = ["tur"]
comment-token     = ";"
indent            = { tab-width = 2, unit = "  " }
language-servers  = ["turmeric-lsp"]

[language-server.turmeric-lsp]
command = "tur"
args    = ["lsp"]
```

### Zed

In `~/.config/zed/settings.json`:

```json
{
  "lsp": {
    "turmeric": {
      "binary": {
        "path": "tur",
        "arguments": ["lsp"]
      }
    }
  }
}
```

Zed also requires a language extension for filetype detection. Until an
official extension is published, use the Zed extension API to register `.tur`
manually or rely on generic highlighting.

## How it works

When a `.tur` file is opened or modified, the server:

1. Writes the current buffer text to a temporary file under `/tmp/`.
2. Runs the Turmeric compiler in type-check-only mode (`tur_check_only`).
3. Collects all diagnostics via the internal `diag_lsp_*` API.
4. Remaps temp-file paths back to the real document URI.
5. Sends a `textDocument/publishDiagnostics` notification to the editor.
6. Deletes the temporary file.

The server uses `TextDocumentSyncKind.Full` (sync kind 1): the entire file
content is sent on every change, not just diffs. This keeps the implementation
simple at the cost of slightly more data per keystroke for large files.

## Troubleshooting

**No diagnostics appear.**
Verify `tur` is on your `$PATH`:

```sh
which tur
tur --version
```

**Server exits immediately.**
Run `tur lsp` in a terminal and type a minimal JSON-RPC initialize request.
Any startup error (missing stdlib, bad install) will appear on stderr.

**Diagnostics are stale or missing after save.**
Some editors only sync on save (not on every keystroke). Check your editor's
LSP sync setting; the server responds to both `textDocument/didOpen` and
`textDocument/didChange`.

## See also

- `src/lsp/lsp.c` -- JSON-RPC dispatcher and diagnostic publisher
- `src/lsp/lsp_docs.c` -- In-memory document store
- `src/lsp/lsp_io.c` -- Framed stdio read/write
- [vscode-guide.md](vscode-guide.md) -- VS Code syntax extension
- [vim-guide.md](vim-guide.md) -- Vim / Neovim syntax highlighting
- [formatter-guide.md](formatter-guide.md) -- `tur format` CLI
