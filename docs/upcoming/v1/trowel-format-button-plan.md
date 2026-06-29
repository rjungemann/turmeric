# Trowel: Command Bar Button to Format Selected Text or Document -- Plan

**Status:** Proposed

**Last updated:** 2026-06-28

---

## North star

**Make formatting code in Trowel effortless, instantaneous, and fully integrated with the editing toolbar.**

A Trowel user should be able to click a single "Format" button (or trigger a shortcut) to instantly reformat their code according to standard Turmeric styling. The tool should behave intelligently depending on whether the user has active selections:

1. **Selection Mode:** If there is a text selection, only the selected text is reformatted.
2. **Document Mode:** If there is no selection, the entire document is reformatted.
3. **Smart Dialect Detection:** Code in `.tur.sweet` or `.sweet` files is formatted using the `sweet-exp` dialect, while default `.tur` files use standard `turmeric`.
4. **Non-blocking Execution:** The formatting process runs asynchronously in a background thread to prevent Trowel's UI from freezing during formatting.
5. **Robust Error Handling:** If the formatter encounters a syntax error, it displays the error in the Trowel error pane without modifying the user's document.

---

## Technical Design & Strategy

### 1. Document Range & Selection Handling
Using Lite XL / Trowel `Doc` methods:
- Determine if the user has an active selection using `l1, c1, l2, c2 = doc:get_selection()`.
- Active selection is detected if `(l1 ~= l2) or (c1 ~= c2)`.
- If a selection is active, extract the selected text via `doc:get_text(l1, c1, l2, c2)`.
- If no selection is active, extract the full document text via `table.concat(doc.lines, "")`.

### 2. Formatter Execution via `tur fmt`
We invoke the local `tur` binary using Lite XL's `process` API:
- Command: `tur fmt --stdin` (with `--lang sweet-exp` if the file has a `.sweet` suffix or the syntax dialect dictates it).
- **Process Communication:**
  1. Start the subprocess with `stdin`, `stdout`, and `stderr` redirected to pipes.
  2. Write the text to be formatted using `proc:write(text)`.
  3. Close the standard input pipe using `proc:close_stream(1)` to signal **EOF** so `tur fmt` knows the input stream is finished and can process the code.
  4. Spin up a background thread using `core.add_thread` to non-blockingly read from `proc:read_stdout()` and `proc:read_stderr()`.
  5. Check `proc:return_code()`. If `0`, proceed with replacing the text. If non-zero, surface the error via `core.error()`.

### 3. Document Replacement & Undo History
To replace the text cleanly and preserve the undo stack as a single mergeable action:
- If a selection was active:
  - Set selection back to `l1, c1, l2, c2` to ensure precise targeting.
  - Call `doc:text_input(formatted)` to replace the selected range.
- If the entire document is being replaced:
  - Select the entire document using `doc:set_selection(1, 1, #doc.lines, #doc.lines[#doc.lines] + 1)`.
  - Call `doc:text_input(formatted)` to replace the selection with the fully formatted text.
  - Restore the cursor position to `l1, c1` so the user's view does not jump unexpectedly.

### 4. UI / Command Bar Integration
We integrate the button into Trowel's command bar (ToolbarView):
- **Standard ToolbarView Integration:** If `plugins.toolbarview` is loaded, add a new button with a dedicated icon (e.g., `▩`) and tooltip `"Format Selection or Document (Cmd+Shift+F)"`.
- **Self-contained Fallback ToolbarView:** Append a new button entry to the fallback `TurmericToolbarView`'s `self.buttons` collection:
  ```lua
  { text = " ▩  Format ", command = "turmeric:format-document", color = { 180, 100, 240 } }
  ```

---

## Detailed Lua Implementation Outline

The following snippet shows the structural implementation to be integrated into `tools/trowel/turmeric.lua`:

```lua
-- -------------------------------------------------------------------------
-- Format Document Command
-- -------------------------------------------------------------------------

command.add("core.docview", {
  ["turmeric:format-document"] = function()
    local doc = nearest_doc()
    if not doc then return end

    -- Only format Turmeric files
    if not (doc.syntax and doc.syntax.name == "Turmeric") then
      return
    end

    local l1, c1, l2, c2 = doc:get_selection()
    local has_selection = (l1 ~= l2) or (c1 ~= c2)

    local text
    if has_selection then
      text = doc:get_text(l1, c1, l2, c2)
    else
      text = table.concat(doc.lines, "")
    end

    -- Determine dialect
    local is_sweet = doc.filename and (doc.filename:match("%.sweet$") or doc.filename:match("%.tur%.sweet$"))
    local args = { "fmt", "--stdin" }
    if is_sweet then
      table.insert(args, "--lang")
      table.insert(args, "sweet-exp")
    end

    local cmd = { config.plugins.turmeric.tur }
    for _, a in ipairs(args) do
      table.insert(cmd, a)
    end

    local ok, proc = pcall(process.start, cmd, {
      stdin = process.REDIRECT_PIPE,
      stdout = process.REDIRECT_PIPE,
      stderr = process.REDIRECT_PIPE
    })

    if not ok then
      core.error("turmeric: failed to spawn formatter: " .. tostring(proc))
      return
    end

    -- Write code and close stdin to signal EOF
    proc:write(text)
    proc:close_stream(1)

    core.add_thread(function()
      local formatted = ""
      local stderr_output = ""
      while proc:running() do
        local out = proc:read_stdout(8192)
        local err = proc:read_stderr(8192)
        if out then formatted = formatted .. out end
        if err then stderr_output = stderr_output .. err end
        coroutine.yield(0.01)
      end

      -- Flush any remaining output
      while true do
        local out = proc:read_stdout(8192)
        local err = proc:read_stderr(8192)
        local has_data = false
        if out and #out > 0 then
          formatted = formatted .. out
          has_data = true
        end
        if err and #err > 0 then
          stderr_output = stderr_output .. err
          has_data = true
        end
        if not has_data then break end
      end

      local code = proc:return_code()
      if code ~= 0 then
        if stderr_output ~= "" then
          core.error("turmeric formatter: " .. stderr_output:gsub("\n$", ""))
        else
          core.error("turmeric formatter failed with exit code " .. tostring(code))
        end
        return
      end

      if formatted == text then
        core.log("turmeric: already formatted")
        return
      end

      -- Perform surgical replacement within a single undo-merge context
      if has_selection then
        doc:set_selection(l1, c1, l2, c2)
        doc:text_input(formatted)
        core.log("turmeric: formatted selection")
      else
        doc:set_selection(1, 1, #doc.lines, #doc.lines[#doc.lines] + 1)
        doc:text_input(formatted)
        doc:set_selection(l1, c1, l1, c1) -- Restore cursor
        core.log("turmeric: formatted document")
      end
    end)
  end
})

-- Bind command to a hotkey (Cmd+Shift+F or Ctrl+Shift+F)
keymap.add {
  ["cmd+shift+f"]  = "turmeric:format-document",
  ["ctrl+shift+f"] = "turmeric:format-document",
}
```

---

## Validation & Testing Plan

### 1. Verification of the `process:close_stream` EOF signaling
Verify that `proc:close_stream(1)` successfully triggers the formatter execution on stdin, and that output streams are non-blockingly consumed.

### 2. Selection Formatting Smoke Test
1. Open Trowel, create a scratchpad buffer or open a `.tur` file.
2. Introduce mismatched or ugly indentation within a small helper block:
   ```turmeric
   (defn add [a :int b :int] :int
         (+ a
     b))
   ```
3. Select only that helper block and click the "Format" button or press `Cmd+Shift+F`.
4. Verify that only the selection is reformatted to:
   ```turmeric
   (defn add [a :int b :int] :int
     (+ a b))
   ```
5. Confirm that surrounding unselected parts of the document remain untouched.

### 3. Full Document Formatting Smoke Test
1. With no active selection, click the "Format" button or press `Cmd+Shift+F`.
2. Verify that the entire document is reformatted cleanly.
3. Confirm that the cursor is restored to its pre-formatting location.

### 4. Dialect Support Verification
- Open a `.tur.sweet` file, write sweet-exp code with non-standard spacing, and verify that triggering format formats it as `sweet-exp` dialect (using `--lang sweet-exp`).
- Confirm standard `.tur` files default to the standard parenthesis-based dialect formatting.

### 5. Error Path & Undo Resiliency
- Introduce a syntax error (e.g. unclosed parenthesis `(defn foo [`) and click "Format".
- Verify that a clear error message from `tur fmt` is printed to Trowel's error logs, and the document is not modified.
- Verify that on a successful reformat, triggering "Undo" (`Cmd+Z` / `Ctrl+Z`) reverts the formatting change in exactly one step.
