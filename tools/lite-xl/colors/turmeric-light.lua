-- mod-version:3
-- Turmeric light color scheme for Lite XL.
-- Ported from the Monaco theme baked into web/main.js. Keep in sync
-- via tests/lite-xl/check-palette-sync.py.
local style  = require "core.style"
local common = require "core.common"

-- Try Turmeric light palette (web/main.js turmeric-light).
local bg          = "#FFFFFF"
local bg_alt      = "#F8F8F8"   -- editor.lineHighlightBackground
local bg_widget   = "#FFFFFF"
local fg          = "#24292E"   -- editor.foreground, identifier, delimiter
local fg_dim      = "#959DA5"   -- editorLineNumber.foreground
local fg_dim_more = "#E1E4E8"   -- editorIndentGuide.background
local blue        = "#0366D6"   -- cursor, selection accent
local teal_key    = "#20BBB4"   -- keyword (bold) + keyword.operator
local orange_type = "#DC9656"   -- type
local sky         = "#89DDFF"   -- operator
local red         = "#D16969"   -- number
local string_grn  = "#2AA198"
local comment_grn = "#8292A2"

style.background         = { common.color(bg) }
style.background2        = { common.color(bg_alt) }
style.background3        = { common.color(bg_widget) }
style.text               = { common.color(fg_dim) }
style.caret              = { common.color(blue) }
style.accent             = { common.color(blue) }
style.dim                = { common.color(fg_dim) }
style.divider            = { common.color(fg_dim_more) }
style.selection          = { common.color("rgba(3,102,214,0.30)") }
style.line_number        = { common.color(fg_dim) }
style.line_number2       = { common.color(fg) }
style.line_highlight     = { common.color(bg_alt) }
style.scrollbar          = { common.color(fg_dim_more) }
style.scrollbar2         = { common.color(fg_dim) }
style.nagbar             = { common.color(red) }
style.nagbar_text        = { common.color(bg) }
style.nagbar_dim         = { common.color("rgba(0,0,0,0.5)") }
style.guide              = { common.color(fg_dim_more) }
style.guide_highlight    = { common.color(fg_dim) }

style.syntax             = {}
style.syntax["normal"]   = { common.color(fg) }
style.syntax["symbol"]   = { common.color(fg) }
style.syntax["comment"]  = { common.color(comment_grn) }
style.syntax["keyword"]  = { common.color(teal_key) }
style.syntax["keyword2"] = { common.color(orange_type) }
style.syntax["number"]   = { common.color(red) }
style.syntax["literal"]  = { common.color(teal_key) }
style.syntax["string"]   = { common.color(string_grn) }
style.syntax["operator"] = { common.color(sky) }
style.syntax["function"] = { common.color(teal_key) }

style.syntax.paren1      = { common.color(teal_key) }
style.syntax.paren2      = { common.color(orange_type) }
style.syntax.paren3      = { common.color(string_grn) }
style.syntax.paren4      = { common.color(red) }
style.syntax.paren5      = { common.color(blue) }

style.lint               = {}
style.lint.info          = { common.color(blue) }
style.lint.hint          = { common.color(string_grn) }
style.lint.warning       = { common.color(orange_type) }
style.lint.error         = { common.color(red) }
