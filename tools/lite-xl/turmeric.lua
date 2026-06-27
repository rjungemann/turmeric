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
local common = require "core.common"
local keymap = require "core.keymap"
local syntax = require "core.syntax"
local process = require "process"

-- -------------------------------------------------------------------------
-- Syntax
-- -------------------------------------------------------------------------

syntax.add {
  name = "Turmeric",
  files = { "%.tur$", "%.tur%.sweet$" },
  comment = ";",
  patterns = {
    -- Inline-C fence: keep the whole ```c ... ``` block visually quiet
    -- by tagging the fence itself; body is rendered as the default
    -- "string" style so users can tell C apart from Turmeric at a glance.
    { pattern = { "```c", "```" },                type = "string" },
    -- Strings (double-quoted with backslash escapes).
    { pattern = { '"', '"', '\\' },              type = "string" },
    -- Docstring (;;; ... line) takes precedence over plain comments and
    -- gets the `keyword2` slot so themes can render it distinctly.
    { pattern = ";;;.-\n",                        type = "keyword2" },
    { pattern = ";.-\n",                          type = "comment" },
    -- Data literal headers: #map{...}, #set{...}, #row{...}, #fx{...},
    -- #lang sweet-exp, #refine{...}. Just tag the head; the contents
    -- follow normal rules thanks to the standalone brace tokens.
    { pattern = "#%a[%w%-]*%f[{]",                type = "literal" },
    -- Numeric literals -- floats first so 7.1 wins over 7 then .1.
    { pattern = "-?%d+%.%d+",                     type = "number" },
    { pattern = "-?%.%d+",                        type = "number" },
    { pattern = "-?%d+",                          type = "number" },
    -- Turmeric keyword literals: :name and :ns/name.
    { pattern = ":[%a_][%w_%-/]*",                type = "literal" },
    -- Curly-infix braces are their own tokens so themes can highlight
    -- {a + b} arithmetic distinctly from plain s-expr parens.
    { pattern = "[{}]",                           type = "operator" },
    -- Sweet-exp rest-of-line marker `$`.
    { pattern = "%$",                             type = "keyword" },
    -- Operators / delimiters worth coloring.
    { pattern = "[%(%)%[%]]",                     type = "operator" },
    -- Identifier shapes -- allow Lisp-y chars in names (-, ?, !, *, /,
    -- +, -, =, <, >, ').
    { pattern = "[%a_][%w_%-%?!%*/%+=<>']*",      type = "symbol" },
  },
  symbols = {
    -- Special forms / binding forms.
    ["defn"]        = "keyword",
    ["defmacro"]    = "keyword",
    ["defstruct"]   = "keyword",
    ["defopaque"]   = "keyword",
    ["definstance"] = "keyword",
    ["defclass"]    = "keyword",
    ["defmodule"]   = "keyword",
    ["def"]         = "keyword",
    ["fn"]          = "keyword",
    ["let"]         = "keyword",
    ["loop"]        = "keyword",
    ["recur"]       = "keyword",
    ["if"]          = "keyword",
    ["when"]        = "keyword",
    ["unless"]      = "keyword",
    ["cond"]        = "keyword",
    ["do"]          = "keyword",
    ["for"]         = "keyword",
    ["while"]       = "keyword",
    ["match"]       = "keyword",
    ["import"]      = "keyword",
    ["export"]      = "keyword",
    ["extern-c"]    = "keyword",
    ["lambda"]      = "keyword",
    -- Primitive types.
    ["int"]         = "keyword2",
    ["float"]       = "keyword2",
    ["bool"]        = "keyword2",
    ["cstr"]        = "keyword2",
    ["void"]        = "keyword2",
    ["byte"]        = "keyword2",
    ["char"]        = "keyword2",
    -- Type constructors used in annotations.
    ["option"]      = "keyword2",
    ["result"]      = "keyword2",
    ["list"]        = "keyword2",
    ["vec"]         = "keyword2",
    ["map"]         = "keyword2",
    ["set"]         = "keyword2",
    -- Boolean / nil literals.
    ["true"]        = "literal",
    ["false"]       = "literal",
    ["nil-value"]   = "literal",
    ["nil"]         = "literal",
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

-- -------------------------------------------------------------------------
-- New project (Phase 1.5)
-- -------------------------------------------------------------------------

config.plugins.turmeric.init_cmd = config.plugins.turmeric.init_cmd or "tur"
config.plugins.turmeric.init_subcommand = config.plugins.turmeric.init_subcommand or "init"

local function project_root()
  return core.project_dir or system.absolute_path(".") or "."
end

-- Scaffold a Turmeric project at <parent>/<name> by shelling out to
-- `tur init [flags] <name>`. On success, open build.tur and src/main.tur
-- in new buffers and switch the project root to the new directory.
local function new_project(extra_flags, label)
  core.command_view:enter("New Turmeric project name", {
    submit = function(name)
      name = (name or ""):gsub("^%s+", ""):gsub("%s+$", "")
      if name == "" then
        core.error("turmeric: project name cannot be empty")
        return
      end
      local parent = project_root()
      local target = parent .. "/" .. name
      local info = system.get_file_info(target)
      if info then
        core.error("turmeric: %s already exists", target)
        return
      end
      -- tur init scaffolds INTO cwd (not into a subdir named after the
      -- project), so we create the target dir and cd into it ourselves.
      local mkdir_ok, mkdir_err = common.mkdirp(target)
      if not mkdir_ok then
        core.error("turmeric: cannot create %s: %s", target, mkdir_err)
        return
      end

      local cmd = { config.plugins.turmeric.init_cmd, config.plugins.turmeric.init_subcommand }
      for _, f in ipairs(extra_flags or {}) do table.insert(cmd, f) end
      table.insert(cmd, name)

      local ok, proc = pcall(process.start, cmd, {
        cwd = target,
        stdin = process.REDIRECT_DISCARD,
        stdout = process.REDIRECT_PIPE,
        stderr = process.REDIRECT_PIPE,
      })
      if not ok then
        core.error("turmeric: failed to spawn tur init: %s", tostring(proc))
        return
      end
      core.log("turmeric: %s %s in %s", label or "init", name, parent)

      core.add_thread(function()
        while proc:running() do
          local out = proc:read_stdout(4096)
          local err = proc:read_stderr(4096)
          if out and #out > 0 then
            for line in out:gmatch("[^\n]+") do core.log("%s", line) end
          end
          if err and #err > 0 then
            for line in err:gmatch("[^\n]+") do core.error("%s", line) end
          end
          coroutine.yield(0.05)
        end
        local rc = proc:returncode() or -1
        if rc ~= 0 then
          core.error("turmeric: tur init exit %d (project not scaffolded)", rc)
          return
        end

        -- Open the manifest and main source if they exist. tur init writes
        -- build.tur by default; --sweet writes build.tur.sweet.
        local manifest = target .. "/build.tur"
        if not system.get_file_info(manifest) then
          manifest = target .. "/build.tur.sweet"
        end
        local main_src = target .. "/src/main.tur"
        for _, path in ipairs({ manifest, main_src }) do
          if system.get_file_info(path) then
            core.root_view:open_doc(core.open_doc(path))
          end
        end
        core.log("turmeric: project ready at %s", target)
      end)
    end,
  })
end

command.add(nil, {
  ["turmeric:new-project"]      = function() new_project({},          "init") end,
  ["turmeric:new-project-sweet"]= function() new_project({"--sweet"}, "init --sweet") end,
  ["turmeric:new-library"]      = function() new_project({"--lib"},   "init --lib") end,
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
