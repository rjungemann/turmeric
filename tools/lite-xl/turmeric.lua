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
    -- v1 target: the tree-walking interpreter. No codegen, no
    -- per-file build cache, fastest edit-run loop.
    run_tur({ "--interpret" }, "interpret")
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
-- Phase 3: lsp-lite helper -- autocomplete + calltips
-- -------------------------------------------------------------------------
--
-- Spawns one long-lived `tur lsp-lite` subprocess on first symbol lookup and
-- talks to it with newline-delimited JSON. Feeds prefix matches into Lite XL's
-- built-in `autocomplete` plugin (when present); falls back to a status-bar
-- ping otherwise. F1 prints the doc for the symbol under the cursor.
--
-- The helper is intentionally line-oriented (one JSON object per line in,
-- one per line out) so the Lua client does not need a JSON framing parser.

config.plugins.turmeric.lsp_cmd = config.plugins.turmeric.lsp_cmd or "lsp-lite"

local lsp_proc = nil
local lsp_buf  = ""       -- partial line accumulator
local lsp_seq  = 0
local lsp_callbacks = {}  -- id -> function(result, err)

local function lsp_start()
  if lsp_proc and lsp_proc:running() then return true end
  local ok, p = pcall(process.start,
    { config.plugins.turmeric.tur, config.plugins.turmeric.lsp_cmd },
    { stdin = process.REDIRECT_PIPE,
      stdout = process.REDIRECT_PIPE,
      stderr = process.REDIRECT_PIPE })
  if not ok then
    core.error("turmeric: failed to spawn lsp-lite: %s", tostring(p))
    return false
  end
  lsp_proc = p
  lsp_buf  = ""
  core.add_thread(function()
    while lsp_proc and lsp_proc:running() do
      local out = lsp_proc:read_stdout(8192)
      if out and #out > 0 then
        lsp_buf = lsp_buf .. out
        while true do
          local nl = lsp_buf:find("\n", 1, true)
          if not nl then break end
          local line = lsp_buf:sub(1, nl - 1)
          lsp_buf = lsp_buf:sub(nl + 1)
          local id = tonumber(line:match('"id"%s*:%s*(%-?%d+)'))
          if id and lsp_callbacks[id] then
            local cb = lsp_callbacks[id]
            lsp_callbacks[id] = nil
            -- Cheap "result" / "error" extraction; lsp-lite responses are
            -- flat (no nested objects in result, ever -- arrays of strings,
            -- bare strings, or "ok"). Strip outer quoting where present.
            local err = line:match('"error"%s*:%s*"(.-)"')
            local result_raw = line:match('"result"%s*:%s*(.+)}%s*$')
            cb(result_raw, err)
          end
        end
      end
      coroutine.yield(0.05)
    end
  end)
  return true
end

local function lsp_send(method, params, cb)
  if not lsp_start() then return end
  lsp_seq = lsp_seq + 1
  local id = lsp_seq
  if cb then lsp_callbacks[id] = cb end
  -- Minimal JSON encoder for the params we send: only string values.
  local parts = {}
  for k, v in pairs(params or {}) do
    local s = tostring(v):gsub("\\", "\\\\"):gsub('"', '\\"')
                         :gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
    table.insert(parts, string.format('"%s":"%s"', k, s))
  end
  local msg = string.format('{"id":%d,"method":"%s","params":{%s}}\n',
                            id, method, table.concat(parts, ","))
  lsp_proc:write(msg)
end

-- Parse a result that is a JSON array of strings like `["a","b","c"]` into
-- a Lua array. Returns {} if the raw string is missing or malformed.
local function parse_string_array(raw)
  -- Walk the raw `["a","b","..."]` looking for "..."-quoted substrings.
  -- Honors backslash escapes so an embedded \" does not terminate an item.
  local out = {}
  if not raw then return out end
  local i, n = 1, #raw
  while i <= n do
    local q = raw:find('"', i, true)
    if not q then break end
    local j = q + 1
    while j <= n do
      local c = raw:sub(j, j)
      if c == '\\' then j = j + 2
      elseif c == '"' then break
      else j = j + 1 end
    end
    table.insert(out, raw:sub(q + 1, j - 1))
    i = j + 1
  end
  return out
end

local function parse_string(raw)
  if not raw then return "" end
  local s = raw:match('^"(.*)"$') or ""
  s = s:gsub("\\n", "\n"):gsub("\\r", "\r"):gsub("\\t", "\t")
       :gsub('\\"', '"'):gsub("\\\\", "\\")
  return s
end

-- Symbol-at-cursor extraction. Turmeric identifiers allow Lisp-y chars.
local function symbol_at_cursor()
  local doc = nearest_doc()
  if not doc then return nil end
  local line, col = doc:get_selection()
  local text = doc.lines[line] or ""
  local i = col - 1
  local function is_sym(c) return c and c:match("[%a%d_%-%?!%*/%+=<>'%./]") end
  while i > 0 and is_sym(text:sub(i, i)) do i = i - 1 end
  local j = col - 1
  while j <= #text and is_sym(text:sub(j, j)) do j = j + 1 end
  local sym = text:sub(i + 1, j - 1)
  return sym ~= "" and sym or nil
end

command.add(nil, {
  ["turmeric:doc-at-cursor"] = function()
    local sym = symbol_at_cursor()
    if not sym then core.error("turmeric: no symbol at cursor"); return end
    lsp_send("doc", { name = sym }, function(raw, err)
      if err then core.error("turmeric: doc lookup failed: %s", err); return end
      local doc_text = parse_string(raw)
      if doc_text == "" then
        core.log("turmeric: no doc for %s", sym)
      else
        core.log("== %s ==\n%s", sym, doc_text)
      end
    end)
  end,
  ["turmeric:calltip-at-cursor"] = function()
    local sym = symbol_at_cursor()
    if not sym then core.error("turmeric: no symbol at cursor"); return end
    lsp_send("calltip", { name = sym }, function(raw, err)
      if err then core.error("turmeric: calltip failed: %s", err); return end
      local s = parse_string(raw)
      if s == "" then core.log("turmeric: no calltip for %s", sym)
      else core.log("%s", s) end
    end)
  end,
})

keymap.add {
  ["f1"]    = "turmeric:doc-at-cursor",
  ["cmd+i"] = "turmeric:doc-at-cursor",
}

-- Push the active buffer text into lsp-lite on save so per-buffer defns
-- show up in completions.
local DocSave = require "core.doc".save
require "core.doc".save = function(self, ...)
  local rc = DocSave(self, ...)
  if self.filename and self.filename:match("%.tur$") then
    lsp_send("update", {
      uri  = "file://" .. (self.abs_filename or self.filename),
      text = table.concat(self.lines, ""),
    })
  end
  return rc
end

-- -------------------------------------------------------------------------
-- Phase 4: REPL pane (via CommandView prompt + LogView output)
-- -------------------------------------------------------------------------
--
-- Lite XL's CommandView + LogView together cover the input/output halves of
-- a REPL surface, so the v1 ReplView is a bridge over those two existing
-- views rather than a bespoke `View` subclass. A long-lived `tur repl`
-- subprocess holds the evaluator state across commands.

config.plugins.turmeric.repl_subcommand = config.plugins.turmeric.repl_subcommand or "repl"

local repl_proc = nil
local repl_buf  = ""

local function repl_drain()
  if not repl_proc then return end
  while true do
    local out = repl_proc:read_stdout(8192)
    local err = repl_proc:read_stderr(8192)
    if (not out or out == "") and (not err or err == "") then break end
    if out and out ~= "" then repl_buf = repl_buf .. out end
    if err and err ~= "" then
      for line in err:gmatch("[^\n]+") do core.error("repl: %s", line) end
    end
  end
end

local function repl_emit()
  if repl_buf == "" then return end
  for line in repl_buf:gmatch("[^\n]+") do core.log("repl> %s", line) end
  repl_buf = ""
end

local function repl_start(quiet)
  if repl_proc and repl_proc:running() then
    if not quiet then core.log("turmeric: repl already running") end
    return true
  end
  local ok, p = pcall(process.start,
    { config.plugins.turmeric.tur, config.plugins.turmeric.repl_subcommand },
    { stdin = process.REDIRECT_PIPE,
      stdout = process.REDIRECT_PIPE,
      stderr = process.REDIRECT_PIPE })
  if not ok then
    core.error("turmeric: failed to spawn tur repl: %s", tostring(p))
    return false
  end
  repl_proc = p
  repl_buf  = ""
  core.add_thread(function()
    while repl_proc and repl_proc:running() do
      repl_drain()
      repl_emit()
      coroutine.yield(0.05)
    end
    repl_drain()
    repl_emit()
    core.log("turmeric: repl exited")
    repl_proc = nil
  end)
  core.log("turmeric: repl started")
  return true
end

local function repl_send(text)
  if not repl_proc or not repl_proc:running() then
    if not repl_start() then return end
  end
  if not text:match("\n$") then text = text .. "\n" end
  repl_proc:write(text)
end

command.add(nil, {
  ["turmeric:start-repl"] = function() repl_start() end,
  ["turmeric:stop-repl"]  = function()
    if repl_proc and repl_proc:running() then
      repl_proc:write(":quit\n")
      repl_proc:terminate()
      core.log("turmeric: repl stopped")
    end
  end,
  ["turmeric:repl-eval"] = function()
    core.command_view:enter("Turmeric expr", {
      submit = function(expr)
        if expr and expr ~= "" then
          core.log("repl< %s", expr)
          repl_send(expr)
        end
      end,
    })
  end,
  ["turmeric:repl-reload-buffer"] = function()
    local doc = nearest_doc()
    if not doc then core.error("turmeric: no active buffer"); return end
    if doc.filename then doc:save() end
    local text = table.concat(doc.lines, "")
    core.log("turmeric: reloading buffer into repl (%d bytes)", #text)
    repl_send(text)
  end,
})

keymap.add {
  ["cmd+shift+l"]  = "turmeric:repl-reload-buffer",
  ["ctrl+shift+l"] = "turmeric:repl-reload-buffer",
  ["cmd+e"]        = "turmeric:repl-eval",
  ["ctrl+e"]       = "turmeric:repl-eval",
}

-- Optional integration with Lite XL's built-in `autocomplete` plugin.
-- It exposes `autocomplete.add({ name=..., files=..., items={...} })`.
-- We seed an empty list and refresh it as the user types.
local ok_ac, autocomplete = pcall(require, "plugins.autocomplete")
if ok_ac and autocomplete and autocomplete.add then
  local items = {}
  autocomplete.add({
    name  = "turmeric",
    files = { "%.tur$", "%.tur%.sweet$" },
    items = items,
  })
  -- Periodically refresh from lsp-lite based on the current word prefix.
  -- Cheap: lsp-lite filters server-side.
  local last_prefix = ""
  core.add_thread(function()
    while true do
      coroutine.yield(0.4)
      local doc = nearest_doc()
      if doc and doc.filename and doc.filename:match("%.tur$") then
        local sym = symbol_at_cursor() or ""
        if #sym >= 2 and sym ~= last_prefix then
          last_prefix = sym
          lsp_send("complete", { prefix = sym }, function(raw, err)
            if err then return end
            for k in pairs(items) do items[k] = nil end
            local arr = parse_string_array(raw)
            for _, n in ipairs(arr) do items[n] = true end
          end)
        end
      end
    end
  end)
end

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
