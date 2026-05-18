" Vim syntax file
" Language:     Turmeric
" Maintainer:   Turmeric Project
" File Types:   *.tur

if exists("b:current_syntax")
  finish
endif

" Turmeric identifiers can contain hyphens, !, ?, and *
" ASCII codes: 45=- 33=! 63=? 42=*
setlocal iskeyword=@,48-57,_,45,33,63,42

" ---- Comments ----
syn keyword turmericTodo       contained TODO FIXME HACK NOTE XXX
syn match   turmericLineComment /;.*$/    contains=turmericTodo
syn match   turmericDocComment  /;;;.*$/  contains=turmericTodo
syn region  turmericBlockComment start=/#|/ end=/|#/
      \ contains=turmericBlockComment,turmericTodo fold

" ---- Strings ----
syn region  turmericString   start=/"/ skip=/\\"/ end=/"/ keepend
      \ contains=turmericStringEsc,turmericBadEsc
syn match   turmericStringEsc  /\\["\\/abfnrtv0]/          contained
syn match   turmericStringEsc  /\\u[0-9a-fA-F]\{4}/        contained
syn match   turmericStringEsc  /\\U[0-9a-fA-F]\{8}/        contained
syn match   turmericStringEsc  /\\x[0-9a-fA-F]\{2}/        contained
syn match   turmericBadEsc     /\\[^"\\/abfnrtv0uUx]/       contained

" ---- C inline blocks ----
syn region  turmericCBlock start=/```c\?/ end=/```/ keepend

" ---- Numbers ----
syn match   turmericHex   /\<0[xX][0-9a-fA-F]\+\>/
syn match   turmericBin   /\<0[bB][01]\+\>/
syn match   turmericFloat /\<-\?\(\d\+\.\d*\|\.\d\+\)\([eE][+-]\?\d\+\)\?\>/
syn match   turmericInt   /\<-\?\d\+\>/

" ---- Booleans ----
syn match   turmericBool  /\<#[tf]\>/

" ---- Character literals ----
syn match   turmericChar  /#\\\(nul\|null\|backspace\|tab\|newline\|linefeed\|vtab\|page\|return\|space\|rubout\|altmode\|escape\)\>/
syn match   turmericChar  /#\\./

" ---- Nil and unit ----
syn keyword turmericNil   nil null none unit

" ---- Keyword literals (:foo, :else, :pre, :post) ----
syn match   turmericKeyLit /:[a-zA-Z_][a-zA-Z0-9_+\-*\/<>=!?$]*/

" ---- Metadata annotations (^linear, ^mut, ^unique, ^affine) ----
syn match   turmericMeta  /\^[a-zA-Z][a-zA-Z0-9_\-]*/

" ---- Reader macros ----
syn match   turmericSplice  /\~@/
syn match   turmericUnquote /\~/
syn match   turmericQuote   /[`']/

" ---- Special operators ----
syn match   turmericOp  /::/
syn match   turmericOp  /|>/

" ---- Definition keywords ----
syn keyword turmericDefine  def defn defmacro defmodule defdata defgadt
syn keyword turmericDefine  defclass definstance defstruct deftuple
syn keyword turmericDefine  defpackage use import export

" ---- Control flow ----
syn keyword turmericControl  if cond case match loop while for
syn keyword turmericControl  break continue return and or not

" ---- Type system keywords ----
syn keyword turmericType  type typeclass impl where forall generic trait any

" ---- Effect system keywords ----
syn keyword turmericEffect  effect handle do perform with

" ---- Exception keywords ----
syn keyword turmericExcept  try catch throw finally raise

" ---- Core special forms ----
syn keyword turmericSpecial  let let* lambda fn quote unquote quasiquote begin

" ---- Built-in functions ----
syn keyword turmericBuiltin  vec push pop len nth set-nth! append first rest
syn keyword turmericBuiltin  cons reverse list apply map filter reduce fold
syn keyword turmericBuiltin  print println read str concat split coerce cast
syn keyword turmericBuiltin  type-of is-a?
syn keyword turmericBuiltin  string? vector? list? number? symbol?
syn keyword turmericBuiltin  boolean? null? atom? pair? empty? extern-c

" ---- Delimiters ----
syn match   turmericDelim  /[()[\]{}]/

" ---- Highlight links ----
" Later definitions take priority at the same position (Vim priority rule),
" so turmericDocComment overrides turmericLineComment for ;;; lines.
hi def link turmericLineComment   Comment
hi def link turmericDocComment    SpecialComment
hi def link turmericBlockComment  Comment
hi def link turmericTodo          Todo
hi def link turmericString        String
hi def link turmericStringEsc     SpecialChar
hi def link turmericBadEsc        Error
hi def link turmericCBlock        PreProc
hi def link turmericHex           Number
hi def link turmericBin           Number
hi def link turmericFloat         Float
hi def link turmericInt           Number
hi def link turmericBool          Boolean
hi def link turmericChar          Character
hi def link turmericNil           Constant
hi def link turmericKeyLit        Constant
hi def link turmericMeta          PreProc
hi def link turmericSplice        Special
hi def link turmericUnquote       Special
hi def link turmericQuote         Special
hi def link turmericOp            Operator
hi def link turmericDefine        Define
hi def link turmericControl       Conditional
hi def link turmericType          Type
hi def link turmericEffect        Keyword
hi def link turmericExcept        Exception
hi def link turmericSpecial       Special
hi def link turmericBuiltin       Function
hi def link turmericDelim         Delimiter

let b:current_syntax = "turmeric"
