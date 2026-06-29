# Try Turmeric: REPL Send Selection / Current Form at Cursor (Phase L5)

> **Status:** Drafted as a follow-on plan split from the original `repl-load-definitions-plan.md` (where L1-L4 are implemented/landed).

## Goal

Provide a common DrRacket-adjacent and Emacs/SLIME/Calva-like affordance to evaluate the current selection or the enclosing top-level form at the cursor in the active REPL pane on pressing `cmd+enter` (or `ctrl+enter`). This allows quick, interactive prototyping without needing to re-run or re-load the entire file.

## 1. Trigger

- **Keybinding**: `cmd+enter` (macOS) and `ctrl+enter` (Linux/Windows) in a normal code document (`DocView`).
- **Command**: `turmeric:send-form-at-cursor`.

## 2. Walk & Detection Strategy

1. **Selection**: If the user has a non-empty text selection, use that selected text raw.
2. **Form at Cursor**: If the selection is empty, walk the buffer from the current cursor position to identify the boundaries of the enclosing top-level form:
   - Walk backwards in the document to find the nearest unmatched opening parenthesis `(` or bracket `[` at a zero indentation level, or perform a paren-balance walk similar to Trowel's existing indentation logic.
   - Alternatively, identify the full top-level form enclosing the cursor.
   - Extract the substring of this form.

## 3. REPL Transmission & Feedback

- Save the active buffer if it has a file name (to ensure state is consistent).
- Focus/open the `ReplView` (split "down" if closed).
- Wrap the raw form in a scoping construct if necessary, or pass it directly.
- Send the extracted text to the REPL's stdin.
- Echo the sent text into the REPL history pane with a special prefix, e.g. `eval< (println "hello")` or styled in a dim/sys text kind, so the user has immediate visual confirmation of what was evaluated.

## 4. Acceptance Criteria

- With the cursor inside a function definition `(defn add [a b] (+ a b))` or a simple print expression, pressing `cmd+enter` sends only that form to the REPL.
- The REPL evaluates the form and prints the output (and any returned value).
- If text is selected (e.g. `(+ 2 3)`), pressing `cmd+enter` evaluates only the selection.

## 5. Rollout & Safety

- Non-disruptive: The feature only adds a new keybinding/command to `DocView`.
- Revertable: Removing the `cmd+enter` command and the paren-balance walk function from `tools/trowel/turmeric.lua` completely disables the feature without affecting standard file running (`cmd+r`).
