# Turmeric syntax for Vim

Syntax highlighting, filetype detection and indent settings for Turmeric
(`.tur`, `.tur.sweet`, `build.tur`) including the `#lang saffron` dialect.

## Install

Copy the tree into your runtime path, or point at it directly:

```vim
set runtimepath+=/path/to/turmeric/editors/vim-turmeric
```

With a plugin manager, point it at this directory. The layout is the standard
`syntax/` + `ftdetect/` + `ftplugin/` one, so nothing else is needed.

## What it covers

- `#lang <base>[/<dialect>] <layer>*` on the first line, with the base
  highlighted distinctly -- `#lang saffron` reads as a language choice.
- `;;;` docstrings distinct from `;;` comments (the marker CLAUDE.md defines).
- Definition forms (`defn`, `defstruct`, `defgadt`, `definstance`, ...) and the
  name each binds.
- Both annotation spellings: fused `x :int` and spaced `x : int`.
- Parameter attributes (`^mut`, `^linear`, `^borrow`, ...), effect rows
  (`#{...}`), reader dispatch (`#map{`, `#set{`, `#refine{`).
- Inline C (` ```c ... ``` `) marked as embedded rather than parsed as Turmeric.

`lispwords` and `iskeyword` are set so `w`, `*` and `=` treat `vec-push!` and
`is?` as single words and indent the special forms sensibly.

## Testing

`tests/run-editor-syntax.sh` loads this pack in a real Vim and asserts the
highlight group at specific positions (`synIDattr(synIDtrans(synID(...)))`).
That is how the fused-vs-spaced annotation bug was found: Vim gives a
later-defined item priority, and with the rules the other way round `y :float`
highlighted as a keyword literal while `y : float` highlighted as a type.
