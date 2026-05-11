# Module System — Racket-Style Alternative Plan

> **Status:** Draft — Alternative Design Proposal  
> **Compare with:** [module-system-plan.md](module-system-plan.md) (Clojure-style, currently adopted)  
> **Target:** v1 or v2  
> **Related:** See [turmeric-plan.md](turmeric-plan.md) for background

---

## Executive Summary

This document proposes a **Racket-style module system** as an alternative to the Clojure-inspired `defmodule` design in [module-system-plan.md](module-system-plan.md). The core differences are:

1. **`module` is a first-class syntactic form** with an explicit *language parameter*, enabling language towers and embedded DSLs.
2. **`provide`/`require` instead of `export`/`import`**, with richer combinator-based import/export transformers.
3. **Multiple modules per file** — a file may contain several named `module` forms; submodules are first-class.
4. **Explicit compilation phases** — macro dependencies are imported via `(require (for-syntax ...))`, cleanly separating compile-time and run-time dependency graphs.
5. **`module+` submodules** — accumulate test code alongside the code under test without a separate test file.
6. **Units** (v2) — first-class linkable components with explicit signatures, for dependency injection and parameterized libraries.

### When to choose this design over the Clojure-style plan

| Concern | Clojure-style | Racket-style |
|---|---|---|
| Mental model | Familiar to Clojure/Java users | Familiar to Scheme/Racket users |
| Test co-location | Separate test files | `module+ test` inline |
| DSL authoring | Not addressed | Language parameter enables `#%module-begin` |
| Macro phases | All macros at all phases (implicit) | Explicit `for-syntax`, clean phase separation |
| Multiple modules per file | One file = one module (enforced) | Many modules per file allowed |
| Re-export | `export-from` macro workaround | `all-from-out` / `re-provide` built-in |
| Parametric modules | Not addressed | Units / signatures (v2) |

---

## Phase Overview

| Phase | Deliverable | Exit Criterion | Estimated Effort |
|---|---|---|---|
| R0 | `module` form + language parameter | `module`, `provide`, `require` parsed; language param validated | Medium (1–2 weeks) |
| R1 | `provide` transformer system | `all-defined-out`, `rename-out`, `protect-out`, `except-out` | Medium (1–2 weeks) |
| R2 | `require` combinator system | `only-in`, `except-in`, `prefix-in`, `rename-in`, `all-from-out` | Medium (2–3 weeks) |
| R3 | Phased imports | `for-syntax`, `for-template`, `for-label` phases; phase-separated symbol tables | Hard (3–4 weeks) |
| R4 | Submodules (`module+` / `module*`) | Inline test submodules; per-file multi-module support | Medium (2–3 weeks) |
| R5 | Separate compilation | One `.c`+`.h` per module; cross-module linking; C mangling | Hard (3–4 weeks) |
| R6 | Language protocol (`#%module-begin`) | Any module usable as a language; stdlib DSL modules | Hard (2–3 weeks) |
| R7 | Units and signatures (v2) | `define-signature`, `define-unit`, `invoke-unit`, `link` | Hard (4–5 weeks) |
| R8 | Integration & polish | Stdlib migration; documentation; benchmarks; tests | Medium (1–2 weeks) |

---

## Prerequisites Checklist

Same as the Clojure-style plan, plus:

- [ ] `Form` ADT stable and can carry phase metadata
- [ ] Macro expander has a phase concept (even if only phase 0 and phase 1 in v1)
- [ ] Elaborator supports a "language environment" concept (the base bindings a module starts with)
- [ ] C codegen uses `tur__` prefix on all symbols (easy to extend to `module__sym`)

---

## Core Design Decisions

### The `module` Form

Every Turmeric source file is implicitly a **top-level module** named after its path. Optionally, a file may contain explicit named `module` forms:

```scheme
;; src/geom/vector.tur
(module geom/vector turmeric/base
  (require turmeric/math)

  (provide distance cross-product scale)

  (defn distance [a : vec2, b : vec2] : float ...)
  (defn cross-product [a : vec2, b : vec2] : float ...)
  (defn- magnitude [v : vec2] : float ...)   ; not provided — private
  (defn scale [v : vec2, s : float] : vec2 ...))
```

The second argument to `module` is the **language parameter** — it names the module that supplies the initial bindings and expander for the module body. `turmeric/base` is the default language (all standard forms). A different language like `turmeric/testing` could provide a DSL with test-specific forms.

**Contrast with Clojure-style:** In the existing plan, `defmodule` is a declaration with an export list embedded in it; `provide` forms here are separate and may appear anywhere in the module body (making it easy to co-locate declarations with the code they describe).

### File-Module Relationship

**Decision: Flexible — one file may contain zero or more explicit `module` forms.**

- If a file has **no** explicit `module` form, the file itself is treated as an anonymous module named after its path (same as the Clojure-style plan for migration compatibility).
- If a file has **one** `module` form, the name need not match the file path (the compiler warns, but does not error, making rename/move workflows easier).
- If a file has **multiple** `module` forms, each is independently compiled and linked. The file path does not constrain module names.

This contrasts sharply with the Clojure plan, which requires a 1:1 match between file path and module name.

### `provide` — Exporting Bindings

`provide` forms control what is visible outside the module. Anything not `provide`d is private by default.

```scheme
; Export specific names
(provide distance cross-product)

; Export all definitions in this module (no re-exports)
(provide (all-defined-out))

; Rename on export
(provide (rename-out [internal-distance distance]))

; Re-export everything imported from another module
(provide (all-from-out turmeric/math))

; Protect: exported but cannot be re-exported by importers
(provide (protect-out sensitive-fn))

; Exclude specific names from a bulk export
(provide (except-out (all-defined-out) internal-helper))
```

`provide` may appear **anywhere** in the module body, not just at the top. This allows:

```scheme
(defn distance [a b] ...)
(provide distance)   ; co-located with definition
```

### `require` — Importing Bindings

```scheme
; Plain import — all provided names enter scope
(require geom/vector)

; Import only specific names
(require (only-in geom/vector distance))

; Import everything except specific names
(require (except-in geom/vector cross-product))

; Add a prefix to all imported names
(require (prefix-in vec: geom/vector))
; → uses vec:distance, vec:cross-product, etc.

; Rename specific imports
(require (rename-in geom/vector [distance dist]))

; Re-export what you imported (combine require + provide)
(require (all-from-out geom/vector))     ; in provide form

; Combine combinators
(require (rename-in (only-in geom/vector distance) [distance dist]))
```

**`require` vs `:refer` / `:as`:** The Clojure plan uses `:refer`/`:as` keyword options; this plan uses composable combinator forms. The combinator approach is more uniform and extensible — adding a new kind of import transformation (e.g., `contract-in`) requires no new keywords, only a new combinator.

### Phased Imports (`for-syntax` / `for-template`)

Macro definitions run at **compile time (phase 1)**. Their dependencies must be available at compile time, not just run time. `for-syntax` shifts an import into phase 1:

```scheme
(module my-dsl turmeric/base
  (require (for-syntax turmeric/syntax-utils))  ; available at macro-expand time
  (require turmeric/runtime)                     ; available at run time

  (defmacro my-form [x]
    ; uses turmeric/syntax-utils here — compile time
    ...)

  (defn do-thing [] ...)
  (provide do-thing my-form))
```

Phase levels:

| Phase | `require` form | When bindings are available |
|---|---|---|
| 0 (run time) | `(require m)` | At run time |
| 1 (compile time) | `(require (for-syntax m))` | When expanding macros in this module |
| −1 (template time) | `(require (for-template m))` | When macro-generated code from this module is later expanded |
| label | `(require (for-label m))` | Static analysis only (not executed) |

**v1 scope:** Only phases 0 and 1 are required for v1. `for-template` and `for-label` are v2 stretch goals. The key v1 win is that `for-syntax` makes the compile-time/run-time split explicit — the compiler can then produce a clean dependency graph for separate compilation.

**Contrast with Clojure plan:** The Clojure-style plan serializes macro `Form` bodies and re-evaluates them in the importer context (Phase M4). The Racket approach instead separates the dependency graph by phase, which is simpler to reason about and avoids the serialization round-trip.

---

## Phase R0 — `module` Form and Language Parameter

**Goal:** Parse and validate the `module` form, the language parameter, and bare `provide`/`require`.

### Tasks

#### Reader extensions (`src/reader.c`)
- [ ] Recognize `module` as a special form (distinct from `defmodule`)
- [ ] Recognize `provide` as a special form
- [ ] Recognize `require` as a special form
- [ ] Parse `(module name lang body...)` — name is a symbol, lang is a symbol
- [ ] Support `module` at top level of a file and nested inside another `module`
- [ ] `provide` and `require` allowed anywhere in module body
- [ ] Reserve phase keywords: `for-syntax`, `for-template`, `for-label`

#### AST nodes (`src/expr.h` or new `src/module.h`)
- [ ] Define `EXPR_MODULE` node:
  - `name` — Symbol
  - `lang` — Symbol (language module name)
  - `body` — vector of `Expr *`
  - `span` — Span
- [ ] Define `EXPR_PROVIDE` node:
  - `specs` — vector of provide specs (see Phase R1)
  - `span` — Span
- [ ] Define `EXPR_REQUIRE` node:
  - `specs` — vector of require specs (see Phase R2)
  - `phase` — enum `{ PHASE_RUN, PHASE_SYNTAX, PHASE_TEMPLATE, PHASE_LABEL }`
  - `span` — Span

#### Validation (`src/reader.c`)
- [ ] Module name must be a valid qualified symbol (letters, digits, `/`, `-`, `_`)
- [ ] Language parameter must be a valid symbol
- [ ] Error if `module` form is not at top level of file (no nested `module` inside non-module forms)
- [ ] Warning if explicit module name does not match file path

#### Language parameter resolution (`src/module.c` — new)
- [ ] Look up language module in module search path
- [ ] Load language module's `#%module-begin` if present (Phase R6); otherwise use default
- [ ] Error if language module not found
- [ ] `turmeric/base` is always available (compiler built-in)

---

## Phase R1 — `provide` Transformer System

**Goal:** Implement all `provide` combinator forms.

### Provide combinator forms

```
(provide name ...)                         ; explicit list
(provide (all-defined-out))                ; all definitions in this module
(provide (rename-out [old new] ...))       ; export with rename
(provide (protect-out name ...))           ; exportable but not re-exportable
(provide (except-out spec name ...))       ; bulk export minus exclusions
(provide (all-from-out module ...))        ; re-export everything from another module
```

### Tasks

#### AST for provide specs (`src/module.h`)
- [ ] Define `ProvideSpec` tagged union:
  - `PROV_NAME` — single symbol
  - `PROV_ALL_DEFINED` — no fields
  - `PROV_RENAME_OUT` — vector of `(old, new)` pairs
  - `PROV_PROTECT_OUT` — vector of symbols
  - `PROV_EXCEPT_OUT` — inner `ProvideSpec *` + vector of excluded symbols
  - `PROV_ALL_FROM_OUT` — module symbol

#### Processing (`src/module.c`)
- [ ] Collect all `provide` forms in a module after parsing
- [ ] Expand each provide spec against the module's definition table:
  - `all-defined-out`: iterate all definitions in module scope
  - `rename-out`: map old name to new name in export table
  - `protect-out`: mark export as protected (cannot be re-exported via `all-from-out`)
  - `except-out`: expand inner spec, remove excluded names
  - `all-from-out`: pull all exported (non-protected) names from the named import
- [ ] Build final export table: `Map<Symbol, ExportEntry>`
- [ ] `ExportEntry` carries: internal symbol, exported symbol, `is_protected` flag, source span

#### Error reporting
- [ ] Error: `provide` refers to undefined name
- [ ] Error: `provide` tries to re-export a protected import
- [ ] Warning: `provide` duplicates a name already provided
- [ ] Warning: `all-defined-out` exports names that look internal (convention: prefixed `%`)

---

## Phase R2 — `require` Combinator System

**Goal:** Implement all `require` combinator forms.

### Require combinator forms

```scheme
(require module)                                   ; all provided names
(require (only-in module name ...))                ; whitelist
(require (except-in module name ...))              ; blacklist
(require (prefix-in pfx: module))                  ; add prefix to all names
(require (rename-in module [old new] ...))         ; rename specific names
(require (combine-in spec ...))                    ; compose specs (union)
```

### Tasks

#### AST for require specs (`src/module.h`)
- [ ] Define `RequireSpec` tagged union:
  - `REQ_MODULE` — module symbol (plain import)
  - `REQ_ONLY_IN` — inner `RequireSpec *` + vector of symbols
  - `REQ_EXCEPT_IN` — inner `RequireSpec *` + vector of symbols
  - `REQ_PREFIX_IN` — prefix symbol + inner `RequireSpec *`
  - `REQ_RENAME_IN` — inner `RequireSpec *` + vector of `(old, new)` pairs
  - `REQ_COMBINE_IN` — vector of `RequireSpec *`

#### Processing (`src/module.c`)
- [ ] Resolve `RequireSpec` against target module's export table:
  - `only-in`: intersect imported names with whitelist
  - `except-in`: subtract blacklist from imported names
  - `prefix-in`: prepend prefix string to all imported names
  - `rename-in`: apply rename mappings
  - `combine-in`: union of results from child specs (error on ambiguity)
- [ ] Register resulting bindings in current module's local scope
- [ ] Track origin module + original name for each binding (for error messages and re-export)

#### Error reporting
- [ ] Error: module not found
- [ ] Error: `only-in` names a symbol not exported by target module
- [ ] Error: `rename-in` old name not exported by target module
- [ ] Error: two require specs introduce the same local name (ambiguity)
- [ ] Warning: `except-in` excludes a name that does not exist in the module

#### Circular import detection
- [ ] Build directed dependency graph during require resolution
- [ ] Topological sort; report cycle if found
- [ ] Cycle error lists full import chain

---

## Phase R3 — Phased Imports

**Goal:** Separate compile-time and run-time dependency graphs; implement `for-syntax`.

### Phase model

Every module has a **phase-0 environment** (run-time bindings) and a **phase-1 environment** (compile-time / macro-expansion bindings). Importing a module at phase 1 makes that module's phase-0 exports available as phase-1 bindings in the importer.

```
          importer module
         ┌─────────────────────────────┐
         │ phase-1 env (macro expand)  │◄─── (require (for-syntax foo))
         │ phase-0 env (run time)      │◄─── (require foo)
         └─────────────────────────────┘
```

### Tasks

#### Phase-annotated symbol tables (`src/module.h`)
- [ ] Extend `ModuleEnv` to hold separate symbol tables per phase
- [ ] `phase_env[0]` — run-time bindings
- [ ] `phase_env[1]` — compile-time bindings
- [ ] Current phase tracked in elaborator context

#### `for-syntax` require processing (`src/module.c`)
- [ ] When `(require (for-syntax m))` is encountered, load `m` and install its phase-0 exports into importer's phase-1 environment
- [ ] Macros defined in this module may call functions from phase-1 env
- [ ] Run-time code may not refer to phase-1 bindings (error)

#### Elaborator integration (`src/elab.c`)
- [ ] Query correct phase environment during name resolution
- [ ] Phase-1 context: active during `defmacro` body expansion
- [ ] Phase-0 context: active during normal expression elaboration
- [ ] Error: reference to phase-1 binding at phase-0

#### Separate compilation implications
- [ ] Phase-1 dependencies compiled first (they are needed during the compile of the importer)
- [ ] Phase-0 dependencies compiled concurrently or lazily
- [ ] `.h` files include only phase-0 declarations

#### v1 scope
- [ ] v1: `for-syntax` and `for-template` stubs: treat `for-syntax` imports as compile-time only; do not execute at run time
- [ ] v2: full phase tower with `for-template` and `for-label`

---

## Phase R4 — Submodules (`module+` and `module*`)

**Goal:** Allow test code (and other auxiliary code) to live inside the same file as the code it tests, via named submodules.

### `module+` — accumulating submodule

`module+` adds forms to a named submodule that is collected across multiple occurrences in the same file. Classic use: inline tests.

```scheme
(module geom/vector turmeric/base
  (require turmeric/math)
  (provide distance)

  (defn distance [a : vec2, b : vec2] : float
    (sqrt (+ (sq (- b.x a.x)) (sq (- b.y a.y)))))

  (module+ test
    (require turmeric/test)
    (deftest "distance is non-negative"
      (assert (>= (distance (vec2 0 0) (vec2 3 4)) 0.0))))

  (defn- sq [x : float] : float (* x x))

  (module+ test
    (deftest "distance(0,0 → 3,4) = 5"
      (assert (== (distance (vec2 0 0) (vec2 3 4)) 5.0)))))
```

The two `module+ test` blocks are merged. The `test` submodule is only compiled and linked when running tests (controlled by a compiler flag or build target).

### `module*` — nested instantiated submodule

`module*` creates a submodule that is instantiated as part of the enclosing module (unlike `module+` which is collected lazily). The `#f` language means "inherit the enclosing module's language."

```scheme
(module* helpers #f
  (provide clamp)
  (defn clamp [v lo hi] (max lo (min hi v))))
```

Importers can require it as `(require (submod "geom/vector" helpers))`.

### Tasks

#### Parser (`src/reader.c`)
- [ ] Recognize `module+` special form: `(module+ name body...)`
- [ ] Recognize `module*` special form: `(module* name lang body...)` where `lang` may be `#f`
- [ ] Allow multiple `module+` blocks with the same name in one file
- [ ] `module+` and `module*` only valid inside a `module` form body

#### Submodule collection (`src/module.c`)
- [ ] During file parsing, accumulate all `module+` bodies per name into a single `SubModule` node
- [ ] `module*` compiled as a standalone module linked to the parent
- [ ] Map submodule names to parent module: `geom/vector::test`, `geom/vector::helpers`

#### `submod` require path (`src/module.c`)
- [ ] Support `(require (submod "path/to/file" subname))` to import a specific submodule
- [ ] Support `(require (submod "." subname))` — relative to current file
- [ ] Submodule exports are independent of parent module exports

#### Build system integration (`src/main.c`)
- [ ] `tur build` — skip `test` submodules
- [ ] `tur test` — compile and run `test` submodules
- [ ] `tur test path/to/file.tur` — run tests only in a specific file

#### `module+` vs external test files
Test co-location trade-offs:

| Approach | Pro | Con |
|---|---|---|
| `module+ test` | Tests live next to code; can access private definitions | File gets longer |
| External test file | Separation of concerns | Tests can only access public API |

Both are supported. Recommend `module+ test` for unit tests of internals, external test files for integration tests.

---

## Phase R5 — Separate Compilation

**Goal:** Emit one `.c` + `.h` pair per module with proper cross-module linking. (Largely mirrors Phase M3 in the Clojure plan, with adjustments for the multi-module-per-file and phased compilation models.)

### Tasks

#### One compilation unit per `module` form
- [ ] Each `module` form compiles to `<mangled-name>.c` + `<mangled-name>.h`
- [ ] Multiple `module` forms in one file produce multiple `.c`/`.h` pairs
- [ ] Submodules produce separate `.c`/`.h` pairs with name `parent__subname`

#### Phase-0 vs phase-1 headers
- [ ] Phase-0 `.h`: run-time declarations (functions, types, constants)
- [ ] Phase-1 `.h` (new): compile-time declarations for macros/syntax helpers
- [ ] Phase-1 `.h` only included during compilation of dependents, not final link

#### C symbol mangling
- [ ] Module `/` → `__`; `-` → `_`; `::` (submodule) → `___`
- [ ] Examples: `geom/vector` → `geom__vector`; `geom/vector::test` → `geom__vector___test`
- [ ] Exported symbols: `<module_mangled>__<symbol>`
- [ ] Protected symbols: `static` in `.c`, omitted from `.h`
- [ ] Collision detection: error if two modules mangle to the same C prefix

#### Include guards and `#include` chains
- [ ] Header include guard: `#ifndef TUR_MODULE_<MANGLED>_H`
- [ ] Header `#include`s phase-0 headers of each `(require m)` module

#### Incremental compilation
- [ ] Content-hash based: recompile only if source hash or any dependency hash changes
- [ ] Dependency graph stored in a lock file (`.tur-deps.json` or similar)

---

## Phase R6 — Language Protocol (`#%module-begin`)

**Goal:** Allow any module to be used as a **language** by providing a `#%module-begin` macro that wraps the module body.

### Motivation

Turmeric's standard library could ship domain-specific languages:

```scheme
;; A testing DSL
(module my-tests turmeric/testing
  (deftest "addition"
    (assert (== (+ 1 2) 3))))
```

`turmeric/testing` provides a `#%module-begin` that automatically wraps the body in test-runner scaffolding, imports assertion helpers, and registers tests with the test harness — without the user needing to `require` anything.

### Tasks

#### `#%module-begin` protocol (`src/elab.c`)
- [ ] After loading the language module, look up its exported `#%module-begin` macro
- [ ] If found, wrap the module body: `(#%module-begin form1 form2 ...)`
- [ ] If not found, use the default module-begin that simply evaluates forms in order
- [ ] `#%module-begin` receives the entire module body as a list of unevaluated forms

#### Default `#%module-begin` (`stdlib/base.tur`)
- [ ] Implement default `#%module-begin` that processes `provide` and `require` forms, then evaluates remaining forms in order
- [ ] Export `#%module-begin` from `turmeric/base`

#### DSL language modules (`stdlib/`)
- [ ] `turmeric/testing` — testing DSL language: auto-import `test.tur`, wrap tests in runner
- [ ] `turmeric/script` — scripting language: auto-import `io.tur`, `str.tur`; no explicit `provide` required

#### Language tower
- [ ] A language module may itself use a language: `(module turmeric/testing turmeric/base ...)`
- [ ] Cycle detection: error if language chain is circular

---

## Phase R7 — Units and Signatures (v2)

**Goal:** First-class linkable components (Racket *units*) for dependency injection and parameterized libraries.

### Motivation

Units allow a module to be instantiated with *pluggable dependencies* at link time, rather than at module load time. This is useful for:
- Mocking I/O in tests
- Shipping multiple implementations of an interface (e.g., a memory allocator) and choosing at link time
- Breaking circular dependencies between modules that logically depend on each other

### Core concepts

```scheme
;; Define a signature (an interface type)
(define-signature allocator^
  (alloc : (-> usize ptr))
  (free  : (-> ptr void)))

;; Define a unit that imports an allocator and exports a pool
(define-unit pool@
  (import allocator^)
  (export pool^)

  (defn make-pool [size] ...)
  (defn pool-alloc [p] ...)
  (provide (all-defined-out)))

;; Instantiate by linking with a concrete allocator unit
(define-unit system-pool@
  (import)
  (export pool^)
  (link pool@ [allocator^ <- system-allocator@]))

;; Invoke (run) a unit
(invoke-unit system-pool@)
```

### Tasks

#### Signatures (`src/module.h`)
- [ ] Define `Signature` struct: name, list of `(symbol, type)` pairs
- [ ] `define-signature` form: parses and registers a signature
- [ ] Signatures are structural (duck-typed): any unit that provides all required symbols satisfies the signature

#### Units (`src/module.h`)
- [ ] Define `Unit` struct:
  - `import_sigs` — list of required signatures
  - `export_sigs` — list of provided signatures
  - `body` — module body (not yet instantiated)
- [ ] `define-unit` form: captures body as uninstantiated template
- [ ] Units are values — can be passed to functions, stored in variables (v2 only)

#### Linking (`src/module.c`)
- [ ] `link` form: compose units by connecting export signatures to import signatures
- [ ] Type-check: verify that linked unit's export signature satisfies the import signature
- [ ] Detect unsatisfied imports after linking

#### Invocation (`src/module.c`)
- [ ] `invoke-unit`: instantiate a unit with no remaining imports; execute its body
- [ ] Instantiation creates a fresh environment; units can be instantiated multiple times

#### Separate compilation of units
- [ ] Units compile to object files with unresolved external symbols for imports
- [ ] Linker resolves imports at link time using the `link` form's mapping

---

## Phase R8 — Integration and Polish

Same scope as Phase M7 in the Clojure plan. Key additions:

### Stdlib migration

- [ ] Wrap each `stdlib/*.tur` file in a `module` form using `turmeric/base`
- [ ] Replace all implicit exports with explicit `provide` forms
- [ ] Add `(provide (all-defined-out))` as default for stdlib files during migration
- [ ] Create `stdlib/prelude.tur` as a convenience facade:
  ```scheme
  (module turmeric/prelude turmeric/base
    (require (all-from-out turmeric/option))
    (require (all-from-out turmeric/result))
    (require (all-from-out turmeric/str))
    (provide (all-from-out turmeric/option)
             (all-from-out turmeric/result)
             (all-from-out turmeric/str)))
  ```

### Testing DSL via `module+ test`

- [ ] Convert all `tests/*.tur` test files to use `module+ test` submodules where appropriate
- [ ] `tur test` discovers and runs all `module+ test` submodules
- [ ] `tur test --filter "geom/*"` runs tests only in matching modules

### Benchmarks

- [ ] Measure compilation time with 50, 100, 500 modules
- [ ] Measure symbol resolution overhead per module lookup
- [ ] Measure impact of phased compilation on incremental rebuild time

### Error messages

- [ ] "module not found" suggests closest match by edit distance
- [ ] "symbol not exported" shows what *is* exported from the module
- [ ] "ambiguous import" names both modules and suggests `rename-in` or `only-in`
- [ ] "phase mismatch" explains phase-0 vs phase-1 in plain language

---

## Data Structures Reference

```c
// module.h

typedef enum {
    PHASE_RUN      = 0,   // run time
    PHASE_SYNTAX   = 1,   // compile time (for-syntax)
    PHASE_TEMPLATE = -1,  // for-template (v2)
    PHASE_LABEL    = 2,   // for-label (v2, analysis only)
} Phase;

typedef struct {
    Symbol  internal_name;   // name inside the module
    Symbol  exported_name;   // name visible to importers (may differ via rename-out)
    bool    is_protected;    // cannot be re-exported via all-from-out
    Phase   phase;           // which phase this export lives in
    Span    span;
} ExportEntry;

typedef struct ModuleEnv ModuleEnv;
struct ModuleEnv {
    Map       bindings[2];   // [0] = phase-0, [1] = phase-1
    ModuleEnv *parent;       // for lexically nested modules
    Symbol    module_name;
    Map       exports;       // Symbol → ExportEntry
    Map       imports;       // alias Symbol → Module *
    Vector    submodules;    // vector of Module *
};

typedef struct {
    Symbol   name;
    Symbol   lang;           // language module name
    Vector   body;           // vector of Expr *
    ModuleEnv *env;          // filled in during elaboration
    Span     span;
    bool     processed;
} ExprModule;

typedef struct {
    Symbol module_name;      // module to import
    RequireSpec *spec;       // combinator tree
    Phase   phase;
    Span    span;
} ExprRequire;

// Recursive require-spec combinator tree
typedef struct RequireSpec RequireSpec;
struct RequireSpec {
    enum {
        REQ_MODULE, REQ_ONLY_IN, REQ_EXCEPT_IN,
        REQ_PREFIX_IN, REQ_RENAME_IN, REQ_COMBINE_IN,
    } kind;
    union {
        Symbol module;                      // REQ_MODULE
        struct { RequireSpec *inner; Vector names; };          // REQ_ONLY_IN, REQ_EXCEPT_IN
        struct { Symbol prefix; RequireSpec *inner; };         // REQ_PREFIX_IN
        struct { RequireSpec *inner; Vector renames; };        // REQ_RENAME_IN
        Vector children;                    // REQ_COMBINE_IN
    };
};

typedef struct {
    enum {
        PROV_NAME, PROV_ALL_DEFINED, PROV_RENAME_OUT,
        PROV_PROTECT_OUT, PROV_EXCEPT_OUT, PROV_ALL_FROM_OUT,
    } kind;
    union {
        Symbol  name;                       // PROV_NAME
        struct { ProvideSpec *inner; Vector excludes; };       // PROV_EXCEPT_OUT
        Vector  items;                      // PROV_RENAME_OUT, PROV_PROTECT_OUT
        Symbol  from_module;                // PROV_ALL_FROM_OUT
    };
    Span span;
} ProvideSpec;
```

---

## Comparison: Clojure-style vs Racket-style

| Feature | Clojure-style ([module-system-plan.md](module-system-plan.md)) | Racket-style (this plan) |
|---|---|---|
| Core form | `defmodule` | `module` |
| Language param | No | Yes — enables DSLs |
| File-module match | Enforced (error) | Encouraged (warning only) |
| Modules per file | Exactly one | Zero or more |
| Export syntax | `(export foo bar)` inside `defmodule` | `(provide foo bar)` anywhere in body |
| Bulk export | Via `export-from` macro | `(provide (all-defined-out))` built-in |
| Import syntax | `(import m :as alias :refer [x y])` | `(require (rename-in (only-in m x y) ...))` |
| Import combinators | `:as`, `:refer` | `only-in`, `except-in`, `prefix-in`, `rename-in`, `combine-in` |
| Macro phases | Serialize + re-eval | Explicit `for-syntax` phase |
| Inline tests | Separate test files | `module+ test` submodule |
| DSL support | None | `#%module-begin` protocol |
| Parametric modules | None | Units + signatures (v2) |
| Re-export | `export-from` macro (workaround) | `(provide (all-from-out m))` built-in |
| Migration path | `tur --modules` flag | Same flag; anonymous module = old behavior |

---

## Open Questions

1. **`module` vs `defmodule` naming:** Should the Racket-style plan use `module` (like Racket) or `defmodule` (as used in Erlang/Elixir, and the current plan)? Keeping `defmodule` avoids a Racket-specific association and may be more familiar.

2. **Language param for v1:** Is the language parameter worth implementing in v1, or should it be a no-op that only accepts `turmeric/base`? Lean: accept any symbol but only act on it in Phase R6.

3. **`provide` placement:** Should `provide` be restricted to the top of a module body (like `export` in the Clojure plan) for readability, or allowed anywhere for co-location? Lean: anywhere, but `tur fmt` reorders them to the top.

4. **Submodule naming collision:** If two separate files each contain `(module+ test ...)`, how does the test runner distinguish them? Answer: by the parent module name, which is scoped to the file.

5. **Units in v1:** Are units worth including in the initial plan? Lean: describe the design but defer to v2.

6. **`#%module-begin` complexity:** The language protocol adds significant complexity for v1. Lean: implement as a stub in v1 (always use default module-begin) and flesh out in v2.

7. **Mixed files:** If a file has both a `module` form and top-level forms outside it, what happens to the top-level forms? Lean: error — once a file contains a `module` form, all code must be inside a `module` form.

---

## Migration Path from Flat Namespace

Same as the Clojure plan. The Racket-style plan adds:

- **Step 0 (zero changes):** Files with no `module` form are treated as anonymous modules — identical behavior to today.
- **Step 1:** Wrap each file in `(module file/path turmeric/base ...)` and add `(provide (all-defined-out))` — no semantic change.
- **Step 2:** Narrow `provide` to an explicit list.
- **Step 3:** Add `require` statements with combinators as needed.
- **Step 4 (optional):** Move tests to `module+ test` blocks.

---

## Dependencies on Other Systems

| Feature | Dependency | Status |
|---|---|---|
| `module` parsing | Reader (Phase 1) | Complete |
| Language parameter | Module loader | New (Phase R0) |
| `provide`/`require` | Symbol table (Phase M1 analog) | New (Phase R1/R2) |
| `for-syntax` | Phase-aware elaborator | New (Phase R3) |
| `module+` submodules | Reader + module collection | New (Phase R4) |
| Separate compilation | Codegen (Phase M3 analog) | New (Phase R5) |
| `#%module-begin` | Macro expander | New (Phase R6) |
| Units | Type system + linker | New (Phase R7) |
