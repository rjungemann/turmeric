# Lite XL Turmeric Editor Enhancements Plan

## Goal

Add three major user-experience (UX) enhancements to the Lite XL editor integration for Turmeric (`tools/lite-xl/turmeric.lua`):
1. **Lisp-style Word Selection**: Make double-clicking on hyphenated variable names (e.g., `make-struct` or `compose-lens`) select the entire name rather than stopping at the hyphen.
2. **Context-Aware Auto-Indentation**: Adjust newline indentation dynamically based on the current style. Parenthesized Turmeric files (`.tur`) should use Lisp-aware double-space matching for open delimiters, while Sweet-expression files (`.tur.sweet`) should preserve or increment block indentation.
3. **Toolbar Integration (ToolbarView)**: Integrate a custom run-control toolbar using Lite XL's standard `ToolbarView` API, adding a "Play" button to build/execute files and a "Console" button to open/toggle the ReplView pane.

---

## 1. Lisp-style Word Selection & Hyphens

### Current Behavior
In stock Lite XL, double-clicking on an identifier like `compose-lens` selects only `compose` or `lens`, because the hyphen `-` is defined as a non-word character (like `+`, `*`, `/`, etc.) in the global `config.non_word_chars`.

### Technical Strategy
In Lite XL, word boundaries and double-click selection targets are resolved by the `Doc:is_non_word_char(char)` method of the document. Instead of altering `config.non_word_chars` globally (which would break selection in C, Python, or Markdown files), we can override this method specifically on Turmeric documents.

When a document's syntax is `"Turmeric"`, we will dynamically shadow `Doc:is_non_word_char` to exclude characters like `-`, `?`, `!`, `*`, `/`, `+`, `=`, `<`, `>`, and `'`, which are standard, valid characters in Turmeric and Lisp identifiers.

### Code Sketch (Plugin Injection)
```lua
local Doc = require "core.doc"

-- Intercept Doc:is_non_word_char
local orig_is_non_word_char = Doc.is_non_word_char
function Doc:is_non_word_char(char)
  if self.syntax and self.syntax.name == "Turmeric" then
    -- Treat hyphens, question marks, and Lisp operator symbols as word characters
    local lisp_chars = {
      ["-"] = true, ["?"] = true, ["!"] = true, ["*"] = true, ["/"] = true,
      ["+"] = true, ["="] = true, ["<"] = true, [">"] = true, ["'"] = true
    }
    if lisp_chars[char] then
      return false
    end
  end
  return orig_is_non_word_char(self, char)
end
```

---

## 2. Context-Aware Auto-Indentation

### Current Behavior
When pressing Enter, Lite XL copies the exact indentation level of the previous line. This works for flat line-by-line languages, but causes severe misalignment in deeply nested Lisp code and disrupts block transitions in Sweet-expression indentation.

### Technical Strategy
We will override the default `newline` action in `tools/lite-xl/turmeric.lua` when the active document has `"Turmeric"` syntax. The behavior will fork based on the file type:

#### A. Parenthesized Lisp Indentation (`.tur` files)
When pressing Enter inside a `.tur` file, the indentation of the new line should align with open delimiters:
1. Scan backward from the cursor to find the nearest unmatched opening delimiter (`(`, `[`, `{`).
2. If found, calculate the horizontal column offset of that delimiter.
3. If there is text immediately following that open delimiter on the same line, align the new line to start directly under that first argument (Standard Lisp List Alignment).
4. If the open delimiter is the last character on its line (or is followed only by whitespace/comments), indent the new line by the current indentation level **plus 2 spaces** (Lisp Body Indentation).

#### B. Sweet-expression Indentation (`.tur.sweet` or `.sweet` files)
For Sweet-expressions, indentation defines block structure. When pressing Enter:
1. Read the previous non-blank line.
2. If the line ends with an opening delimiter (`(`, `[`, `{`), increase the indentation by **2 spaces** (or the tab-width).
3. If the previous line starts with block-starting keywords (like `defn`, `let`, `loop`, `if`, `cond`, `match`), or ends with `$` (rest-of-line marker), increase the indentation of the new line by **2 spaces** to open the block.
4. Otherwise, copy the previous line's indentation level verbatim.

---

## 3. Toolbar Integration (ToolbarView)

To improve accessibility and replicate a full "Studio" IDE feeling, we will implement a graphical toolbar at the top of the editor.

### API Reference & Integration
Lite XL provides an optional `ToolbarView` component. We will integrate it dynamically in the Turmeric plugin, ensuring that the editor falls back gracefully if the toolbar package is not installed.

```lua
-- Safe requirement of the ToolbarView plugin
local status, ToolbarView = pcall(require, "plugins.toolbar")
if status then
  -- Add Turmeric actions to the toolbar
  ToolbarView:add_button {
    icon = "play",               -- Standard FontAwesome play icon or similar
    command = "turmeric:run-file",
    tooltip = "Run Turmeric File (Cmd+R)"
  }
  ToolbarView:add_button {
    icon = "terminal",           -- Terminal / Console icon
    command = "turmeric:toggle-repl",
    tooltip = "Toggle REPL Pane"
  }
end
```

### Gritty Details
1. **Button Registration**: Register the buttons using the `ToolbarView:add` API in the plugin's `init` or post-init phase.
2. **Visual Aesthetics**:
   - The "Play" button should render with a green highlight or accent color when hovered.
   - The "REPL Console" button should use a dark grey / terminal slate accent.
3. **Icons & Fallbacks**:
   - If Lite XL is compiled with FontAwesome/MaterialIcons, specify the glyphs (e.g. `\u{f04b}` for play, `\u{f120}` for terminal).
   - If stock Lite XL text icons are used, provide clean text-based fallbacks (e.g., `▶` for Run, `>_` for REPL).

---

## Actionable Milestones & Tasks

### Milestone 1: Lisp Word Character Selection
- [ ] Implement the `Doc:is_non_word_char` interceptor in `tools/lite-xl/turmeric.lua`.
- [ ] Validate that double-clicking `compose-lens` selects the whole word in `.tur` files.
- [ ] Verify that C/C++ files retain their standard hyphen-operator selection boundaries.

### Milestone 2: Indentation System
- [ ] Implement a lightweight parenthesis/bracket nesting tracker in Lua.
- [ ] Overwrite the `keymap` binding for `enter` to trigger custom `turmeric:indent-newline`.
- [ ] Write tests for both `.tur` (double-space open paren match) and `.sweet` (increment on keywords/delimiters).

### Milestone 3: Graphical Toolbar
- [ ] Hook into Lite XL's standard `ToolbarView` lifecycle.
- [ ] Create robust font-character and text-character fallbacks for the buttons.
- [ ] Bind "Play" to `cmd+r` (`turmeric:run-file`) and "Console" to the REPL toggle action.
