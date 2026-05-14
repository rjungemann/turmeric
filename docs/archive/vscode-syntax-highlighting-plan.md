# VSCode Syntax Highlighting for Turmeric (.tur files)

## Overview
Create a TextMate grammar-based syntax definition for VSCode that provides syntax highlighting for `.tur` files, supporting both standard Turmeric syntax and sweet-exp (s-expression) syntax.

## Deliverables

### 1. Main Syntax Definition File
**Path:** `src/syntax/turmeric.tmLanguage.json`

A TextMate-compatible JSON grammar file that defines:
- Language ID: `turmeric`
- File associations: `*.tur`
- Scope name: `source.turmeric`

### 2. VSCode Extension Manifest
**Path:** `vscode-syntax-ext/package.json`

Configuration for packaging as a VSCode extension with:
- Grammar contribution point
- Language definition
- Icon/theme support (optional)

### 3. Theme Support
**Path:** `vscode-syntax-ext/themes/turmeric-light.json` (optional)
**Path:** `vscode-syntax-ext/themes/turmeric-dark.json` (optional)

Theme files defining color schemes optimized for Turmeric code.

## Grammar Scope Hierarchy

```
source.turmeric
├── comment
│   ├── line        (;-style comments)
│   └── block       (#| ... |# block comments)
├── string
│   └── quoted      ("..." strings)
├── number
│   ├── integer
│   ├── float
│   └── hex/binary
├── keyword
│   ├── control     (if, match, loop, etc.)
│   ├── definition  (def, defn, defmacro, etc.)
│   ├── type        (type, typeclass, impl, etc.)
│   ├── effect      (effect, handle, etc.)
│   └── built-in    (common library functions)
├── constant
│   ├── boolean     (#t, #f)
│   └── special     (null, unit, etc.)
├── function
│   ├── name        (function call identifiers)
│   └── built-in    (built-in function names)
├── variable
│   ├── name        (bound variables)
│   └── parameter   (function parameters)
├── operator
│   ├── arithmetic  (+, -, *, /, etc.)
│   ├── comparison  (=, <, >, etc.)
│   ├── logical     (and, or, not, etc.)
│   └── special     (->>, =>, etc.)
├── punctuation
│   ├── bracket     (parentheses, brackets, braces)
│   ├── quote       (', `, unquote symbols)
│   └── separator   (commas, dots)
├── meta
│   ├── sweet-exp   (indentation-based syntax markers)
│   └── s-expr      (parenthetical expressions)
└── invalid
    └── illegal     (syntax errors)
```

## Turmeric Language Features to Highlight

### Keywords
- **Control Flow:** `if`, `match`, `loop`, `while`, `for`, `break`, `continue`, `return`
- **Definitions:** `def`, `defn`, `defmacro`, `defmodule`, `use`
- **Type System:** `type`, `typeclass`, `impl`, `where`, `forall`, `generic`
- **Effects:** `effect`, `handle`, `do`, `perform`, `with`
- **Exception Handling:** `try`, `catch`, `throw`, `finally`
- **Other:** `let`, `cond`, `case`, `lambda`, `fn`, `quote`, `unquote`

### Built-in Functions
Core functions to highlight:
- Vector operations: `vec`, `push`, `pop`, `len`, `nth`
- Type operations: `type-of`, `is-a?`
- Control: `apply`, `map`, `filter`, `reduce`
- I/O: `print`, `println`, `read`
- String operations: `str`, `concat`, `split`
- Collection operations: `list`, `first`, `rest`, `cons`

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`, `**`
- Comparison: `=`, `!=`, `<`, `>`, `<=`, `>=`
- Logical: `and`, `or`, `not`
- Special: `->`, `=>`, `-->` (threading/piping)
- Quote/Meta: `'`, `` ` ``, `,`, `,@` (quote, quasiquote, unquote, unquote-splicing)

### Literals
- **Numbers:** integers, floats, hex (`0x`), binary (`0b`), scientific notation
- **Strings:** double-quoted with escape sequences
- **Symbols:** identifiers starting with letters/special chars
- **Booleans:** `#t`, `#f`
- **Characters:** `#\a`, `#\newline`

## Sweet-Exp Syntax Support

### Indentation-Based Syntax
Detect and highlight:
- Colon syntax: `key: value` (maps to `key value`)
- Curly braces: `{...}` (for groups/blocks)
- Indentation-based grouping (child expressions on next line with increased indent)
- Indentation markers: track indent level for syntax validation

### Sweet-Exp Operators
- Colon (`:`) as argument separator
- Curly braces (`{`, `}`) for grouping
- Indentation levels (track scope)

## Implementation Strategy

### Phase 1: Basic Infrastructure
1. Create extension directory structure
2. Set up `package.json` with minimal configuration
3. Create base `turmeric.tmLanguage.json` with:
   - Language ID and file patterns
   - Basic scopes
   - Comment patterns (line and block)

### Phase 2: Core Syntax Rules
1. Add number patterns (integer, float, hex, binary)
2. Add string patterns with escape sequences
3. Add keyword definitions (control, definition, type, effect)
4. Add boolean and nil constants
5. Add operator patterns

### Phase 3: Advanced Features
1. Implement smart bracket matching
2. Add sweet-exp indentation detection
3. Add function/variable name detection
4. Implement s-expression context awareness

### Phase 4: Refinement & Testing
1. Create test files (`test.tur`) with comprehensive code samples
2. Validate highlighting in VSCode
3. Optimize regex patterns for performance
4. Document color scope names for theme creators

### Phase 5: Distribution
1. Publish to VSCode Marketplace
2. Create README with installation instructions
3. Add theme recommendations
4. Set up repository for community contributions

## Testing Strategy

### Unit Testing (Regex Patterns)
- Test each pattern against expected/unexpected input
- Verify no false positives/negatives
- Performance testing on large files

### Integration Testing
- Create `test-syntax.tur` with all language features
- Validate rendering in VSCode
- Test with various themes (Light, Dark, High Contrast)

### Regression Testing
- Test against existing `.tur` files in `stdlib/`
- Ensure highlighting consistency across codebase

## Files to Create

```
vscode-syntax-ext/
├── package.json
├── package-lock.json
├── README.md
├── LICENSE
├── syntaxes/
│   └── turmeric.tmLanguage.json
├── themes/
│   ├── turmeric-light.json (optional)
│   └── turmeric-dark.json (optional)
├── language-configuration.json
└── test/
    └── test-syntax.tur
```

## Language Configuration File
**Path:** `vscode-syntax-ext/language-configuration.json`

Configure:
- Bracket pairs: `()`, `[]`, `{}`
- Auto-closing pairs
- Auto-indentation rules
- Comment tokens for VSCode's comment toggling
- Code folding regions
- Word patterns (for word boundary detection)

## References & Resources

- [TextMate Language Grammars](https://macromates.com/manual/en/language_grammars)
- [VSCode Language Extensions Guide](https://code.visualstudio.com/api/language-extensions/language-configuration-guide)
- [Turmeric Language Spec](../turmeric-plan.md)
- [Sweet-exp Spec](https://www.gnu.org/software/guile/manual/html_node/Sweet.html)

## Dependencies
- VSCode 1.50+ (for modern TextMate grammar features)
- Node.js (for development/packaging)

## Success Criteria

✓ All Turmeric keywords highlighted correctly  
✓ Numbers, strings, and operators recognized  
✓ Comments (line and block) properly scoped  
✓ Sweet-exp indentation detected  
✓ Function/variable names distinguished  
✓ No performance degradation on large files  
✓ Works with default VSCode themes  
✓ Compatible with popular community themes  

## Future Enhancements

- Language Server Protocol (LSP) integration for:
  - IntelliSense / code completion
  - Go to definition
  - Find references
  - Hover documentation
  - Real-time type checking
- Snippet support (common patterns)
- Bracket pair colorizer integration
- Code formatter integration
- Linter integration
