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
    -- by tagging the fence itself; body is rendered as the nested C
    -- language syntax.
    { pattern = { "```c", "```" },                type = "string", syntax = "C" },
    -- Datum comments: `#;` followed by a balanced collection, string, symbol, or number
    { pattern = "#;%s*%b()",                      type = "comment" },
    { pattern = "#;%s*%b[]",                      type = "comment" },
    { pattern = "#;%s*%b{}",                      type = "comment" },
    { pattern = "#;%s*\"[^\"]*\"",                type = "comment" },
    { pattern = "#;%s*[%a_][%w_%-%?!%*/%+=<>']*", type = "comment" },
    { pattern = "#;%s*-?%d+%.?%d*",               type = "comment" },
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
  -- Try the current active view first
  if core.active_view and core.active_view.doc then
    return core.active_view.doc
  end
  -- Fallback: If focus is temporarily on the Toolbar or another utility view,
  -- query the main editor pane's active view.
  local node = core.root_view:get_active_node_default()
  if node and node.active_view and node.active_view.doc then
    return node.active_view.doc
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

-- turmeric:run-file's implementation is defined after the ReplView (Phase 4)
-- block below, since it has to talk to the running REPL pane. The command is
-- registered there alongside the other ReplView-aware commands. Only
-- turmeric:check-file lives here -- it spawns a one-shot `tur check` and
-- streams diagnostics into the log pane, no REPL involvement.
command.add(nil, {
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
-- Phase 4: REPL pane -- a real ReplView, not a CommandView+LogView bridge
-- -------------------------------------------------------------------------
--
-- A custom `core.view` subclass with its own pane, scrollable output
-- history, and an inline prompt that captures keyboard input. Backed by
-- a long-lived `tur repl` subprocess; output streams in via core.add_thread.

config.plugins.turmeric.repl_subcommand = config.plugins.turmeric.repl_subcommand or "repl"
config.plugins.turmeric.repl_max_lines  = config.plugins.turmeric.repl_max_lines  or 5000

local View      = require "core.view"
local style     = require "core.style"
local renderer  = renderer       -- global in Lite XL
local system    = system         -- global in Lite XL

local ReplView = View:extend()

function ReplView:new()
  ReplView.super.new(self)
  self.scrollable = true
  self.lines      = {}     -- { {text=str, kind="out"|"err"|"in"|"sys"}, ... }
  self.line_buf   = ""     -- partial-line accumulator for stdout
  self.err_buf    = ""     -- partial-line accumulator for stderr
  self.input      = ""     -- current input string
  self.history    = {}     -- list of previously submitted inputs
  self.history_idx= 0      -- 0 = editing fresh input; >0 = browsing
  self.input_draft= ""     -- saved fresh input when browsing history
  self.proc       = nil
  self:start()
end

function ReplView:get_name() return "Turmeric REPL" end

function ReplView:push(text, kind)
  if text == nil or text == "" then return end
  if #self.lines >= config.plugins.turmeric.repl_max_lines then
    table.remove(self.lines, 1)
  end
  table.insert(self.lines, { text = text, kind = kind or "out" })
end

function ReplView:absorb(buf_name, raw, kind)
  -- Split raw stream into complete lines; carry the trailing partial
  -- line over to the next read so we don't fragment escape sequences.
  local buf = self[buf_name] .. (raw or "")
  while true do
    local nl = buf:find("\n", 1, true)
    if not nl then break end
    self:push(buf:sub(1, nl - 1), kind)
    buf = buf:sub(nl + 1)
  end
  self[buf_name] = buf
end

function ReplView:start()
  if self.proc and self.proc:running() then return end
  local ok, p = pcall(process.start,
    { config.plugins.turmeric.tur, config.plugins.turmeric.repl_subcommand },
    { stdin = process.REDIRECT_PIPE,
      stdout = process.REDIRECT_PIPE,
      stderr = process.REDIRECT_PIPE })
  if not ok then
    self:push("failed to spawn tur repl: " .. tostring(p), "err")
    return
  end
  self.proc = p
  self:push("started: " .. config.plugins.turmeric.tur .. " "
            .. config.plugins.turmeric.repl_subcommand, "sys")
  local self_ref = self
  core.add_thread(function()
    while self_ref.proc and self_ref.proc:running() do
      self_ref:absorb("line_buf", self_ref.proc:read_stdout(8192), "out")
      self_ref:absorb("err_buf",  self_ref.proc:read_stderr(8192), "err")
      coroutine.yield(0.05)
    end
    -- Flush whatever the process left behind.
    self_ref:absorb("line_buf", "", "out")
    self_ref:absorb("err_buf",  "", "err")
    if self_ref.line_buf ~= "" then self_ref:push(self_ref.line_buf, "out"); self_ref.line_buf = "" end
    if self_ref.err_buf  ~= "" then self_ref:push(self_ref.err_buf,  "err"); self_ref.err_buf  = "" end
    self_ref:push("repl exited", "sys")
    self_ref.proc = nil
  end)
end

function ReplView:stop()
  if self.proc and self.proc:running() then
    pcall(function() self.proc:write(":quit\n") end)
    self.proc:terminate()
  end
end

function ReplView:try_close(do_close)
  self:stop()
  ReplView.super.try_close(self, do_close)
end

function ReplView:send_input()
  local text = self.input
  if text == "" then return end
  table.insert(self.history, text)
  self.history_idx = 0
  self.input_draft = ""
  self.input = ""
  self:push("> " .. text, "in")
  if not self.proc or not self.proc:running() then self:start() end
  if self.proc then
    pcall(function() self.proc:write(text .. "\n") end)
  end
end

function ReplView:send_buffer_text(text)
  if not self.proc or not self.proc:running() then self:start() end
  self:push("[buffer reload: " .. tostring(#text) .. " bytes]", "sys")
  if self.proc then
    pcall(function() self.proc:write(text)
                     if not text:match("\n$") then self.proc:write("\n") end end)
  end
end

-- DrRacket-style Run: feed `:run <abs-path>` to the running interpreter so
-- the file's top-level forms (and (main) if defined) execute inside the
-- REPL's env. Re-runnable: `:run` resets the session before each load so
-- redefining bindings works without tripping duplicate-defn errors.
-- See docs/notes/tur-repl-reload-semantics.md for why this is needed.
function ReplView:send_run(abs_path)
  if not self.proc or not self.proc:running() then self:start() end
  if not self.proc then return end
  pcall(function() self.proc:write(":run " .. abs_path .. "\n") end)
end

-- Lite XL hooks ------------------------------------------------------------

function ReplView:on_text_input(text)
  self.input = self.input .. text
end

function ReplView:on_mouse_pressed(button, x, y, clicks)
  ReplView.super.on_mouse_pressed(self, button, x, y, clicks)
  core.set_active_view(self)
end

-- Layout: one line per output entry, then the prompt line at the bottom
-- of the visible region (which scrolls with the content).
local function line_h()
  return style.code_font:get_height() + style.padding.y / 2
end

function ReplView:get_scrollable_size()
  return (#self.lines + 1) * line_h() + style.padding.y * 2
end

local KIND_COLOR = {
  out = function() return style.text     end,
  err = function() return style.error or style.accent end,
  ["in"] = function() return style.accent end,
  sys = function() return style.dim or style.text end,
}

function ReplView:draw()
  self:draw_background(style.background)
  local ox, oy = self:get_content_offset()
  local x = ox + style.padding.x
  local y = oy + style.padding.y
  local lh = line_h()
  local fnt = style.code_font

  -- Output history.
  for _, entry in ipairs(self.lines) do
    local color = (KIND_COLOR[entry.kind] or KIND_COLOR.out)()
    renderer.draw_text(fnt, entry.text, x, y, color)
    y = y + lh
  end

  -- Prompt + current input + blinking cursor.
  local prompt_color = style.accent
  local px = x
  px = renderer.draw_text(fnt, "tur> ", px, y, prompt_color)
  px = renderer.draw_text(fnt, self.input, px, y, style.text)

  if core.active_view == self then
    local T = system.get_time()
    if (T - math.floor(T / 0.5) * 0.5) < 0.25 then
      local cw = fnt:get_width("_")
      renderer.draw_rect(px, y, cw, fnt:get_height(), style.caret or style.accent)
    end
  end

  self:draw_scrollbar()
end

function ReplView:update()
  ReplView.super.update(self)
end

-- ReplView registry --------------------------------------------------------

local function find_repl_view()
  -- Locate any existing ReplView already attached to the root view, so
  -- repeat invocations focus it instead of opening duplicates.
  local found
  local function walk(node)
    if not node then return end
    if node.active_view and node.active_view:is(ReplView) then
      found = node.active_view; return
    end
    if node.views then
      for _, v in ipairs(node.views) do
        if v:is(ReplView) then found = v; return end
      end
    end
    if node.a then walk(node.a) end
    if node.b then walk(node.b) end
  end
  walk(core.root_view.root_node)
  return found
end

local function open_repl_view(split_dir)
  local existing = find_repl_view()
  if existing then
    core.set_active_view(existing)
    return existing
  end
  local view = ReplView()
  local node = core.root_view:get_active_node_default()
  if split_dir then
    local new_node = node:split(split_dir)
    new_node:add_view(view)
  else
    node:add_view(view)
  end
  core.set_active_view(view)
  return view
end

local function find_doc_view()
  local found
  local function walk(node)
    if not node then return end
    if node.views then
      for _, v in ipairs(node.views) do
        if v.doc then found = v; return end
      end
    end
    if node.a then walk(node.a) end
    if node.b then walk(node.b) end
  end
  walk(core.root_view.root_node)
  return found
end

local function toggle_repl_view()
  local view = find_repl_view()
  if view then
    if core.active_view == view then
      -- If REPL is currently focused, focus back to the code pane (DocView)
      local doc_view = find_doc_view()
      if doc_view then
        core.set_active_view(doc_view)
      end
    else
      -- If REPL is open but not focused, focus it!
      core.set_active_view(view)
    end
  else
    -- If REPL is closed, open and focus it!
    open_repl_view("down")
  end
end

command.add(nil, {
  ["turmeric:start-repl"] = function() toggle_repl_view() end,
  ["turmeric:stop-repl"]  = function()
    local v = find_repl_view()
    if v then v:stop() end
  end,
  ["turmeric:repl-reload-buffer"] = function()
    local doc = nearest_doc()
    if not doc then core.error("turmeric: no active buffer"); return end
    if doc.filename then doc:save() end
    local text = table.concat(doc.lines, "")
    local view = open_repl_view("down")
    view:send_buffer_text(text)
  end,
  -- DrRacket Run: save buffer, open the REPL pane, ask the running
  -- interpreter to :run the file. Replaces the old `tur --interpret` spawn
  -- so cmd+r leaves every top-level binding visible at the prompt.
  ["turmeric:run-file"] = function()
    local doc = nearest_doc()
    if not doc or not doc.filename then
      core.error("turmeric: save the buffer first")
      return
    end
    doc:save()
    local path = system.absolute_path(doc.filename)
    local view = open_repl_view("down")
    view:send_run(path)
  end,
})

-- ReplView-specific keybindings: only fire when a ReplView has focus, so
-- they don't shadow editor keys when the user is in a normal DocView.
local function repl_predicate()
  return core.active_view and core.active_view:is(ReplView)
end

command.add(repl_predicate, {
  ["turmeric-repl:submit"] = function()
    core.active_view:send_input()
  end,
  ["turmeric-repl:backspace"] = function()
    local v = core.active_view
    if #v.input > 0 then v.input = v.input:sub(1, -2) end
  end,
  ["turmeric-repl:clear-input"] = function()
    core.active_view.input = ""
  end,
  ["turmeric-repl:history-prev"] = function()
    local v = core.active_view
    if #v.history == 0 then return end
    if v.history_idx == 0 then v.input_draft = v.input end
    v.history_idx = math.min(v.history_idx + 1, #v.history)
    v.input = v.history[#v.history - v.history_idx + 1]
  end,
  ["turmeric-repl:history-next"] = function()
    local v = core.active_view
    if v.history_idx == 0 then return end
    v.history_idx = v.history_idx - 1
    if v.history_idx == 0 then
      v.input = v.input_draft
    else
      v.input = v.history[#v.history - v.history_idx + 1]
    end
  end,
})

keymap.add {
  ["cmd+shift+l"]  = "turmeric:repl-reload-buffer",
  ["ctrl+shift+l"] = "turmeric:repl-reload-buffer",
  -- ReplView-only (the predicate above guards against shadowing).
  ["return"]    = "turmeric-repl:submit",
  ["backspace"] = "turmeric-repl:backspace",
  ["ctrl+u"]    = "turmeric-repl:clear-input",
  ["up"]        = "turmeric-repl:history-prev",
  ["down"]      = "turmeric-repl:history-next",
}

-- -------------------------------------------------------------------------
-- Auto-open the REPL on startup
-- -------------------------------------------------------------------------
--
-- DrRacket-style "definitions on top, interactions on the bottom" layout:
-- the REPL pane appears in a horizontal split below the editor as soon as
-- the plugin loads. Opt out with `config.plugins.turmeric.auto_open_repl
-- = false` in init.lua.

config.plugins.turmeric.auto_open_repl = config.plugins.turmeric.auto_open_repl ~= false

if config.plugins.turmeric.auto_open_repl then
  -- Defer one tick so the root view / project tree have finished initial
  -- layout before we split. core.add_thread with no yield runs at the next
  -- frame, which is the canonical "after-startup" hook in Lite XL plugins.
  core.add_thread(function()
    coroutine.yield(0.1)
    open_repl_view("down")
    -- Re-focus the editor so the user can start typing in their file,
    -- not in the REPL prompt.
    local node = core.root_view.root_node
    if node and node.a and node.a.active_view then
      core.set_active_view(node.a.active_view)
    end
  end)
end

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
-- Lisp-style Word Selection & Hyphens
-- -------------------------------------------------------------------------
local Doc = require "core.doc"
local orig_is_non_word_char = Doc.is_non_word_char
function Doc:is_non_word_char(char)
  if self.syntax and self.syntax.name == "Turmeric" then
    -- Treat hyphens, question marks, and Lisp operators as word characters
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

-- -------------------------------------------------------------------------
-- Context-Aware Auto-Indentation
-- -------------------------------------------------------------------------
local function turmeric_doc_predicate()
  return core.active_view and core.active_view.doc and core.active_view.doc.syntax and core.active_view.doc.syntax.name == "Turmeric"
end

command.add(turmeric_doc_predicate, {
  ["turmeric:newline"] = function()
    local dv = core.active_view
    local doc = dv.doc
    local is_sweet = doc.filename and doc.filename:match("%.sweet$")

    -- 1. Grab previous line info before inserting newline
    local line, col = doc:get_selection()
    local prev_line_text = doc.lines[line] or ""

    -- 2. Call default newline command to insert \n and carry previous indent
    command.perform("docview:newline")

    -- Now we are on the new line. Let's get our new cursor position
    local r_line, r_col = doc:get_selection()

    if is_sweet then
      -- --- Sweet-exp Auto-Indentation ---
      local prev_clean = prev_line_text:gsub("%s+$", "") -- trim trailing whitespace
      local ends_with_open = prev_clean:match("[%(%[%{%$]$")
      local starts_with_block = prev_clean:match("^%s*defn%A") or
                                prev_clean:match("^%s*let%A") or
                                prev_clean:match("^%s*loop%A") or
                                prev_clean:match("^%s*if%A") or
                                prev_clean:match("^%s*cond%A") or
                                prev_clean:match("^%s*match%A")
      if ends_with_open or starts_with_block then
        doc:text_input("  ")
      end
    else
      -- --- Parenthesized Lisp Auto-Indentation ---
      local start_scan = math.max(1, r_line - 50)
      local unmatched_pos = nil

      -- We'll track nested delimiters
      local stack = {}
      for l = r_line - 1, start_scan, -1 do
        local text = doc.lines[l] or ""
        -- Scan character by character from the end of the line backwards
        for idx = #text, 1, -1 do
          local c = text:sub(idx, idx)
          if c == ')' or c == ']' or c == '}' then
            table.insert(stack, c)
          elseif c == '(' or c == '[' or c == '{' then
            if #stack > 0 then
              table.remove(stack) -- closed
            else
              -- Found an unmatched opening delimiter!
              unmatched_pos = { line = l, col = idx }
              break
            end
          end
        end
        if unmatched_pos then break end
      end

      if unmatched_pos then
        -- If we found an unmatched opening delimiter:
        -- Let's see if there is any non-space text after it on its line.
        local delim_line_text = doc.lines[unmatched_pos.line] or ""
        local after_delim = delim_line_text:sub(unmatched_pos.col + 1)
        local first_arg_idx = after_delim:find("%S")

        -- Delete the default copied indentation on the current line first
        local current_line_text = doc.lines[r_line] or ""
        local copied_indent = current_line_text:match("^%s*") or ""
        if #copied_indent > 0 then
          doc:set_selection(r_line, 1, r_line, #copied_indent + 1)
          doc:delete_to()
        end

        if first_arg_idx then
          -- Standard Lisp List Alignment: Align with the first argument
          local target_indent_size = unmatched_pos.col + first_arg_idx - 1
          doc:text_input(string.rep(" ", target_indent_size))
        else
          -- Lisp Body Indentation: Align with the open delimiter's line's indent + 2 spaces
          local base_indent = delim_line_text:match("^%s*") or ""
          doc:text_input(base_indent .. "  ")
        end
      end
    end
  end,
})

keymap.add {
  ["return"] = "turmeric:newline"
}

-- -------------------------------------------------------------------------
-- ToolbarView Integration
-- -------------------------------------------------------------------------
local ok_tb, toolbar = pcall(require, "plugins.toolbarview")
if ok_tb and toolbar and type(toolbar) == "table" and toolbar.add_item then
  -- --- Integration with existing plugins.toolbarview ---
  core.add_thread(function()
    coroutine.yield(0.2)
    pcall(function()
      toolbar:add_item({
        icon = "▶",
        command = "turmeric:run-file",
        tooltip = "Run File (Cmd+R)"
      })
      toolbar:add_item({
        icon = ">_",
        command = "turmeric:start-repl",
        tooltip = "Open REPL Pane"
      })
    end)
  end)
else
  -- --- Self-contained Custom ToolbarView Fallback ---
  local TurmericToolbarView = View:extend()

  function TurmericToolbarView:new()
    TurmericToolbarView.super.new(self)
    self.size.y = 30
    self.buttons = {
      { text = " ▶  Run ", command = "turmeric:run-file", color = { 60, 180, 60 } },
      { text = " >_ REPL ", command = "turmeric:start-repl", color = { 100, 140, 240 } },
    }
    self.hovered_idx = nil
  end

  function TurmericToolbarView:get_name() return "Toolbar" end

  function TurmericToolbarView:draw()
    self:draw_background(style.background3 or style.background)
    local ox, oy = self:get_content_offset()
    local lh = style.code_font:get_height()
    local padding = style.padding.x
    local x = ox + padding
    local y = oy + (self.size.y - lh) / 2
    local fnt = style.code_font

    for i, btn in ipairs(self.buttons) do
      local w = fnt:get_width(btn.text) + padding * 2
      local is_hovered = (self.hovered_idx == i)
      
      local bg_color = is_hovered and (style.background3 or style.background) or (style.background2 or style.background)
      renderer.draw_rect(x, oy + 4, w, self.size.y - 8, bg_color)
      
      local text_color = is_hovered and btn.color or style.text
      renderer.draw_text(fnt, btn.text, x + padding, y, text_color)
      
      btn.rect = { x = x, y = oy + 4, w = w, h = self.size.y - 8 }
      x = x + w + padding
    end
  end

  function TurmericToolbarView:on_mouse_moved(mx, my, dx, dy)
    TurmericToolbarView.super.on_mouse_moved(self, mx, my, dx, dy)
    local ox, oy = self:get_content_offset()
    local old_hovered = self.hovered_idx
    self.hovered_idx = nil
    for i, btn in ipairs(self.buttons) do
      if btn.rect and mx >= btn.rect.x and mx <= btn.rect.x + btn.rect.w and
         my >= btn.rect.y and my <= btn.rect.y + btn.rect.h then
        self.hovered_idx = i
        break
      end
    end
    if old_hovered ~= self.hovered_idx then
      core.redraw = true
    end
  end

  function TurmericToolbarView:on_mouse_pressed(button, mx, my, clicks)
    TurmericToolbarView.super.on_mouse_pressed(self, button, mx, my, clicks)
    if button == "left" then
      for i, btn in ipairs(self.buttons) do
        if btn.rect and mx >= btn.rect.x and mx <= btn.rect.x + btn.rect.w and
           my >= btn.rect.y and my <= btn.rect.y + btn.rect.h then
          command.perform(btn.command)
          break
        end
      end
    end
  end

  core.add_thread(function()
    coroutine.yield(0.2)
    local toolbar = TurmericToolbarView()
    -- Safely split active node default (always a DocView/leaf) to avoid root_node split-type restrictions.
    -- Passes 'toolbar' directly to split and locks height on y-axis ({y = true}).
    local node = core.root_view:get_active_node_default()
    if node then
      node:split("up", toolbar, {y = true})
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
