# Module System Implementation Plan for Turmeric

> **Status:** Draft — Not Started  
> **Prerequisite:** Phases 0-7 must be complete (bootstrap interpreter, macro system)  
> **Target:** v1 or v2  
> **Related:** See [turmeric-plan.md §12.3](turmeric-plan.md) for background and design constraints

---

## Executive Summary

The module system introduces namespaced, self-contained units of code with controlled visibility, export/import mechanisms, and separate compilation. This enables large-scale code organization, prevents naming collisions, and allows selective exposure of APIs.

**Primary motivators:**
1. Code organization at scale (libraries, applications with multiple components)
2. Namespace isolation (prevent symbol collisions)
3. Selective API exposure (encapsulation)
4. Separate compilation (incremental builds)
5. Cross-module macro export
6. Module-level resource management (`defer` at process exit)

**Decision rule:** Modules are a stretch goal for v1. Ship when users report namespace collisions or demand library distribution mechanisms. v0/v1 can ship with a flat namespace and add modules later.

---

## Phase Overview

| Phase | Deliverable | Exit Criterion | Estimated Effort |
|---|---|---|---|
| M0 | Module syntax foundation | `defmodule`, `export`, `import` forms parsed and validated | Medium (1-2 weeks) |
| M1 | Module namespace system | Per-module symbol tables, qualified name resolution | Medium (2-3 weeks) |
| M2 | Import/export processing | Resolve imports, validate exports, handle aliases and `:refer` | Medium (2-3 weeks) |
| M3 | Separate compilation | One `.c` + `.h` pair per module, cross-module linking | Hard (3-4 weeks) |
| M4 | Macro export | Serialize macro `Form` bodies, re-evaluate in importer context | Medium (1-2 weeks) |
| M5 | Module-level defer | Top-level `(defer ...)` registered via `atexit` | Medium (1 week) |
| M6 | C symbol mangling | Module-qualified C symbols (`module__name`), visibility control | Medium (1-2 weeks) |
| M7 | Integration & polish | stdlib migration, documentation, performance benchmarks | Medium (1-2 weeks) |

---

## Prerequisites Checklist

Before starting Phase M0, verify:

- [ ] Phases 0-7 are complete (reader, core forms, functions, closures, defer, ref, borrow/move/lifetimes)
- [ ] Bootstrap interpreter (§6) operates on `Form` lists (for macro export)
- [ ] Compiler emits `.c` + `.h` even for single compilation unit
- [ ] Symbol naming in codegen uses `tur__` prefix (easy to extend to `module__name`)
- [ ] `Form` ADT (§11) is stable and can carry module metadata
- [ ] Arena allocator supports multi-unit compilation
- [ ] Error reporting with spans flows through all phases

---

## Module System Design Decisions

### File-Module Relationship

**Decision: Hybrid (Clojure-style) — Option 3 from turmeric-plan.md**

- `defmodule` is **required** at the top of each file
- Module name **must match** the file path: `src/geom/vector.tur` declares `(defmodule geom/vector ...)`
- Compiler errors if names don't match
- Benefits: explicitness + discoverability, clear loader, no manifest needed

### Visibility Default

**Decision: Private-by-default with explicit export**

- All definitions are private by default
- Must be listed in `(export ...)` to be visible outside the module
- `defn-` and `def-` are explicitly private (redundant but allowed)
- Encourages narrow surface areas and explicit API design

### Circular Imports

**Decision: Error in v1**

- Detect and report circular import chains
- Topological sort for future versions (stretch goal)
- Lazy resolution not implemented in v1

### Build Artifacts

**Decision: One `.c` + `.h` pair per module**

- Each module compiles to its own `.c` and `.h` files
- Header contains declarations (not definitions) for exported symbols
- `.h` regenerated on every compile
- Linker resolves cross-module references

---

## Phase M0 — Module Syntax Foundation

**Goal:** Add parsing and basic validation for module-related forms.

### Tasks

#### Reader extensions (`src/reader.c`)
- [ ] Recognize `defmodule` as a special form
- [ ] Recognize `export` as a special form
- [ ] Recognize `import` as a special form
- [ ] Support `:as` keyword for module aliases
- [ ] Support `:refer` keyword for selective imports
- [ ] Reserve `defmodule` at file top-level only (error if not first form)

#### AST nodes (`src/ast.h`)
- [ ] Define `AST_DefModule` node type
  - Module name (symbol)
  - Export list (vector of symbols)
  - Body (vector of forms)
  - Source span
- [ ] Define `AST_Export` node type
  - List of exported symbols
  - Source span
- [ ] Define `AST_Import` node type
  - Module to import (symbol)
  - Optional `:as` alias (symbol or null)
  - Optional `:refer` list (vector of symbols or null for all)
  - Source span

#### Parser validation (`src/reader.c` or new `src/module.c`)
- [ ] `defmodule` must be first form in file
- [ ] Module name must be a valid symbol (no special characters except `/` for nested modules)
- [ ] Only one `defmodule` per file
- [ ] `export` form only allowed inside `defmodule`
- [ ] `import` forms only allowed inside `defmodule` (or at top level of module body)
- [ ] Validate that `:as` alias is a valid symbol
- [ ] Validate that `:refer` symbols are valid identifiers

---

## Phase M1 — Module Namespace System

**Goal:** Implement per-module symbol tables and qualified name resolution.

### Tasks

#### Compiler state (`src/compiler.h`)
- [ ] Add `current_module` field to compiler state
- [ ] Add module stack for nested module contexts (if supporting nested `defmodule`)
- [ ] Add module symbol table type: `typedef struct ModuleSymbolTable ModuleSymbolTable;`
- [ ] Define module symbol table structure with:
  - Hash map: symbol → definition
  - Parent module reference (for nested modules)
  - Export list
  - Import list

#### Symbol table management (`src/module.c`)
- [ ] Create module symbol table when `defmodule` is encountered
- [ ] Push module onto compiler state stack
- [ ] Pop module when exiting module body
- [ ] Register definitions in current module's symbol table
- [ ] Store definition metadata: symbol, type, visibility (public/private), source location

#### Name resolution (`src/resolve.c` — new or extend existing)
- [ ] Resolve unqualified symbols in current module first
- [ ] Resolve qualified symbols (`module/name`) by:
  - Split at `/` to get module name and symbol
  - Look up module in import table
  - Look up symbol in module's export list
  - Report error if module or symbol not found
- [ ] Support `:as` aliases in resolution
- [ ] Support `:refer` symbols as local aliases
- [ ] Error on ambiguous references (same symbol imported from multiple modules)

#### Visibility tracking
- [ ] Mark each definition as public or private based on `export` list
- [ ] Error on access to private definitions from other modules
- [ ] Track `defn-` / `def-` as explicitly private

---

## Phase M2 — Import/Export Processing

**Goal:** Resolve module dependencies, validate exports, and handle import options.

### Tasks

#### Import resolution (`src/module.c`)
- [ ] Build import dependency graph
- [ ] Track which modules have been loaded/processed
- [ ] Resolve module names to file paths:
  - Map `geom/vector` to `src/geom/vector.tur` (configurable path mapping)
  - Support standard library module paths
  - Support user-defined module search paths
- [ ] Process `import` forms:
  - Load referenced module (if not already loaded)
  - Register module in current module's import table
  - Register `:as` alias mapping
  - Register `:refer` symbol mappings in current module's symbol table
- [ ] Detect circular import chains and report errors

#### Export validation (`src/module.c`)
- [ ] Validate that all symbols in `export` list exist in module
- [ ] Error on exporting non-existent symbols
- [ ] Error on exporting private definitions (`defn-`, `def-`)
- [ ] Build export table for module:
  - Map exported symbol → actual definition
  - Track original symbol name (for re-export)

#### Module metadata (`src/ast.h`)
- [ ] Store resolved import table in module AST node
- [ ] Store resolved export table in module AST node
- [ ] Store full module dependency list

#### Error reporting
- [ ] Error: module not found
- [ ] Error: circular import detected (list the cycle)
- [ ] Error: symbol not exported from module
- [ ] Error: ambiguous symbol (imported from multiple modules)
- [ ] Warning: unused import
- [ ] Warning: unused `:refer` symbol

---

## Phase M3 — Separate Compilation

**Goal:** Emit one `.c` + `.h` pair per module with proper cross-module linking.

### Tasks

#### Header generation (`src/emit.c` — extend existing)
- [ ] Modify `emit_header` to accept module context
- [ ] For each exported definition, emit declaration in header:
  - Functions: function signature with `extern`
  - Types: struct/union declarations
  - Constants: `extern const` declarations
  - Macros: macro definitions (or forward declarations for cross-module)
- [ ] Include guard macro: `#ifndef MODULE__NAME__H ... #define MODULE__NAME__H ... #endif`
- [ ] Include dependencies: `#include` for each imported module's header

#### Implementation generation (`src/emit.c` — extend existing)
- [ ] Modify `emit_implementation` to accept module context
- [ ] For each definition, emit implementation:
  - Functions: full function definition
  - Types: struct/union definitions
  - Constants: definition with initial value
- [ ] Mark module-private definitions as `static`
- [ ] Mark exported definitions with module-prefix in C symbol name

#### C symbol mangling (`src/emit.c`)
- [ ] Define mangling scheme: `module__name` for exported symbols
- [ ] Replace special characters in module names:
  - `/` → `__` (so `geom/vector` → `geom__vector`)
  - `-` → `_`
  - `.` → `_`
- [ ] Handle name collisions after mangling (should not happen with proper module naming)
- [ ] Document mangling scheme as part of ABI

#### Build system integration (`src/main.c` — extend existing)
- [ ] Modify `compile_to_h` to handle per-module compilation
- [ ] Modify `compile_to_implementation` to handle per-module compilation
- [ ] Process all `.tur` files in input directory
- [ ] Generate `_main.c` that includes all module headers
- [ ] Link all generated `.c` files together

#### Incremental compilation support
- [ ] Track file modification times
- [ ] Only recompile modules whose source or dependencies have changed
- [ ] Cache compiled modules

---

## Phase M4 — Macro Export

**Goal:** Enable macros to cross module boundaries by serializing and re-evaluating `Form` bodies.

### Tasks

#### Macro serialization (`src/macro.c` — extend existing)
- [ ] Define serialization format for `Form` ADT
- [ ] Serialize macro definition to portable representation
- [ ] Store serialized macros in module metadata
- [ ] Handle special forms in serialization
- [ ] Handle reader macros in serialization

#### Macro export/import (`src/module.c`)
- [ ] Track which macros are exported from each module
- [ ] Include macro definitions in module's export table
- [ ] When importing module, deserialized exported macros into current module's macro environment
- [ ] Validate macro names don't collide with existing macros

#### Macro re-evaluation (`src/macro.c`)
- [ ] Re-evaluate serialized macro `Form` in importer's expansion context
- [ ] Preserve macro metadata (docstrings, etc.) across module boundaries
- [ ] Handle macro dependencies (macros that use other macros from same or different modules)

#### Error handling
- [ ] Error: macro not found in imported module
- [ ] Error: macro name collision on import
- [ ] Warning: macro shadowing local definition

---

## Phase M5 — Module-Level Defer

**Goal:** Support top-level `(defer ...)` forms that run at process exit via `atexit`.

### Tasks

#### AST extension (`src/ast.h`)
- [ ] Track top-level `defer` forms in module AST
- [ ] Distinguish module-level defers from function-level defers

#### Code generation (`src/emit.c`)
- [ ] Generate `atexit()` registration for module-level defers
- [ ] Store defer callbacks in global table
- [ ] Ensure defers are registered before `main()` is called

#### Runtime (`src/runtime.c`)
- [ ] Define module defer table structure
- [ ] Implement `register_module_defer()` function
- [ ] Call registered defers at process exit
- [ ] Handle defer ordering (FIFO or LIFO? Match function-level defer semantics)

#### Ordering semantics
- [ ] Module defers run after all function-level defers from `main()`
- [ ] Module defers run in reverse order of module initialization (LIFO)
- [ ] Within a module, defers run in order of definition (FIFO)

---

## Phase M6 — C Symbol Mangling and Visibility

**Goal:** Implement robust C symbol naming and visibility control for cross-module linking.

### Tasks

#### Mangling scheme finalization
- [ ] Finalize character replacement rules
- [ ] Define reserved prefixes (e.g., `tur__` for internal, `module__` for exported)
- [ ] Document ABI stability guarantees

#### Visibility control (`src/emit.c`)
- [ ] Emit `static` for module-private definitions
- [ ] Emit `extern` for exported definitions in headers
- [ ] Omit `static` for exported definitions in implementations
- [ ] Handle `extern-c` declarations at module scope:
  - `extern` for exported C declarations
  - `static` for module-private C declarations
- [ ] Handle inline-C blocks at module scope with appropriate visibility

#### Name collision detection
- [ ] Detect C symbol collisions after mangling
- [ ] Error if two modules export symbols that mangle to the same C name
- [ ] Suggest solutions in error messages

#### FFI considerations
- [ ] Support `extern-c` for exporting C-compatible symbols
- [ ] Allow explicit C name specification: `(defn ^:export-as("c_name") foo ...)`
- [ ] Document FFI naming conventions

---

## Phase M7 — Integration and Polish

**Goal:** Migrate stdlib, add documentation, and ensure performance.

### Tasks

#### Stdlib migration
- [ ] Convert all stdlib files to use `defmodule`
- [ ] Define stdlib module hierarchy
- [ ] Add `export` lists to all stdlib modules
- [ ] Ensure all stdlib modules compile with module system
- [ ] Create facade modules for convenience (e.g., `prelude` that re-exports common functions)

#### Documentation
- [ ] Write user-facing module system documentation
- [ ] Document module naming conventions
- [ ] Document import/export syntax and options
- [ ] Document visibility rules
- [ ] Document build system integration
- [ ] Add module system examples to tutorial

#### Performance
- [ ] Benchmark module loading times
- [ ] Benchmark compilation times with many modules
- [ ] Optimize symbol table lookups
- [ ] Consider caching resolved imports

#### Testing
- [ ] Add module system fixtures:
  - Basic module definition and export
  - Module import with `:as` and `:refer`
  - Cross-module function calls
  - Cross-module type usage
  - Cross-module macro usage
  - Private definition access errors
  - Circular import detection
  - Module-level defer
  - C symbol mangling
  - Nested module paths
  - Re-export via facade modules

#### Error messages
- [ ] Review and improve all module-related error messages
- [ ] Add helpful suggestions for common mistakes
- [ ] Ensure spans are accurate for module forms

---

## Data Structures Reference

### Module AST Node

```c
typedef struct {
    Symbol name;              // Module name (e.g., geom/vector)
    Vector export_list;      // List of exported symbols
    Vector import_list;      // List of ImportSpec
    Vector body;             // Module body forms
    Span span;               // Source span
    
    // Resolved during compilation
    ModuleSymbolTable *symtab;
    Vector dependencies;    // List of module names this module imports
    Vector exported_syms;    // Map: exported name -> Definition*
    Vector imported_modules; // Map: alias -> Module*
    bool processed;          // Has this module been fully processed?
} AST_DefModule;

typedef struct {
    Symbol module_name;      // Module to import
    Symbol alias;            // :as alias (or null)
    Vector refer_list;       // :refer symbols (or null for all)
    Span span;
} ImportSpec;
```

### Module Symbol Table

```c
typedef struct ModuleSymbolTable {
    Map symbols;             // symbol -> Definition*
    Map exports;             // exported symbol -> Definition*
    Map imports;             // alias -> Module*
    Map refer_map;           // referred symbol -> Definition*
    struct ModuleSymbolTable *parent;
    Symbol module_name;
} ModuleSymbolTable;

typedef struct {
    Symbol name;
    enum { DEF_FN, DEF_TYPE, DEF_CONST, DEF_MACRO, DEF_VAR } kind;
    void *definition;        // Points to AST node or other definition
    bool is_public;
    bool is_exported;
    Span span;
} Definition;
```

---

## Open Questions

1. **Nested modules:** Should `geom/vector/3d` be a valid module name? How are file paths mapped?
   - Lean: Yes, with directory structure matching module path

2. **Module reloading:** Should modules support hot-reloading during development?
   - Lean: Not in v1; defer to future version

3. **Dynamic imports:** Should there be a `(require ...)` form for dynamic imports?
   - Lean: Not in v1; all imports must be static and at top level

4. **Module metadata:** Should modules support docstrings, version info, author info?
   - Lean: Yes, add `(defmodule geom/vector "Vector math library" ...)` with optional docstring

5. **Module aliases in `:refer`:** Should `:refer` support aliasing individual symbols?
   - Lean: Defer; can be added as `(import geom :refer [distance :as dist])` later

6. **Re-export syntax:** Should there be explicit re-export syntax?
   - Lean: Use `(export-from other-module foo bar)` as a macro initially

7. **Module-private types in public signatures:** Should this be an error or warning?
   - Lean: Error; the type system will enforce this

8. **Wildcard imports:** Should `(import module :refer :all)` be supported?
   - Lean: No in v1; explicit is better than implicit

---

## Migration Path from Flat Namespace

1. **No breaking changes:** All existing flat-namespace code continues to work
2. **Wrapped modules:** Add `(defmodule main ...)` around existing code
3. **Default exports:** Initially export all public definitions from main module
4. **Gradual adoption:** Users can add `defmodule` to new files while keeping old code flat
5. **Compiler flag:** `tur --modules` to enable module system (off by default initially)

---

## Dependencies on Other Systems

| Module System Feature | Dependency | Status |
|---|---|---|
| `defmodule` parsing | Reader (Phase 1) | Complete |
| `Form` serialization | Bootstrap interpreter (Phase 6) | Complete |
| Per-module symbol tables | Scope system | Needs extension |
| Cross-module macro export | Macro system (Phase 6) | Complete |
| Separate compilation | Emit `.c` + `.h` (Phase 2) | Complete |
| Module-level defer | Defer system (Phase 4) | Complete |
| Type checking across modules | Type system (Phase 15+) | Needs coordination |

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Circular import detection performance | Medium | Medium | Use efficient graph algorithms, cache results |
| Symbol table memory usage | Medium | Medium | Use arena allocation, share common structures |
| Macro serialization compatibility | Low | High | Version serialization format, test extensively |
| C symbol name collisions | Low | High | Use robust mangling scheme, validate at link time |
| Build system complexity | High | Medium | Keep per-file compilation simple, defer advanced features |

---

## Success Criteria

- [ ] All module system fixtures pass
- [ ] Stdlib compiles with module system enabled
- [ ] No performance regression for single-module programs
- [ ] Clear error messages for all module-related errors
- [ ] Documentation is complete and accurate
- [ ] Migration guide for existing code

---

## Appendix A: Example Module Usage

```clojure
;; math/vector.tur
(defmodule math/vector
  "2D vector operations"
  (export Point make-vector vector-x vector-y magnitude distance)
  
  (defstruct Point [x : float, y : float])
  
  (defn make-vector [x : float, y : float] : Point
    (Point x y))
  
  (defn vector-x [^Point p] : float (:get p :x))
  (defn vector-y [^Point p] : float (:get p :y))
  
  (defn magnitude [^Point p] : float
    (math/sqrt (+ (* (vector-x p) (vector-x p))
                (* (vector-y p) (vector-y p)))))
  
  (defn distance [^Point a, ^Point b] : float
    (magnitude (make-vector (- (vector-x b) (vector-x a))
                             (- (vector-y b) (vector-y a))))))

;; math.tur
(defmodule math
  (export sqrt)
  
  (extern-c "#include <math.h>")
  (defn sqrt [x : float] : float (sqrt x)))

;; user.tur
(defmodule user
  (import math :as m)
  (import math/vector :refer [Point make-vector distance])
  
  (defn main [] : int
    (let [p1 (make-vector 0.0 0.0)
          p2 (make-vector 3.0 4.0)]
      (printf "Distance: %f\n" (distance p1 p2))
      (printf "Square root of 16: %f\n" (m/sqrt 16.0))
      0)))
```

---

## Appendix B: Generated C Output

```c
/* math__vector.h */
#ifndef MATH__VECTOR__H
#define MATH__VECTOR__H

#include "math__h.h"

typedef struct {
    float x;
    float y;
} math__vector__Point;

extern math__vector__Point math__vector__make_vector(float x, float y);
extern float math__vector__vector_x(math__vector__Point p);
extern float math__vector__vector_y(math__vector__Point p);
extern float math__vector__magnitude(math__vector__Point p);
extern float math__vector__distance(math__vector__Point a, math__vector__Point b);

#endif /* MATH__VECTOR__H */

/* math__vector.c */
#include "math__vector.h"

math__vector__Point math__vector__make_vector(float x, float y) {
    return (math__vector__Point){x, y};
}

float math__vector__vector_x(math__vector__Point p) {
    return p.x;
}

float math__vector__vector_y(math__vector__Point p) {
    return p.y;
}

float math__vector__magnitude(math__vector__Point p) {
    return math__sqrt((p.x * p.x) + (p.y * p.y));
}

float math__vector__distance(math__vector__Point a, math__vector__Point b) {
    return math__vector__magnitude(math__vector__make_vector(
        b.x - a.x, b.y - a.y));
}
```
