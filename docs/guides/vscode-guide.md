---
title: VS Code Extension
category: Editor and IDE
description: VS Code extension installation and configuration
---

# VS Code Extension for Turmeric

The Turmeric VS Code extension provides syntax highlighting for `.tur` files,
including support for both standard S-expression syntax and sweet-exp
(indentation-based) syntax.

The extension lives in `vscode-syntax-ext/` at the repo root and ships a
pre-built `.vsix` package.

## Installation

### From the pre-built package

```sh
cd vscode-syntax-ext
code --install-extension turmeric-syntax-0.1.0.vsix
```

### From source (development)

```sh
cd vscode-syntax-ext

# Option A: copy to extensions directory
mkdir -p ~/.vscode/extensions/turmeric-syntax-0.1.0
cp -r * ~/.vscode/extensions/turmeric-syntax-0.1.0/

# Option B: symlink for live editing
ln -s "$(pwd)" ~/.vscode/extensions/turmeric-syntax-dev
```

Reload VS Code after either option.

## Features

Once installed, any `.tur` file is automatically highlighted:

- **Keywords** -- `defn`, `let`, `if`, `match`, `handle`, `defclass`,
  `definstance`, `defeffect`, and all other Turmeric special forms
- **Built-in functions** -- `cons`, `car`, `cdr`, `map`, `filter`, etc.
- **Literals** -- integers, floats, hex (`0xFF`), binary (`0b1010`),
  scientific notation, strings, booleans (`#t`, `#f`), characters (`#\a`)
- **Comments** -- line comments (`;`) and block comments (`#| ... |#`)
- **Operators** -- arithmetic, comparison, logical, threading (`->`, `->>`, `|>`)
- **Meta-programming** -- quote (`'`), quasiquote (`` ` ``), unquote (`,`)

### Bracket and indentation support

- Bracket pair colorization and auto-close for `()`, `[]`, `{}`
- Comment toggle with `Cmd+/` (`;` for line comments)
- Auto-indentation increases after opening brackets and control forms

## Recommended VS Code settings

```json
{
  "[turmeric]": {
    "editor.tabSize": 2,
    "editor.insertSpaces": true,
    "editor.formatOnSave": false
  }
}
```

## Scope names (for theme customization)

| Scope | Matches |
|---|---|
| `keyword.control.turmeric` | `if`, `match`, `loop`, `while`, etc. |
| `keyword.definition.turmeric` | `defn`, `defmacro`, `defstruct`, etc. |
| `keyword.type.turmeric` | `:int`, `:cstr`, type annotations |
| `keyword.effect.turmeric` | `defeffect`, `handle`, `perform` |
| `support.function.builtin.turmeric` | Built-in functions |
| `entity.name.function.turmeric` | User-defined function names |
| `constant.numeric.integer.turmeric` | Integer literals |
| `constant.numeric.float.turmeric` | Float literals |
| `constant.language.boolean.true.turmeric` | `#t` |
| `constant.language.boolean.false.turmeric` | `#f` |
| `constant.character.turmeric` | Character literals |
| `string.quoted.double.turmeric` | String literals |
| `comment.line.semicolon.turmeric` | Line comments |
| `comment.block.turmeric` | Block comments |

## Known limitations

- Sweet-exp detection is heuristic (indentation patterns); a full parser
  would be needed for perfect accuracy.
- Macro expansions are not highlighted differently from regular calls.
- No LSP (IntelliSense, go-to-definition, hover docs) yet -- planned for Phase 2.

## Rebuilding the `.vsix`

```sh
cd vscode-syntax-ext
npm install -g @vscode/vsce
vsce package
# produces turmeric-syntax-X.Y.Z.vsix
```

## See also

- `vscode-syntax-ext/` -- Extension source
- `vscode-syntax-ext/syntaxes/turmeric.tmLanguage.json` -- TextMate grammar
- `vscode-syntax-ext/language-configuration.json` -- Bracket / comment config
- [formatter-guide.md](formatter-guide.md) -- `tur format` CLI (format-on-save planned)
