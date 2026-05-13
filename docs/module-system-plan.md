# Module System — Racket-Style Alternative (Phases R0–R8)

**Status:** Planned (v1 or v2). Alternative design to the existing Clojure-style module plan. Prerequisites: Phase 15 (Typeclasses — for module-scoped instances), Phase 19 (Algebraic effects — modules can declare effect rows on exports). See [module-system-racket-alt-plan.md](module-system-racket-alt-plan.md) for full executive summary and design rationale.

**When to choose this design over Clojure-style:**
- **Test co-location:** `module+ test` inline blocks vs. separate test files.
- **DSL authoring:** Language parameter enables `#%module-begin` hook for custom module semantics.
- **Macro phases:** Explicit `for-syntax` import for clean compile-time/run-time separation.
- **Multiple modules per file:** Useful for submodules and library organization.
- **Re-export combinators:** `all-from-out` and `protect-out` are built-in, not macro workarounds.

**Decision rule:** Adopt Racket-style plan if the team prioritizes DSL support and explicit phase separation; Clojure-style if simplicity and Java-like naming conventions are preferred.

| Phase | Goal | Exit Criterion |
|---|---|---|
| **R0** | `module` form + language parameter | `module`, `provide`, `require` parse; language parameter validates; reader reserves phase keywords |
| **R1** | `provide` transformer system | `all-defined-out`, `rename-out`, `protect-out`, `except-out`, `all-from-out` work correctly |
| **R2** | `require` combinator system | `only-in`, `except-in`, `prefix-in`, `rename-in` combinators; circular import detection works |
| **R3** | Phased imports | `for-syntax` phase separation; phase-0 and phase-1 environments isolated; v2: `for-template`, `for-label` |
| **R4** | Submodules (`module+` / `module*`) | Inline test submodules; per-file multi-module support; `submod` require paths work |
| **R5** | Separate compilation | One `.c`+`.h` per module; cross-module linking; C symbol mangling; incremental build support |
| **R6** | Language protocol (`#%module-begin`) | Any module usable as a language; stdlib DSL modules work; language tower with cycle detection |
| **R7** | Units and signatures (v2) | `define-signature`, `define-unit`, `invoke-unit`, `link` forms; dependency injection patterns work |
| **R8** | Integration & polish | Stdlib migration to modules; documentation; tests; benchmarks; DSL examples in stdlib |

---

### Phase R0 — `module` Form and Language Parameter

**Goal:** Parse and validate the `module` form, language parameter, and bare `provide`/`require` forms. Establish the core reader infrastructure.

**Reader extensions** — `src/reader.{c,h}`
- [ ] Recognize `module` as a special form (distinct from `defmodule` if it exists from earlier planning).
- [ ] Recognize `provide` as a special form.
- [ ] Recognize `require` as a special form.
- [ ] Parse `(module name lang body...)` — `name` is a symbol, `lang` is a symbol (module path).
- [ ] Support `module` at top level of a file and allow nesting inside another `module`.
- [ ] Allow `provide` and `require` anywhere in module body (not just top).
- [ ] Reserve phase keywords: `for-syntax`, `for-template`, `for-label`.
- [ ] Error on invalid qualified symbols in module names: e.g., reject names with invalid characters.

**AST nodes** — `src/expr.h` or new `src/module.h`
- [ ] Define `EXPR_MODULE` node:
  - `name` — `Symbol *` (module name)
  - `lang` — `Symbol *` (language module name)
  - `body` — `Vec<Expr *>` (module body forms)
  - `span` — `Span` (source location)
- [ ] Define `EXPR_PROVIDE` node:
  - `specs` — `Vec<ProvideSpec *>` (export specifications)
  - `span` — `Span`
- [ ] Define `EXPR_REQUIRE` node:
  - `specs` — `Vec<RequireSpec *>` (import specifications)
  - `phase` — `enum { PHASE_RUN, PHASE_SYNTAX, PHASE_TEMPLATE, PHASE_LABEL }`
  - `span` — `Span`

**Validation** — `src/reader.{c,h}` + `src/elab.{c,h}`
- [ ] Module name must be a valid qualified symbol: letters, digits, `/`, `-`, `_`.
- [ ] Language parameter must be a valid symbol.
- [ ] Error if top-level `module` form is not at file scope (no nested `module` inside regular `defn`/`let`).
- [ ] Warning if explicit module name does not match file path (informational; does not prevent loading).
- [ ] Collect all `provide` and `require` forms during parsing for later processing.

**Language parameter resolution** — `src/module.c` (new file)
- [ ] Look up language module in the module search path (resolved from Turmeric home + user paths).
- [ ] Load language module's exports and `#%module-begin` form if present (Phase R6 feature; v1: error if not found).
- [ ] Error if language module not found; list search paths in error message.
- [ ] `turmeric/base` is always available (compiler built-in; provides default bindings and `#%module-begin`).
- [ ] Store language module reference in `ModuleEnv` for use by elaborator.

**Type system** — `src/types.{c,h}` + `src/elab.{c,h}`
- [ ] Add `ModuleEnv` struct to hold module-local symbol table:
  - `name` — module name
  - `lang` — language module name
  - `provides` — export table (built in Phase R1)
  - `phase_env[2]` — phase-0 (run-time) and phase-1 (compile-time) symbol tables
  - `search_path` — module search path

**Fixtures** — `tests/fixtures/modules/`
- [ ] `module-basic.tur` — parse a simple `module` form with name and language.
- [ ] `module-provide-require-parse.tur` — parser accepts `provide` and `require` anywhere in body.
- [ ] `module-language-builtin.tur` — `turmeric/base` language always available.
- [ ] Negative: `module-bad-name.tur` — invalid characters in module name error.
- [ ] Negative: `module-language-not-found.tur` — missing language error lists search paths.

**Exit criterion:** `module`, `provide`, `require` parse correctly; language parameter resolves; `turmeric/base` is available by default; validation catches invalid module names.

---

### Phase R1 — `provide` Transformer System

**Goal:** Implement all `provide` combinator forms to control module exports.

**Provide combinator forms**
```
(provide name ...)                         ; explicit list
(provide (all-defined-out))                ; all definitions in this module
(provide (rename-out [old new] ...))       ; export with rename
(provide (protect-out name ...))           ; exportable but not re-exportable
(provide (except-out spec name ...))       ; bulk export minus exclusions
(provide (all-from-out module ...))        ; re-export everything from another module
```

**AST for provide specs** — `src/module.h`
- [ ] Define `ProvideSpec` tagged union:
  - `PROV_NAME` — single symbol export
  - `PROV_ALL_DEFINED` — no fields (all local definitions)
  - `PROV_RENAME_OUT` — `Vec<(Symbol old, Symbol new)>` pairs
  - `PROV_PROTECT_OUT` — `Vec<Symbol>` protected exports
  - `PROV_EXCEPT_OUT` — inner `ProvideSpec *` + `Vec<Symbol>` excluded names
  - `PROV_ALL_FROM_OUT` — `Vec<Symbol>` module names to re-export from
- [ ] Each `ProvideSpec` carries a `Span` for error reporting.

**Export table processing** — `src/module.c`
- [ ] Collect all `provide` forms in a module after elaboration (forms can appear anywhere in body).
- [ ] Build export table: `Map<Symbol, ExportEntry>` where `ExportEntry` contains:
  - `internal_name` — name as defined locally
  - `exported_name` — name visible to importers (may differ via `rename-out`)
  - `is_protected` — flag: cannot be re-exported via `all-from-out`
  - `source_span` — for error messages
- [ ] Expand each provide spec against the module's definition table:
  - `all-defined-out`: iterate all definitions in module scope
  - `rename-out`: map old name to new name in export table
  - `protect-out`: mark export as protected (cannot be re-exported)
  - `except-out`: expand inner spec, remove excluded names
  - `all-from-out`: pull all exported (non-protected) names from the named import module
- [ ] Check consistency: error if `provide` refers to undefined name.
- [ ] Warning if `provide` duplicates a name already provided (e.g., two `provide` forms export the same symbol).
- [ ] Warning if `all-defined-out` exports names that look private (convention: prefixed with `%` or `_`).

**Error reporting**
- [ ] Error: `provide` refers to undefined name; list available names in error.
- [ ] Error: `provide` tries to re-export a protected import; list the import source.
- [ ] Error: `all-from-out` names a module that was not imported.
- [ ] Warning: `provide` duplicates a name already provided.
- [ ] Warning: `all-defined-out` would export internal-looking names (suggestion to use `except-out`).

**Fixtures** — `tests/fixtures/modules/`
- [ ] `provide-explicit.tur` — `(provide name1 name2)` exports only those names.
- [ ] `provide-all-defined.tur` — `(provide (all-defined-out))` exports all public definitions.
- [ ] `provide-rename.tur` — `(provide (rename-out [internal external]))` renames on export.
- [ ] `provide-protect.tur` — `(provide (protect-out name))` prevents re-export.
- [ ] `provide-except.tur` — `(provide (except-out (all-defined-out) internal))` excludes specific names.
- [ ] `provide-all-from.tur` — `(provide (all-from-out other-module))` re-exports all.
- [ ] `provide-mixed.tur` — combination of multiple `provide` forms in one module.
- [ ] Negative: `provide-undefined.tur` — `provide` names undefined symbol errors.
- [ ] Negative: `provide-reexport-protected.tur` — re-exporting protected import errors.
- [ ] Negative: `provide-duplicate.tur` — duplicate export name warns.

**Exit criterion:** All `provide` combinator forms work; export table is correctly computed; errors and warnings are actionable; fixtures pass.

---

### Phase R2 — `require` Combinator System

**Goal:** Implement all `require` combinator forms to control module imports.

**Require combinator forms**
```scheme
(require module)                                   ; all provided names
(require (only-in module name ...))                ; whitelist
(require (except-in module name ...))              ; blacklist
(require (prefix-in pfx: module))                  ; add prefix to all names
(require (rename-in module [old new] ...))         ; rename specific names
(require (combine-in spec ...))                    ; compose specs (union)
(require (all-from-out module))                    ; re-export everything imported
```

**AST for require specs** — `src/module.h`
- [ ] Define `RequireSpec` tagged union:
  - `REQ_MODULE` — module symbol (plain import)
  - `REQ_ONLY_IN` — inner `RequireSpec *` + `Vec<Symbol>` names
  - `REQ_EXCEPT_IN` — inner `RequireSpec *` + `Vec<Symbol>` excluded names
  - `REQ_PREFIX_IN` — `Symbol` prefix + inner `RequireSpec *`
  - `REQ_RENAME_IN` — inner `RequireSpec *` + `Vec<(Symbol old, Symbol new)>` pairs
  - `REQ_COMBINE_IN` — `Vec<RequireSpec *>` (union)
  - `REQ_ALL_FROM_OUT` — module symbol (re-export)
- [ ] Each `RequireSpec` carries a `Span`.

**Require processing** — `src/module.c`
- [ ] Resolve `RequireSpec` against target module's export table:
  - `only-in`: intersect imported names with whitelist; error if name not found.
  - `except-in`: subtract blacklist from imported names; warn if excluded name doesn't exist.
  - `prefix-in`: prepend prefix string to all imported names; no name collisions possible.
  - `rename-in`: apply rename mappings; error if old name not found.
  - `combine-in`: union results from child specs; error if two specs introduce the same local name (ambiguity).
  - `all-from-out`: pull all exported (non-protected) names from target module's exports; register as re-export.
- [ ] Register resulting bindings in current module's local scope.
- [ ] Track origin module + original name for each binding (for error messages, re-export tracking, and cross-module debugging).
- [ ] Populate the appropriate phase environment (phase-0 for `require`, phase-1 for `require (for-syntax ...)` — Phase R3).

**Circular import detection** — `src/module.c`
- [ ] Build directed dependency graph during require resolution: nodes = modules, edges = import relationships.
- [ ] After loading all top-level modules, topological sort; report cycle if found.
- [ ] Cycle error message lists full import chain: "Module A imports B, B imports C, C imports A".

**Error reporting**
- [ ] Error: module not found; list search paths.
- [ ] Error: `only-in` names a symbol not exported by target module; list available exports.
- [ ] Error: `rename-in` old name not exported by target module.
- [ ] Error: `combine-in` specs introduce the same local name (ambiguity); list conflicting specs.
- [ ] Error: circular import detected; list cycle chain.
- [ ] Warning: `except-in` excludes a name that does not exist in the module.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `require-plain.tur` — `(require module)` imports all exported names.
- [ ] `require-only-in.tur` — `(require (only-in module name1 name2))` imports subset.
- [ ] `require-except-in.tur` — `(require (except-in module name))` imports all but excluded.
- [ ] `require-prefix-in.tur` — `(require (prefix-in pfx: module))` adds prefix to all.
- [ ] `require-rename-in.tur` — `(require (rename-in module [old new]))` renames on import.
- [ ] `require-all-from-out.tur` — `(require (all-from-out module))` re-exports.
- [ ] `require-combine.tur` — `(require (combine-in spec1 spec2))` combines specs.
- [ ] `require-nested-combinators.tur` — nested combinators work correctly.
- [ ] Negative: `require-module-not-found.tur` — missing module errors.
- [ ] Negative: `require-only-in-bad-name.tur` — `only-in` with non-existent name errors.
- [ ] Negative: `require-circular-import.tur` — A imports B, B imports A detected.
- [ ] Negative: `require-combine-ambiguity.tur` — `combine-in` specs with same name error.

**Exit criterion:** All `require` combinator forms work; dependency graph is computed; circular imports are detected; all errors and warnings are actionable.

---

### Phase R3 — Phased Imports (`for-syntax`, `for-template`)

**Goal:** Separate compile-time and run-time dependency graphs using `for-syntax` and friends. Enable explicit phase separation for macros.

**Phase model**
- Every module has a **phase-0 environment** (run-time bindings) and a **phase-1 environment** (compile-time / macro-expansion bindings).
- Importing a module at phase 1 makes that module's phase-0 exports available as phase-1 bindings in the importer.
- v1 scope: only phases 0 and 1; `for-template` and `for-label` are v2 stretch goals.

**Phase-annotated symbol tables** — `src/module.h`
- [ ] Extend `ModuleEnv` to hold separate symbol tables per phase:
  - `phase_env[0]` — run-time bindings
  - `phase_env[1]` — compile-time bindings (macros and their dependencies)
- [ ] Add `current_phase` field to elaborator context to track which phase is active.

**`for-syntax` require processing** — `src/module.c` + `src/elab.{c,h}`
- [ ] When `(require (for-syntax m))` is encountered, load `m` and install its phase-0 exports into importer's phase-1 environment.
- [ ] Macros defined in this module may call functions from phase-1 env during `defmacro` body expansion.
- [ ] Run-time code may not refer to phase-1 bindings; elaborate error: reference to phase-1 binding at phase-0.

**Elaborator integration** — `src/elab.{c,h}`
- [ ] Query correct phase environment during name resolution based on `current_phase`.
- [ ] Phase-1 context: active during `defmacro` body expansion.
- [ ] Phase-0 context: active during normal expression elaboration.
- [ ] Error: reference to phase-1 binding at phase-0; suggest moving code into a macro or `defmacro` context.

**Separate compilation implications** — `src/main.c`
- [ ] Phase-1 dependencies compiled first (they are needed during elaboration of the importer).
- [ ] Phase-0 dependencies compiled concurrently or lazily.
- [ ] `.h` files include only phase-0 declarations; phase-1 symbols are internal to the compiler.

**v1 scope** — `src/elab.{c,h}`
- [ ] Implement `for-syntax` and `for-template` keyword parsing (reader already reserves them in Phase R0).
- [ ] `for-syntax` working; `for-template` and `for-label` treat imports as compile-time-only (no execute-at-compile behavior).
- [ ] Mark items as to-be-deferred in phase roadmap.

**v2 roadmap**
- [ ] Full phase tower with `for-template` and `for-label`.
- [ ] `for-template`: bindings available when macro-generated code is later expanded.
- [ ] `for-label`: bindings available only for static analysis (not executed at any phase).

**Fixtures** — `tests/fixtures/modules/`
- [ ] `phase-separate-basic.tur` — `(require turmeric/base)` at phase-0, `(require (for-syntax turmeric/syntax-utils))` at phase-1.
- [ ] `phase-separate-macro.tur` — macro uses phase-1 binding; phase-0 code does not see it.
- [ ] `phase-separate-error.tur` — referencing phase-1 binding at phase-0 is a compile error.
- [ ] Negative: `phase-mismatch.tur` — phase-0 code tries to call phase-1 function errors.

**Exit criterion:** `for-syntax` separates run-time and compile-time environments; macros can access phase-1 bindings; phase-0 code errors on phase-1 references; v2 items marked for future.

---

### Phase R4 — Submodules (`module+` and `module*`)

**Goal:** Allow test code and auxiliary code to live inline via named submodules.

**`module+` — accumulating submodule**

Multiple `module+ test` blocks in the same file are merged. Classic use: inline unit tests.

```scheme
(defn distance [a : vec2, b : vec2] : float
  (sqrt (+ (sq (- b.x a.x)) (sq (- b.y a.y)))))

(module+ test
  (deftest "distance is non-negative"
    (assert (>= (distance (vec2 0 0) (vec2 3 4)) 0.0))))

(defn- sq [x : float] : float (* x x))

(module+ test
  (deftest "distance(0,0 → 3,4) = 5"
    (assert (== (distance (vec2 0 0) (vec2 3 4)) 5.0))))
```

**`module*` — nested instantiated submodule**

Creates a submodule that is instantiated as part of the enclosing module.

```scheme
(module* helpers #f
  (provide clamp)
  (defn clamp [v lo hi] (max lo (min hi v))))
```

Importers can `(require (submod "geom/vector" helpers))`.

**Parser** — `src/reader.{c,h}`
- [ ] Recognize `module+` special form: `(module+ name body...)`.
- [ ] Recognize `module*` special form: `(module* name lang body...)` where `lang` may be `#f` (inherit parent language).
- [ ] Allow multiple `module+` blocks with the same name in one file (accumulated).
- [ ] Error if `module+` or `module*` appears outside a `module` form body.

**Submodule collection** — `src/module.c`
- [ ] During file parsing, accumulate all `module+` bodies per name into a single submodule node.
- [ ] `module*` compiled as a standalone module linked to the parent.
- [ ] Map submodule names to parent: `geom/vector::test`, `geom/vector::helpers`.
- [ ] Verify no naming collisions between submodules.

**`submod` require path** — `src/module.c`
- [ ] Support `(require (submod "path/to/file" subname))` to import a specific submodule.
- [ ] Support `(require (submod "." subname))` — relative to current file.
- [ ] Submodule exports are independent of parent module exports.

**Build system integration** — `src/main.c` + `Makefile`
- [ ] `tur build` — skip `test` submodules.
- [ ] `tur test` — compile and run `test` submodules.
- [ ] `tur test path/to/file.tur` — run tests only in a specific file.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `submodule-plus-basic.tur` — `module+ test` block co-located with code.
- [ ] `submodule-plus-multiple.tur` — multiple `module+ test` blocks accumulate.
- [ ] `submodule-star-basic.tur` — `module*` nested instantiated submodule.
- [ ] `submodule-require.tur` — `(require (submod "path" name))` imports submodule.
- [ ] Negative: `submodule-outside-module.tur` — `module+` outside `module` body errors.

**Exit criterion:** `module+` and `module*` parse; `module+` blocks accumulate; `submod` imports work; build system respects test submodules.

---

### Phase R5 — Separate Compilation

**Goal:** Emit one `.c` + `.h` pair per module with proper cross-module linking.

**Tasks**

**One compilation unit per `module` form** — `src/emit.{c,h}` + `src/main.c`
- [ ] Each `module` form compiles to `<mangled-name>.c` + `<mangled-name>.h`.
- [ ] Multiple `module` forms in one file produce multiple `.c`/`.h` pairs.
- [ ] Submodules produce separate `.c`/`.h` pairs with name `parent__subname`.
- [ ] Compilation order respects dependency graph (phase-1 before phase-0).

**Phase-0 vs phase-1 headers** — `src/emit.{c,h}`
- [ ] Phase-0 `.h`: run-time declarations (functions, types, constants, typeclass instances).
- [ ] Phase-1 `.h` (new): compile-time declarations for macros and syntax helpers (not included in final link).
- [ ] Compiler driver only includes phase-0 headers in final link.

**C symbol mangling** — `src/emit.{c,h}`
- [ ] Module `/` → `__`; `-` → `_`; `::` (submodule) → `___`.
- [ ] Examples: `geom/vector` → `geom__vector`; `geom/vector::test` → `geom__vector___test`.
- [ ] Exported symbols: `<module_mangled>__<symbol>`.
- [ ] Protected symbols: `static` in `.c`, omitted from `.h`.
- [ ] Collision detection: error if two modules mangle to the same C prefix.

**Include guards and `#include` chains** — `src/emit.{c,h}`
- [ ] Header include guard: `#ifndef TUR_MODULE_<MANGLED>_H`.
- [ ] Header `#include`s phase-0 headers of each `(require m)` module.
- [ ] Transitive includes: if `A` requires `B` and `B` requires `C`, `A.h` includes both `B.h` and `C.h` (for full type definitions).

**Incremental compilation** — `src/main.c` + `.tur-deps` file format
- [ ] Content-hash based: recompile only if source hash or any dependency hash changes.
- [ ] Dependency graph stored in `.tur-deps` file (JSON or simple text format).
- [ ] Detect changes to headers and re-link dependents.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `separate-compile-basic.tur` — two modules in separate files; `tur build` emits `.c`/`.h` for each.
- [ ] `separate-compile-link.tur` — linking between two modules works correctly.
- [ ] `separate-compile-submodule.tur` — submodule emits separate `.c`/`.h`.
- [ ] Codegen snapshots: symbol mangling, header generation, include chains.

**Exit criterion:** Each module compiles to separate `.c`/`.h`; linking works; incremental compilation detects changes; all snapshots stable.

---

### Phase R6 — Language Protocol (`#%module-begin`)

**Goal:** Allow any module to be used as a **language** by providing a `#%module-begin` macro.

**Motivation**

Turmeric's standard library could ship domain-specific languages without requiring explicit imports:

```scheme
(module my-tests turmeric/testing
  (deftest "addition"
    (assert (== (+ 1 2) 3))))
```

`turmeric/testing` provides a `#%module-begin` that wraps the body, imports assertion helpers, and registers tests with the harness.

**Tasks**

**`#%module-begin` protocol** — `src/elab.{c,h}`
- [ ] After loading the language module, look up its exported `#%module-begin` macro.
- [ ] If found, wrap the module body: `(#%module-begin form1 form2 ...)`.
- [ ] If not found, use the default module-begin that processes `provide`/`require` then evaluates forms.
- [ ] `#%module-begin` receives the entire module body as a list of unevaluated forms.

**Default `#%module-begin`** — `stdlib/base.tur`
- [ ] Implement default `#%module-begin` that processes `provide` and `require` forms, then evaluates remaining forms in order.
- [ ] Export `#%module-begin` from `turmeric/base`.

**DSL language modules** — `stdlib/`
- [ ] `turmeric/testing` — testing DSL: auto-import `test.tur`, register `deftest` forms.
- [ ] `turmeric/script` — scripting language: auto-import `io.tur`, `str.tur`; no explicit `provide` required.

**Language tower** — `src/elab.{c,h}`
- [ ] A language module may itself use a language: `(module turmeric/testing turmeric/base ...)`.
- [ ] Cycle detection: error if language chain is circular; report cycle chain.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `language-basic.tur` — custom language with `#%module-begin` hook.
- [ ] `language-testing.tur` — `turmeric/testing` language with `deftest`.
- [ ] `language-tower.tur` — language chain works correctly.
- [ ] Negative: `language-circular.tur` — circular language chain detected and errors.

**Exit criterion:** Custom languages via `#%module-begin` work; stdlib includes `turmeric/testing`; language tower with cycle detection works.

---

### Phase R7 — Units and Signatures (v2)

**Goal:** First-class linkable components for dependency injection and parameterized libraries.

**Motivation**

Units allow a module to be instantiated with *pluggable dependencies* at link time, enabling mocking, multiple implementations, and breaking circular dependencies.

**Tasks**

**Signatures** — `src/module.h` + `src/elab.{c,h}`
- [ ] Define `Signature` struct: name, list of `(symbol, type)` pairs.
- [ ] `define-signature` form: parses and registers a signature.
- [ ] Signatures are structural (duck-typed): any unit providing all required symbols satisfies the signature.

**Units** — `src/module.h` + `src/elab.{c,h}`
- [ ] Define `Unit` struct:
  - `import_sigs` — list of required signatures
  - `export_sigs` — list of provided signatures
  - `body` — module body (not yet instantiated)
- [ ] `define-unit` form: captures body as uninstantiated template.
- [ ] Units are values — can be passed to functions, stored in variables.

**Linking** — `src/module.c`
- [ ] `link` form: compose units by connecting export signatures to import signatures.
- [ ] Type-check: verify linked unit's export signature satisfies import signature.
- [ ] Detect unsatisfied imports after linking.

**Invocation** — `src/module.c`
- [ ] `invoke-unit`: instantiate a unit with no remaining imports; execute its body.
- [ ] Instantiation creates a fresh environment; units can be instantiated multiple times.

**Fixtures** — `tests/fixtures/modules/`
- [ ] `unit-basic.tur` — define and invoke a unit.
- [ ] `unit-link.tur` — link units with compatible signatures.
- [ ] `unit-signature.tur` — signature checking works.
- [ ] Negative: `unit-unsatisfied-import.tur` — linking without satisfying all imports errors.

**Exit criterion:** Units and signatures work for dependency injection; linking type-checks; invocation executes correctly; v2 feature complete.

---

### Phase R8 — Integration and Polish

**Goal:** Migrate stdlib to modules, stabilize APIs, document, and ship stdlib DSLs.

**Tasks**

**Stdlib migration** — `stdlib/`
- [ ] Wrap each `stdlib/*.tur` file in a `module` form using `turmeric/base`.
- [ ] Replace all implicit exports with explicit `provide` forms.
- [ ] Add `(provide (all-defined-out))` as default for stdlib files during migration.
- [ ] Create `stdlib/prelude.tur` as a convenience facade re-exporting common modules.
- [ ] Update module search paths and build rules to find migrated modules.

**Documentation** — `docs/`
- [ ] Write `module-system-user-guide.md`: overview, examples, best practices.
- [ ] Write `module-phase-separation-guide.md`: when to use `for-syntax`; macro dependency organization.
- [ ] Write `dsl-authoring-guide.md`: how to create a custom language with `#%module-begin`.
- [ ] Update existing docs to reference new module system.

**Testing** — `tests/fixtures/modules/` + `tests/integration/`
- [ ] Comprehensive fixture suite for all phases (already started in earlier phases).
- [ ] Integration tests: full stdlib build, cross-module linking, DSL usage.
- [ ] Negative tests: error messages are actionable and point to root causes.

**Benchmarks** — `tests/benchmarks/modules/`
- [ ] Module loading time: compare cold startup vs. warm cache.
- [ ] Compilation time: separate vs. monolithic compilation.
- [ ] Link time: large stdlib with many modules.

**Fixtures & Examples** — `stdlib/` + `docs/examples/`
- [ ] Example: custom testing DSL with `#%module-begin`.
- [ ] Example: library with submodules and re-exports.
- [ ] Example: phased imports for macro dependencies.
- [ ] Example: units for dependency injection (if v2).

**Exit criterion:** Stdlib fully migrated to modules; documentation is complete and examples work; no regressions in build time or code generation; DSL examples in stdlib.
