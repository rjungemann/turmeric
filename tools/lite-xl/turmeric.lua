-- mod-version:3
-- Turmeric mode for Lite XL.
--
-- Spike scope:
--   - Syntax highlighting for .tur and .tur.sweet files.
--   - "Turmeric: Run File" command bound to ctrl+r / cmd+r that spawns
--     `tur --interpret <file>` and streams stdout/stderr into Lite XL's
--     LogView (the bottom log pane). Errors with a path:line:col prefix
--     are logged via core.error so they show up clickable.
--   - "Turmeric: Check File" bound to ctrl+shift+r / cmd+shift+r that
--     runs `tur check <file>` the same way.
--
-- Install: drop into ~/.config/lite-xl/plugins/ on macOS/Linux, or
-- %USERPROFILE%\.config\lite-xl\plugins on Windows.

local core = require "core"
local command = require "core.command"
local keymap = require "core.keymap"
local syntax = require "core.syntax"
local process = require "process"

-- -------------------------------------------------------------------------
-- Syntax
-- -------------------------------------------------------------------------

syntax.add {
  name = "Turmeric",
  files = { "%.tur$", "%.tur%.sweet$" },
  comment = ";;",
  patterns = {
    -- Strings (double-quoted with backslash escapes).
    { pattern = { '"', '"', '\\' },              type = "string" },
    -- Triple-semicolon docstring takes precedence over plain comment.
    { pattern = ";;;.*",                          type = "comment" },
    { pattern = ";.*",                            type = "comment" },
    -- Numeric literals.
    { pattern = "-?%d+%.%d+",                     type = "number" },
    { pattern = "-?%.%d+",                        type = "number" },
    { pattern = "-?%d+",                          type = "number" },
    -- Keyword literals (Turmeric :keyword).
    { pattern = ":[%a_][%w_%-]*",                 type = "literal" },
    -- Inline-C fence marker (just the backticks; body remains plain).
    { pattern = "```c",                           type = "keyword2" },
    { pattern = "```",                            type = "keyword2" },
    -- Operators / delimiters worth coloring.
    { pattern = "[%(%)%[%]{}]",                   type = "operator" },
    -- A symbol-shaped function call head: identifier followed by space-args.
    { pattern = "[%a_][%w_%-%?!%*/%+%-=<>]*",     type = "symbol" },
  },
  symbols = {
    -- Special forms / binding forms.
    ["defn"]       = "keyword",
    ["defmacro"]   = "keyword",
    ["defstruct"]  = "keyword",
    ["defopaque"]  = "keyword",
    ["definstance"]= "keyword",
    ["defclass"]   = "keyword",
    ["def"]        = "keyword",
    ["fn"]         = "keyword",
    ["let"]        = "keyword",
    ["if"]         = "keyword",
    ["when"]       = "keyword",
    ["cond"]       = "keyword",
    ["do"]         = "keyword",
    ["for"]        = "keyword",
    ["while"]      = "keyword",
    ["loop"]       = "keyword",
    ["recur"]      = "keyword",
    ["match"]      = "keyword",
    ["import"]     = "keyword",
    ["export"]     = "keyword",
    -- Primitive types.
    ["int"]        = "keyword2",
    ["float"]      = "keyword2",
    ["bool"]       = "keyword2",
    ["cstr"]       = "keyword2",
    ["void"]       = "keyword2",
    -- Literals.
    ["true"]       = "literal",
    ["false"]      = "literal",
    ["nil-value"]  = "literal",
  },
}

-- -------------------------------------------------------------------------
-- Run / Check commands
-- -------------------------------------------------------------------------

-- User-overridable in init.lua via `config.plugins.turmeric.tur = "/abs/path"`.
local config = require "core.config"
config.plugins.turmeric = config.plugins.turmeric or {}
config.plugins.turmeric.tur = config.plugins.turmeric.tur or "tur"

local function nearest_doc()
  if core.active_view and core.active_view.doc then
    return core.active_view.doc
  end
  return nil
end

-- Run `cmd` with the active file as $1, streaming stdout+stderr into the
-- Lite XL log pane. Lines that look like `path:line:col:` are routed
-- through core.error so the LogView highlights them.
local function run_tur(args, label)
  local doc = nearest_doc()
  if not doc or not doc.filename then
    core.error("turmeric: save the buffer first")
    return
  end
  doc:save()
  local path = system.absolute_path(doc.filename)
  local dir = path:match("^(.*)/[^/]+$") or "."
  local cmd = { config.plugins.turmeric.tur }
  for _, a in ipairs(args) do table.insert(cmd, a) end
  table.insert(cmd, path)

  local ok, proc = pcall(process.start, cmd, {
    cwd = dir,
    stdin = process.REDIRECT_DISCARD,
    stdout = process.REDIRECT_PIPE,
    stderr = process.REDIRECT_PIPE,
  })
  if not ok then
    core.error("turmeric: failed to spawn %s: %s", config.plugins.turmeric.tur, tostring(proc))
    return
  end
  core.log("turmeric: %s %s", label, path)

  core.add_thread(function()
    while proc:running() do
      local out = proc:read_stdout(4096)
      local err = proc:read_stderr(4096)
      if out and #out > 0 then
        for line in out:gmatch("[^\n]+") do core.log("%s", line) end
      end
      if err and #err > 0 then
        for line in err:gmatch("[^\n]+") do
          if line:match("^[^:]+:%d+:%d+:") then
            core.error("%s", line)
          else
            core.log("%s", line)
          end
        end
      end
      coroutine.yield(0.05)
    end
    local rc = proc:returncode() or -1
    if rc == 0 then
      core.log("turmeric: %s exit 0", label)
    else
      core.error("turmeric: %s exit %d", label, rc)
    end
  end)
end

command.add(nil, {
  ["turmeric:run-file"] = function()
    -- v1 should use `--interpret` (tree-walking, no codegen) but that
    -- subcommand currently crashes on a null ReaderMacroRegistry; see
    -- docs/reported/tur-interpret-null-reader-macro-registry.md. Falling
    -- back to `tur run` (AOT) until the interpreter is fixed.
    run_tur({ "run" }, "run")
  end,
  ["turmeric:check-file"] = function()
    run_tur({ "check" }, "check")
  end,
})

keymap.add {
  ["cmd+r"]       = "turmeric:run-file",
  ["ctrl+r"]      = "turmeric:run-file",
  ["cmd+shift+r"] = "turmeric:check-file",
  ["ctrl+shift+r"]= "turmeric:check-file",
}

-- -------------------------------------------------------------------------
-- Sidebar visibility -- Processing-style "file vs project" launch
-- -------------------------------------------------------------------------
--
-- Rule: if every command-line arg is a file (no directories), hide the
-- TreeView so single-file launches feel like opening a sketch.
-- Opening a directory (or no args at all in a project root) leaves the
-- TreeView visible. The user can still toggle it with ctrl+\ / cmd+\.
local function any_dir_arg()
  for i = 2, #ARGS do
    local arg = ARGS[i]
    if arg and not arg:match("^-") then
      local info = system.get_file_info(arg)
      if info and info.type == "dir" then return true end
    end
  end
  return false
end

if not any_dir_arg() and #ARGS >= 2 then
  local ok, tree = pcall(require, "plugins.treeview")
  if ok and tree then tree.visible = false end
end
