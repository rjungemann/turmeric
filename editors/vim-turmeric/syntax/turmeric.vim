" Vim syntax file for Turmeric, including the Saffron dialect.
"
" Kept deliberately close to the VS Code TextMate grammar in
" ../vscode-turmeric/syntaxes/turmeric.tmLanguage.json -- the two are meant to
" agree, so a construct highlighted in one should be highlighted in the other.

if exists("b:current_syntax") | finish | endif

syn case match

" `#lang <base>[/<dialect>] <layer>*`, first line only.  The base is its own
" group so `#lang saffron` reads as a language choice, not a comment.
syn match turmericLangLayer  "\v\s+[a-z][a-z0-9-]*" contained
syn match turmericLangBase   "\v[a-z][a-z0-9-]*(/[a-z][a-z0-9-]*)?" contained nextgroup=turmericLangLayer skipwhite
syn match turmericLang       "\v^#lang>" nextgroup=turmericLangBase skipwhite

" `;;;` is the docstring marker (CLAUDE.md); `;;` and `;` are ordinary.
syn match turmericDocComment "\v;;;.*$"  contains=turmericTodo
syn match turmericComment    "\v;{1,2}([^;].*)?$" contains=turmericTodo
syn keyword turmericTodo TODO FIXME XXX NOTE contained

syn region turmericString start=+"+ skip=+\\.+ end=+"+ contains=turmericEscape
syn match  turmericEscape "\v\\." contained

" Inline C: the ```c ... ``` fence is C, not Turmeric.
syn region turmericInlineC start="```c\>" end="```" keepend contains=@NoSpell

syn match turmericDefine "\v\(\s*(defprotocol|definstance|defdynamic|defrecord|defstruct|defopaque|defmodule|defmacro|defalias|defclass|defeffect|defimage|defworld|defgadt|defdata|deftype|defkind|defrec|define|defn|def)>" nextgroup=turmericDefName skipwhite
syn match turmericDefName "\v[^ \t()\[\]{};]+" contained

" The terminator is an explicit not-an-identifier class, not `\>`: a form
" ending in `?` or `!` has no word boundary after it.
syn match turmericSpecial "\v\(@<=(with-handler|compose-handlers|cloneable-reset|cloneable-shift|serial-reset|serial-shift|catch-unwind|discontinue|with-region|use-reader-macros|panic-with|bt-scope|call/cc|handle|resume|perform|reset|shift0|shift|escape|import|export|match|letrec|loop|while|when|unless|cond|case|let|fn|if|do|for|set!|and|or|not|quote|quasiquote|unquote|lambda|recur|return|defer|await|async|spawn|yield|try|catch|throw|is\?|cast|type-of)([A-Za-z0-9_/!?<>\=*+-])@!"

syn match turmericDispatch "\v#(map|set|refine|fx|writes|reads|s|lang-layers)>"
syn match turmericEffect   "\v#\{[^}]*\}"
syn match turmericAttr     "\v\^[a-z][a-z0-9-]*"

" A bare `:name` in value position (a keyword literal, e.g. `#map{:a 1}`).
" Defined BEFORE the type rule on purpose: Vim gives a later-defined item
" priority when two match at the same position, and both match at the `:` of a
" fused annotation.  With the order reversed the fused `y :float` highlighted
" as a keyword literal while the spaced `y : float` highlighted as a type --
" the same annotation rendering two ways depending on spelling.  Measured with
" `synIDattr(synIDtrans(synID(...)))`, not assumed.
syn match turmericKeyword  "\v:[A-Za-z_][A-Za-z0-9_-]*"
" Both annotation spellings: fused `x :int` and spaced `x : int`.  The leading
" whitespace-or-`[` is what separates an annotation from a keyword literal.
syn match turmericType     "\v(\s|\[)@<=:\s*[A-Za-z_][A-Za-z0-9_/<>-]*"

syn keyword turmericConstant true false nil nil-value
" Floats before ints so 7.1 is one token.
syn match turmericNumber "\v(<|-)@<=-?(0[xX][0-9a-fA-F]+|[0-9]+\.[0-9]+([eE][-+]?[0-9]+)?|[0-9]+)>"

hi def link turmericLang       PreProc
hi def link turmericLangBase   Type
hi def link turmericLangLayer  Identifier
hi def link turmericDocComment SpecialComment
hi def link turmericComment    Comment
hi def link turmericTodo       Todo
hi def link turmericString     String
hi def link turmericEscape     SpecialChar
hi def link turmericInlineC    Special
hi def link turmericDefine     Statement
hi def link turmericDefName    Function
hi def link turmericSpecial    Keyword
hi def link turmericDispatch   PreProc
hi def link turmericEffect     Identifier
hi def link turmericAttr       StorageClass
hi def link turmericType       Type
hi def link turmericKeyword    Constant
hi def link turmericConstant   Boolean
hi def link turmericNumber     Number

let b:current_syntax = "turmeric"
