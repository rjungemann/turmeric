" Vim colorscheme: turmeric-dark
" Extracted from the Monaco "turmeric-dark" theme used in the Try Turmeric
" web REPL (web/main.js). "Dark Spice Market" palette: amber keywords,
" teal types, violet builtins, coral strings, sage numbers.
"
" Requires a true-color terminal. Add to ~/.vimrc:
"   set termguicolors
"   colorscheme turmeric-dark

hi clear
if exists("syntax_on")
  syntax reset
endif
let g:colors_name = "turmeric-dark"
set background=dark

" ---- Palette ----
"   bg          #0C0A08    fg          #EAE0D2
"   amber       #EFA030    gold        #D48B1C
"   teal        #7AC4B8    violet      #C4A0E8
"   coral       #D9735A    sage        #A8C98A
"   comment     #48433D    line-bg     #111009
"   line-nr     #453F39    line-nr-act #88796C
"   op          #6A5F58    delim       #5A5448
"   widget-bg   #181512    widget-bd   #302B24

" ---- Editor chrome ----
hi Normal           guifg=#EAE0D2 guibg=#0C0A08 ctermfg=253 ctermbg=232
hi NormalNC         guifg=#EAE0D2 guibg=#0C0A08
hi Cursor           guifg=#0C0A08 guibg=#D48B1C
hi CursorLine                     guibg=#111009 cterm=NONE
hi CursorLineNr     guifg=#88796C guibg=#111009 gui=bold
hi LineNr           guifg=#453F39 guibg=#0C0A08
hi SignColumn                     guibg=#0C0A08
hi ColorColumn                    guibg=#111009
hi VertSplit        guifg=#302B24 guibg=#0C0A08
hi WinSeparator     guifg=#302B24 guibg=#0C0A08
hi StatusLine       guifg=#EAE0D2 guibg=#181512 gui=NONE
hi StatusLineNC     guifg=#88796C guibg=#111009 gui=NONE
hi TabLine          guifg=#88796C guibg=#111009 gui=NONE
hi TabLineSel       guifg=#EFA030 guibg=#181512 gui=bold
hi TabLineFill                    guibg=#0C0A08
hi Folded           guifg=#88796C guibg=#111009
hi FoldColumn       guifg=#48433D guibg=#0C0A08
hi MatchParen       guifg=#EFA030 guibg=#1F1A12 gui=bold

" ---- Selection / search / diff ----
hi Visual                         guibg=#3A2A14
hi Search           guifg=#0C0A08 guibg=#EFA030
hi IncSearch        guifg=#0C0A08 guibg=#D48B1C gui=bold
hi CurSearch        guifg=#0C0A08 guibg=#D48B1C gui=bold
hi DiffAdd          guifg=#A8C98A guibg=#13180E
hi DiffChange                     guibg=#1A1812
hi DiffDelete       guifg=#D9735A guibg=#1A100D
hi DiffText         guifg=#EFA030 guibg=#1F1A12 gui=bold

" ---- Messages / popup widgets ----
hi Pmenu            guifg=#EAE0D2 guibg=#181512
hi PmenuSel         guifg=#EFA030 guibg=#241D14 gui=bold
hi PmenuSbar                      guibg=#181512
hi PmenuThumb                     guibg=#3E3830
hi WildMenu         guifg=#0C0A08 guibg=#EFA030
hi NormalFloat      guifg=#EAE0D2 guibg=#181512
hi FloatBorder      guifg=#302B24 guibg=#181512
hi ErrorMsg         guifg=#D9735A guibg=NONE gui=bold
hi WarningMsg       guifg=#EFA030 guibg=NONE
hi MoreMsg          guifg=#7AC4B8 guibg=NONE
hi Question         guifg=#7AC4B8 guibg=NONE
hi Title            guifg=#EFA030 guibg=NONE gui=bold
hi Directory        guifg=#7AC4B8 guibg=NONE

" ---- Spelling ----
hi SpellBad         guisp=#D9735A gui=undercurl
hi SpellCap         guisp=#EFA030 gui=undercurl
hi SpellRare        guisp=#C4A0E8 gui=undercurl
hi SpellLocal       guisp=#7AC4B8 gui=undercurl

" ---- Syntax groups (the Monaco token -> Vim group mapping) ----
" Monaco "comment" -> Comment
hi Comment          guifg=#48433D guibg=NONE gui=italic
hi SpecialComment   guifg=#88796C guibg=NONE gui=italic
hi Todo             guifg=#0C0A08 guibg=#EFA030 gui=bold

" Monaco "string" -> String
hi String           guifg=#D9735A guibg=NONE
hi Character        guifg=#D9735A guibg=NONE
hi SpecialChar      guifg=#EFA030 guibg=NONE

" Monaco "number"/"number.float" -> Number/Float
hi Number           guifg=#A8C98A guibg=NONE
hi Float            guifg=#A8C98A guibg=NONE
hi Boolean          guifg=#A8C98A guibg=NONE

" Monaco "keyword" -> Statement/Keyword/Conditional (amber, bold)
hi Statement        guifg=#EFA030 guibg=NONE gui=bold
hi Conditional      guifg=#EFA030 guibg=NONE gui=bold
hi Repeat           guifg=#EFA030 guibg=NONE gui=bold
hi Label            guifg=#EFA030 guibg=NONE gui=bold
hi Keyword          guifg=#EFA030 guibg=NONE gui=bold
hi Exception        guifg=#EFA030 guibg=NONE gui=bold
hi Define           guifg=#EFA030 guibg=NONE gui=bold

" Monaco "keyword.operator" -> builtins / Special (violet)
hi Function         guifg=#C4A0E8 guibg=NONE
hi Special          guifg=#C4A0E8 guibg=NONE
hi Macro            guifg=#C4A0E8 guibg=NONE
hi PreProc          guifg=#C4A0E8 guibg=NONE
hi PreCondit        guifg=#C4A0E8 guibg=NONE
hi Include          guifg=#C4A0E8 guibg=NONE

" Monaco "type" -> Type (teal)
hi Type             guifg=#7AC4B8 guibg=NONE
hi StorageClass     guifg=#7AC4B8 guibg=NONE
hi Structure        guifg=#7AC4B8 guibg=NONE
hi Typedef          guifg=#7AC4B8 guibg=NONE

" Monaco "operator" / "delimiter" (muted browns)
hi Operator         guifg=#6A5F58 guibg=NONE
hi Delimiter        guifg=#5A5448 guibg=NONE

" Monaco "identifier" -> base fg
hi Identifier       guifg=#EAE0D2 guibg=NONE
hi Constant         guifg=#C4A0E8 guibg=NONE

hi Error            guifg=#D9735A guibg=NONE gui=bold
hi NonText          guifg=#302B24 guibg=NONE
hi EndOfBuffer      guifg=#0C0A08 guibg=#0C0A08
hi Whitespace       guifg=#252119 guibg=NONE
hi SpecialKey       guifg=#48433D guibg=NONE
hi IndentBlanklineChar guifg=#252119 guibg=NONE

" ---- Direct mappings for the turmeric.vim syntax groups ----
" (Mirror the Monaco token roles exactly.)
hi link turmericLineComment   Comment
hi link turmericDocComment    SpecialComment
hi link turmericBlockComment  Comment
hi link turmericString        String
hi link turmericStringEsc     SpecialChar
hi link turmericBadEsc        Error
hi link turmericCBlock        PreProc
hi link turmericHex           Number
hi link turmericBin           Number
hi link turmericFloat         Float
hi link turmericInt           Number
hi link turmericBool          Boolean
hi link turmericChar          Character
hi link turmericNil           Constant
hi link turmericKeyLit        Constant
hi link turmericMeta          PreProc
hi link turmericDefine        Define
hi link turmericControl       Conditional
hi link turmericType          Type
hi link turmericEffect        Keyword
hi link turmericExcept        Exception
hi link turmericNeotericCall  Function
hi link turmericDelim         Delimiter
hi link turmericCurlyInfix    Special
hi link turmericLangDir       PreProc
hi link turmericLangName      Type
