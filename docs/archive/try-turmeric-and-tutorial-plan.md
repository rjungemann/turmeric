# Try Turmeric & Interactive Tutorial Plan

## Overview

This document outlines the plan for creating:
1. **Try Turmeric** - A web-based REPL allowing users to experiment with Turmeric code directly in the browser
2. **Interactive Tutorial** - An optional guided tutorial accessible from within the REPL

## Goals

- Lower the barrier to entry for new users
- Provide immediate, hands-on experience with Turmeric syntax
- Offer progressive learning through interactive examples
- Maintain consistency with the existing CLI REPL experience

---

## Part 1: Try Turmeric Site

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Browser (Client)                         │
│  ┌─────────────────┐    ┌─────────────────┐                │
│  │   Code Editor   │    │   Output Console │                │
│  │                 │    │                 │                │
│  │  Monaco/VSCode  │───▶│  ANSI-colored   │                │
│  │   web editor    │    │   terminal      │                │
│  └─────────────────┘    └─────────────────┘                │
│                       │                                    │
│          ┌────────────────────────────────────────────┐     │
│          │          WebAssembly Module               │     │
│          │  ┌─────────────┐  ┌─────────────────────┐   │     │
│          │  │  Emscripten  │  │   libturi (compiled) │   │     │
│          │  │   runtime    │  │   to WebAssembly     │   │     │
│          │  └─────────────┘  └─────────────────────┘   │     │
│          └────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────┘
```

### Implementation Options

| Approach | Pros | Cons | Recommendation |
|----------|------|------|----------------|
| **WASM (Emscripten)** | Native performance, full language support, reuse existing compiler | Larger initial load (~2-5MB), build complexity | ✅ Preferred |
| **Server-side execution** | Thin client, no WASM build needed | Latency, server costs, security sandboxing required | Backup option |
| **Docker/Podman in browser** | Full environment | Heavy, not widely supported | ❌ Not viable |

### Component Breakdown

#### 1.1 WebAssembly Compilation

**Dependencies:**
- Emscripten SDK (`emcc`)
- Existing Turmeric compiler (libturi)

**Build Process:**
```bash
# Cross-compile libturi to WebAssembly
emcc -Iinclude -Isrc src/libturi.c ... -o turmeric-wasm.js \
  -sEXPORTED_FUNCTIONS="['_malloc','_free','_turi_eval','_turi_init']" \
  -sEXPORTED_RUNTIME_METHODS="['ccall','cwrap']" \
  -sMODULARIZE=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sWASM=1 \
  -O3
```

**Required C API additions:**
- `turi_init()` - Initialize the Turmeric runtime
- `turi_eval(const char *input, char **output, char **error)` - Evaluate a string, return result
- `turi_reset()` - Reset state for fresh session

**File:** `src/wasm_glue.c` (new)

#### 1.2 Frontend Components

**Page Structure (`/index.html`):**
```
┌─────────────────────────────────────────────────────────┐
│  Turmeric Logo    [Try REPL] [Tutorial] [Docs] [GitHub]   │
├─────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────────────────────────────────────────┐  │
│  │  Editor Tab: main.tur                                  │  │
│  │  ┌─────────────────────────┐  ┌─────────────────┐  │  │
│  │  │ // Enter Turmeric code   │  │ > (+ 1 2)       │  │  │
│  │  │ (defn square [x]         │  │ 3                │  │  │
│  │  │   (* x x))               │  │ > (square 5)    │  │  │
│  │  │                         │  │ 25               │  │  │
│  │  │                         │  │                 │  │  │
│  │  └─────────────────────────┘  └─────────────────┘  │  │
│  │                                                 │  │
│  │  [Run] [Clear] [Load Example ▼] [Share]           │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                                 │
│  Examples: Hello World | FizzBuzz | Closures | Effects     │
└─────────────────────────────────────────────────────────┘
```

**Technologies:**
- **Editor:** Monaco Editor (VS Code's web editor) or CodeMirror
- **Styling:** Tailwind CSS or vanilla CSS with custom theme
- **Build:** Vite or esbuild for bundling
- **WASM loader:** Emscripten's generated loader

**Files:**
- `web/index.html` - Main page structure
- `web/main.js` - WASM initialization, editor setup, REPL logic
- `web/styles.css` - Styling
- `web/examples.js` - Preloaded example code snippets

#### 1.3 Editor Features

- Syntax highlighting for Turmeric (reuse VS Code extension)
- Line numbers
- Auto-indentation
- Basic autocomplete (keywords, builtins)
- Matching parentheses highlighting
- Multiple tabs for different files

#### 1.4 Console Features

- ANSI color support for output
- Separate stdout/stderr streams
- Scrollback buffer (1000+ lines)
- Click to copy output
- Clear console button

#### 1.5 Example Code Snippets

Preloaded examples covering key features:

```lisp
// Hello World
(println "Hello, Turmeric!")

// Basic Math
(defn factorial [n :int] :int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))

(println (factorial 10))

// Closures
(defn make-adder [x :int] (fn [y :int] (+ x y)))
(let [add5 (make-adder 5)]
  (println (add5 10)))

// Algebraic Effects
defeffect Ask [] :int

defn use-ask [] :int
  (+ 1 (perform (Ask)))

(println (handle (use-ask)
  (Ask [] k) (resume k 41)))

// Reference Counting
defn main [] :int
  (let [r (rc/of 42)]
    (println (rc/strong-count r))
    (let [r2 (rc/clone r)]
      (println (rc/strong-count r)))
    (rc/drop r)
    0)
```

#### 1.6 Sharing & URL State

- Encode code in URL hash: `#code=<base64-encoded-gzip>`
- Share button generates short URL
- Load from URL on page init

**Implementation:**
```javascript
function encodeState(code) {
  const compressed = pako.gzip(code);
  return btoa(String.fromCharCode(...compressed)).replace(/\+/g, '-').replace(/\//g, '_');
}

function decodeState(hash) {
  const base64 = hash.replace(/-/g, '+').replace(/_/g, '/');
  const binary = atob(base64);
  const compressed = new Uint8Array(binary.split('').map(c => c.charCodeAt(0)));
  return pako.ungzip(compressed, { to: 'string' });
}
```

#### 1.7 Performance Considerations

- Lazy load WASM module (show loading spinner)
- Streaming compilation for large inputs
- Web Worker for WASM execution to avoid blocking UI
- Debounce rapid input for syntax checking

#### 1.8 Security Considerations

- WASM runs in sandboxed environment
- No filesystem access from browser
- Timeout execution after 5 seconds (configurable)
- Memory limits via WASM runtime

### 1.9 Build & Deployment

**Build Script (`Justfile` additions):**
```just
# Build WASM module
wasm:
  mkdir -p build-wasm
  emcc -Iinclude -Isrc src/libturi.c src/wasm_glue.c ... \
    -o build-wasm/turmeric.js \
    -sMODULARIZE=1 \
    -sALLOW_MEMORY_GROWTH=1 \
    -O3

# Build web bundle
web:
  cd web && npm install && npm run build

# Deploy to GitHub Pages
deploy-web:
  just web
  cd web/dist && git init && git add . && git commit -m "Deploy"
  git push -f git@github.com:turmeric-lang/turmeric.git main:gh-pages
```

**Directory Structure:**
```
web/
├── index.html
├── main.js
├── styles.css
├── examples.js
├── package.json
├── vite.config.js
├── public/
│   └── turmeric.js      # Compiled WASM module
└── src/
    └── editor-setup.js
```

### 1.10 Timeline (Estimated)

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| WASM compilation | 1-2 days | libturi compiles and runs in Emscripten |
| C API wrapper | 1 day | turi_eval, turi_init functions |
| Frontend setup | 1-2 days | Basic page with Monaco editor |
| REPL integration | 2-3 days | WASM ↔ JS communication working |
| Polish & examples | 1-2 days | Syntax highlighting, examples, styling |
| **Total** | **1-2 weeks** | Functional try.turmeric-lang.io |

---

## Part 2: Interactive Tutorial

### Design Philosophy

- **Integrated:** Accessible from within the REPL (both web and CLI)
- **Progressive:** Start simple, build to advanced concepts
- **Hands-on:** Users type code, not just read
- **Immediate feedback:** Instant validation and hints
- **Optional:** Users can skip or exit at any time

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        REPL Session                           │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Tutorial Mode                                        │  │
│  │                                                       │  │
│  │  Lesson: Basics                                        │  │
│  │  ─────────────                                        │  │
│  │                                                       │  │
│  │  Step 1: Hello World                                  │  │
│  │  Type: (println "Hello!")                             │  │
│  │  > (println "Hello!")                                 │  │
│  │  Hello!                                               │  │
│  │  ✓ Correct!                                           │  │
│  │                                                       │  │
│  │  Step 2: Addition                                     │  │
│  │  Type: (+ 1 2)                                        │  │
│  │  >                                                   │  │
│  │  [Hint] [Skip] [Quit Tutorial]                        │  │
│  └───────────────────────────────────────────────────────┘  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Tutorial Content Structure

Tutorials are defined as JSON/YAML files with steps:

```yaml
# tutorials/basics.yaml
id: basics
title: Turmeric Basics
description: Learn the fundamentals of Turmeric syntax
steps:
  - id: hello_world
    title: Hello World
    instruction: Type a hello world program
    expected: '(println "Hello, Turmeric!")'
    hints:
      - Try using the println function
      - Strings are in double quotes
    success_message: Great! You printed your first Turmeric expression.
    
  - id: addition
    title: Basic Arithmetic
    instruction: Add two numbers together
    expected: '(+ 1 2)'
    alternate_accept: ['(+ 2 1)', '(+ 1 2 3)']
    hints:
      - Use the + function
      - Numbers are literals
    success_message: Addition works!
    
  - id: definitions
    title: Defining Functions
    instruction: Define a function that squares a number
    expected: '(defn square [x] (* x x))'
    hints:
      - Use defn to define a function
      - Functions have parameters in brackets
    success_message: Function definition complete!
    verify: '(square 5)'  # Run this to verify the definition
```

### Tutorial Categories

| Category | Description | Estimated Steps |
|----------|-------------|-----------------|
| **Basics** | Syntax, primitives, functions | 10-15 |
| **Data Structures** | Vectors, structs, pattern matching | 8-12 |
| **Type System** | Type annotations, inference | 10-15 |
| **Functions** | Closures, recursion, HOFs | 8-12 |
| **Effects** | Algebraic effects, handlers | 10-15 |
| **Memory** | Borrow checking, RC, GC | 8-12 |
| **FFI** | Calling C from Turmeric | 5-8 |
| **Advanced** | Typeclasses, macros, continuations | 12-18 |

### Command Integration

**New meta-commands for REPL:**

| Command | Description |
|---------|-------------|
| `:tutorial` | List available tutorials |
| `:tutorial <name>` | Start a tutorial |
| `:tutorial <name> <step>` | Jump to specific step |
| `:next` | Go to next step |
| `:prev` | Go to previous step |
| `:hint` | Show hint for current step |
| `:skip` | Skip current step |
| `:quit-tutorial` | Exit tutorial mode |

**CLI REPL integration:**
- Tutorials bundled as resource files
- Read from `TUTORIAL_PATH` environment variable or installed data directory

**Web REPL integration:**
- Tutorials loaded via HTTP or bundled in WASM
- Same command interface as CLI

### Tutorial Engine

```c
// src/tutorial.c (new)
typedef struct {
    const char *id;
    const char *title;
    const char *instruction;
    const char *expected;
    const char **alternate_accept;
    const char **hints;
    const char *success_message;
    const char *verify_expr;
} TutorialStep;

typedef struct {
    const char *id;
    const char *title;
    const char *description;
    TutorialStep *steps;
    int step_count;
} Tutorial;

// Load tutorial by name
Tutorial *tutorial_load(const char *name);

// Check if input matches expected
bool tutorial_check(TutorialStep *step, const char *input);

// Get hint for step
const char *tutorial_hint(TutorialStep *step, int hint_index);
```

### Step Validation

**Matching strategies:**
1. **Exact match** - Input equals expected exactly
2. **Normalized match** - Whitespace-insensitive comparison
3. **AST equivalent** - Parse and compare ASTs (for different but equivalent code)
4. **Output match** - Evaluate and compare output
5. **Regex match** - For flexible patterns

**Implementation levels:**
```c
// Level 1: String comparison (simple, fast)
bool match_exact(const char *input, const char *expected) {
    return strcmp(input, expected) == 0;
}

// Level 2: Normalized comparison
bool match_normalized(const char *input, const char *expected) {
    char *norm_input = normalize_whitespace(input);
    char *norm_expected = normalize_whitespace(expected);
    bool result = strcmp(norm_input, norm_expected) == 0;
    free(norm_input);
    free(norm_expected);
    return result;
}

// Level 3: AST comparison (most accurate)
bool match_ast(const char *input, const char *expected) {
    Ast *input_ast = parse(input);
    Ast *expected_ast = parse(expected);
    bool result = ast_equal(input_ast, expected_ast);
    ast_free(input_ast);
    ast_free(expected_ast);
    return result;
}
```

### Tutorial State Management

**CLI REPL:**
- Store state in REPL session struct
- Persist progress in `~/.turmeric/tutorial-progress.json`

**Web REPL:**
- Store in browser's localStorage
- Optionally sync with GitHub account (future enhancement)

```c
// In REPL session
typedef struct {
    Tutorial *tutorial;
    int current_step;
    bool in_tutorial;
    // Track completed tutorials
    HashSet *completed_tutorials;
} TutorialState;
```

### Tutorial Discovery

**File locations:**
- Installed: `<prefix>/share/turmeric/tutorials/`
- Development: `tutorials/` directory in repo
- User-defined: `~/.turmeric/tutorials/`

**Format detection:**
- `.yaml` or `.yml` files
- `.tur-tutorial` custom format (future)

### Advanced Features

1. **Branching tutorials** - Different paths based on user choices
2. **Adaptive difficulty** - Adjust based on user performance
3. **Achievements** - Badges for completing tutorials
4. **Community tutorials** - User-submitted tutorials
5. **Multi-language support** - i18n for tutorial text

### Tutorial Development

**Authoring tools:**
- YAML-based format for easy editing
- Validation script to check tutorial files
- Preview mode for testing tutorials

**Example validation:**
```python
# scripts/validate_tutorials.py
def validate_tutorial(tutorial_path):
    with open(tutorial_path) as f:
        tutorial = yaml.safe_load(f)
    
    # Check required fields
    assert 'id' in tutorial
    assert 'title' in tutorial
    assert 'steps' in tutorial
    
    # Check each step
    for i, step in enumerate(tutorial['steps']):
        assert 'id' in step
        assert 'instruction' in step
        assert 'expected' in step or 'verify_expr' in step
    
    # Try to parse expected expressions
    for step in tutorial['steps']:
        if 'expected' in step:
            validate_expression(step['expected'])
```

### Integration with Try Turmeric Site

The web-based REPL will have enhanced tutorial integration:

```
┌─────────────────────────────────────────────────────────────┐
│  [Try REPL] [🎓 Tutorials ▼] [Docs] [GitHub]                  │
├─────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────────┐  ┌─────────────────────────────────┐  │
│  │                 │  │  Tutorial: Basics                 │  │
│  │   Editor        │  │ ─────────────────────────────   │  │
│  │                 │  │                                     │  │
│  │                 │  │  Step 3: Defining Functions       │  │
│  │                 │  │                                     │  │
│  │                 │  │  Define a function that adds      │  │
│  │                 │  │  two numbers:                     │  │
│  │                 │  │                                     │  │
│  │  (defn add      │  │  > (defn add [a b] (+ a b))      │  │
│  │    [a b]       │  │  ✓ Correct!                       │  │
│  │    (+ a b))    │  │                                     │  │
│  │                 │  │  [Next Step] [Hint] [Skip]        │  │
│  │                 │  │                                     │  │
│  └─────────────────┘  └─────────────────────────────────┘  │
│                                                                 │
│  Progress: ████████░░░░░░ 3/15 steps                          │
└─────────────────────────────────────────────────────────────┘
```

**Features:**
- Side-by-side or stacked layout (responsive)
- Visual progress indicator
- Tutorial navigation in sidebar
- Resume where you left off
- Tutorial catalog with descriptions and difficulty levels

### Timeline (Estimated)

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| Tutorial format design | 1 day | YAML schema, example tutorial |
| Tutorial engine (C) | 2-3 days | Loading, validation, state management |
| REPL integration | 2 days | Meta-commands, display integration |
| CLI tutorial mode | 1 day | Testing and refinement |
| Web tutorial UI | 2-3 days | Frontend components, styling |
| Content creation | 3-5 days | 5-8 tutorial sets (20-40 steps each) |
| **Total** | **2-3 weeks** | Full tutorial system |

---

## Combined Timeline

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| Phase 1: WASM & Web REPL | 1-2 weeks | try.turmeric-lang.io with basic REPL |
| Phase 2: Tutorial Engine | 2-3 weeks | CLI tutorial system working |
| Phase 3: Web Tutorial UI | 2-3 days | Tutorials integrated into web REPL |
| Phase 4: Content | Ongoing | Tutorial content (can be added incrementally) |
| **Total MVP** | **3-5 weeks** | Both features functional |

---

## File Changes Summary

### New Files

| File | Purpose |
|------|---------|
| `src/wasm_glue.c` | WASM-friendly API wrapper for libturi |
| `src/wasm_glue.h` | Header for WASM glue |
| `src/tutorial.c` | Tutorial engine implementation |
| `src/tutorial.h` | Tutorial engine header |
| `web/index.html` | Main HTML page for try site |
| `web/main.js` | Main JavaScript for web REPL |
| `web/styles.css` | Styling for web site |
| `web/examples.js` | Example code snippets |
| `web/package.json` | Node dependencies |
| `web/vite.config.js` | Build configuration |
| `tutorials/basics.yaml` | Basics tutorial |
| `tutorials/data-structures.yaml` | Data structures tutorial |
| `tutorials/type-system.yaml` | Type system tutorial |
| `tutorials/functions.yaml` | Functions tutorial |
| `tutorials/effects.yaml` | Effects tutorial |
| `tutorials/memory.yaml` | Memory management tutorial |
| `tutorials/ffi.yaml` | FFI tutorial |
| `docs/try-turmeric-and-tutorial-plan.md` | This document |

### Modified Files

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add WASM target, tutorial source files |
| `src/repl.c` | Add tutorial meta-commands |
| `src/repl.h` | Add tutorial state to REPL struct |
| `Justfile` | Add wasm, web, deploy targets |

---

## Success Criteria

### Try Turmeric Site
- [ ] WASM module compiles and loads in browser
- [ ] Editor allows typing Turmeric code
- [ ] Code executes and displays output
- [ ] Examples are preloaded and runnable
- [ ] URL sharing works
- [ ] Mobile-responsive design

### Interactive Tutorial
- [ ] Tutorials can be listed from REPL
- [ ] Tutorials can be started and stepped through
- [ ] Input validation works (exact, normalized, AST)
- [ ] Hints are available
- [ ] Progress is saved between sessions
- [ ] Tutorials work in both CLI and web REPLs

### Stretch Goals
- [ ] Syntax highlighting in web editor
- [ ] Autocomplete in web editor
- [ ] Multiple themes (light/dark)
- [ ] Keyboard shortcuts
- [ ] Tutorial authoring documentation
- [ ] Community tutorial submission flow
- [ ] Achievement system
- [ ] Social sharing of tutorial completion

---

## Open Questions

1. **WASM size:** Should we use WASM optimizations to reduce file size at the cost of compile time?
2. **Tutorial format:** YAML vs JSON vs custom format?
3. **AST matching:** How sophisticated should the equivalence checking be?
4. **Progress tracking:** Should tutorial progress sync across devices?
5. **Hosting:** GitHub Pages vs Netlify vs custom domain?
6. **Offline support:** Should the web REPL work offline (Service Worker + cache)?

---

## Next Steps

1. ✅ Create this plan document
2. [ ] Set up Emscripten build environment
3. [ ] Create minimal WASM compilation of libturi
4. [ ] Build basic web page with Monaco editor
5. [ ] Integrate WASM and web REPL
6. [ ] Design tutorial YAML format
7. [ ] Implement tutorial engine in C
8. [ ] Add tutorial meta-commands to REPL
9. [ ] Create first tutorial (Basics)
10. [ ] Test and refine

---

## References

- [Emscripten Documentation](https://emscripten.org/docs/)
- [Monaco Editor](https://microsoft.github.io/monaco-editor/)
- [VS Code Syntax Highlighting](https://code.visualstudio.com/api/language-extensions/syntax-highlighting)
- Existing Turmeric REPL: `docs/repl.md`
- Existing libturi API: `docs/eval-api.md`
