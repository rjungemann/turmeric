-- mod-version:3
-- Turmeric dark ("Spice Market") color scheme for Trowel.
-- Ported from the Monaco theme baked into web/main.js. Keep in sync
-- via tests/trowel/check-palette-sync.py.
local style  = require "core.style"
local common = require "core.common"

-- Try Turmeric Spice Market palette (web/main.js turmeric-dark).
local bg          = "#0C0A08"
local bg_alt      = "#111009"   -- editor.lineHighlightBackground
local bg_widget   = "#181512"
local bg_gutter   = "#0C0A08"
local fg          = "#EAE0D2"   -- editor.foreground, identifier
local fg_dim      = "#88796C"   -- editorLineNumber.activeForeground
local fg_dim_more = "#453F39"   -- editorLineNumber.foreground
local gold        = "#D48B1C"   -- editorCursor.foreground, accent
local amber       = "#EFA030"   -- keyword (bold)
local teal        = "#7AC4B8"   -- type
local violet      = "#C4A0E8"   -- keyword.operator (also special forms)
local coral       = "#D9735A"   -- string
local sage        = "#A8C98A"   -- number
local comment     = "#48433D"   -- italic
local op_dim      = "#6A5F58"   -- operator
local delim_dim   = "#5A5448"   -- delimiter
local border      = "#302B24"   -- widget border
local indent      = "#252119"   -- indent guide
local indent_act  = "#3E3830"

style.background         = { common.color(bg) }
style.background2        = { common.color(bg_alt) }    -- bottom view bg
style.background3        = { common.color(bg_widget) } -- popup bg
style.text               = { common.color(fg_dim) }
style.caret              = { common.color(gold) }
style.accent             = { common.color(gold) }
style.dim                = { common.color(fg_dim) }
style.divider            = { common.color(border) }
style.selection          = { common.color("rgba(212,139,28,0.18)") }
style.line_number        = { common.color(fg_dim_more) }
style.line_number2       = { common.color(fg_dim) }
style.line_highlight     = { common.color(bg_alt) }
style.scrollbar          = { common.color(indent_act) }
style.scrollbar2         = { common.color(fg_dim) }
style.nagbar             = { common.color(coral) }
style.nagbar_text        = { common.color(fg) }
style.nagbar_dim         = { common.color("rgba(0,0,0,0.5)") }
style.guide              = { common.color(indent) }
style.guide_highlight    = { common.color(indent_act) }

style.syntax             = {}
style.syntax["normal"]   = { common.color(fg) }
style.syntax["symbol"]   = { common.color(fg) }
style.syntax["comment"]  = { common.color(comment) }
style.syntax["keyword"]  = { common.color(amber) }   -- defn / defmacro / let / if ...
style.syntax["keyword2"] = { common.color(teal) }    -- types: int / float / cstr ...
style.syntax["number"]   = { common.color(sage) }
style.syntax["literal"]  = { common.color(violet) }  -- :keywords, true, false
style.syntax["string"]   = { common.color(coral) }
style.syntax["operator"] = { common.color(op_dim) }
style.syntax["function"] = { common.color(violet) }

-- Rainbow parens, dim end of the palette for Lisp readability.
style.syntax.paren1      = { common.color(amber) }
style.syntax.paren2      = { common.color(teal) }
style.syntax.paren3      = { common.color(violet) }
style.syntax.paren4      = { common.color(sage) }
style.syntax.paren5      = { common.color(gold) }

style.lint               = {}
style.lint.info          = { common.color(teal) }
style.lint.hint          = { common.color(sage) }
style.lint.warning       = { common.color(gold) }
style.lint.error         = { common.color(coral) }
