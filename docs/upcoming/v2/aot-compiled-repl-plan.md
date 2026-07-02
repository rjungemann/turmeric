# Plan: AOT-Compiled REPL Substrate for `tur`

> **Status:** Proposed
> **Last Updated:** 2026-06-28
> **Type:** Compiler / CLI / Runtime

## Goal

Design and implement an AOT-compiled REPL substrate for the Turmeric compiler (`tur`). The goal is to enable a fully native, compiled REPL session from the CLI via `tur repl --compiled`, giving users real-time parity with the compiled path (inline-C, optimizations, target memory layout, carrier-crossing ABI semantics).

---

## Technical Overview

Today's `tur repl` is powered by the tree-walking interpreter (`turi`), evaluating AST nodes step-by-step. While highly interactive, it lacks real-time parity with the compiled path (e.g., inline-C expressions, optimizations, exact target memory layout, and `TUR_M7_HKT=0` carrier-crossing ABI semantics).

The **AOT-compiled REPL** bridges this gap by compiling user input on the fly. Each expression or definition entered by the user is transpiled to C, built as a position-independent shared library (`.so` / `.dylib`) via a background C compiler subprocess (`clang` or `gcc`), dynamically loaded into the REPL process via `dlopen`, resolved via `dlsym`, and executed directly as native machine code.

```
       User Form (e.g. (+ x 42))
                   |
                   v
    [ AST Form parsed in REPL Loop ]
                   |
                   v
 [ Append to Session Source / Wrap Stub ]
                   |
                   v  (tur build --shared)
    [ Transient C File & libeval-gen.so ]
                   |
                   v  (dlopen + dlsym)
    [ Invoke compiled __tur_repl_eval ]
                   |
                   v
         Native Output / Result
```

---

## Architectural Design

### 1. The Growing Session File Paradigm

To ensure full type-checking, macro expansion, and lexical scope preservation across multiple evaluations without building a highly complex incremental compiler from scratch, we employ a **Growing Session File** paradigm:

1. **Session File**: On REPL startup, the compiler initializes a transient session file `/tmp/tur-repl-session-<pid>.tur` with a default module declaration (`module turmeric/repl-session`).
2. **Top-Level Definitions**: When the user enters a top-level definition (`def`, `defn`, `defstruct`, `defmodule`, `defclass`, `instance`):
   - The form is appended to `/tmp/tur-repl-session-<pid>.tur`.
   - The compiler runs standard typechecking over the entire updated file.
   - If typechecking fails, the diagnostic is printed, and the append is rolled back.
   - If typechecking succeeds, the file is compiled to C, built as `libeval-<pid>-<gen>.so`, and loaded into the process.
3. **Interactive Expressions**: When the user enters an expression to evaluate (e.g., `(+ x 5)` or `(println "hello")`):
   - The expression is wrapped in a transient, zero-argument evaluation stub:
     ```turmeric
     (defn __tur_repl_eval_stub []
       <expression>)
     ```
   - The stub is appended to a temporary copy of the session file.
   - The compiler typechecks, compiles, and links the entire session + stub into `libeval-<pid>-<gen>.so`.
   - The REPL `dlopens` this library, resolves `__tur_repl_eval_stub` (using mangled conventions), executes it, and prints the return value.

This paradigm ensures 100% feature-completeness with zero code duplication: GADTs, effects, typeclasses, and inline-C work instantly in the REPL because they route through the exact same compiler backend as standalone binaries.

### 2. Global State & Side-Effect Preservation

In a standard compiled shared library, static or global variables (e.g., defined via `(def x (ref-new 0))`) are re-allocated and re-initialized every time a new library is loaded, which would reset user state on every evaluation.

To preserve mutable state across `dlopen` reloads, we introduce a **Global State Registry**:

- The REPL host process maintains a persistent heap-allocated key-value store mapping symbol names to their addresses/values.
- When `(def <name> <expr>)` is compiled in REPL mode, the code generator translates the initialization to a conditional check:
  ```c
  static void *__tur_repl_val_x = NULL;
  void __tur_repl_init_x() {
      __tur_repl_val_x = tur_repl_registry_lookup("x");
      if (!__tur_repl_val_x) {
          // Evaluate initial expression
          __tur_repl_val_x = <compiled_expr_codegen>;
          tur_repl_registry_register("x", __tur_repl_val_x);
      }
  }
  ```
- Subsequent evaluations referencing `x` access it via the resolved pointer in the registry, ensuring reference and value stability across compiler generation runs.

### 3. Generation Control & Memory Hygiene

To prevent symbol collisions and memory leaks:
- Every compilation increment increments a generation counter (`gen`).
- Shared libraries are compiled with a unique filename: `libeval-<pid>-<gen>.so`.
- Before loading `libeval-<pid>-<gen>.so`, the REPL process unloads the previous generation's `.so` using `dlclose` (unless symbols from it are still transitively referenced or registered on the active registry heap, in which case we rely on the operating system's reference counting to clean up unreferenced code segments).
- We use `RTLD_GLOBAL` on platforms that support it, or compile with `-undefined dynamic_lookup` (on macOS) to allow newly loaded libraries to resolve symbols from the REPL host process.

---

## User Experience (CLI Surface)

The feature adds a `--compiled` flag to `tur repl`:

```bash
tur repl --compiled
```

### Prompt Interaction

```
$ tur repl --compiled
Turmeric v0.14.0 (AOT Compiled REPL)
Type :help for help, :quit to exit
[clang-15 compiler backend initialized]

turmeric> (def r (ref-new 42))
=> def r
[Compiled & loaded generation 1 in 48ms]

turmeric> (defn add-r [x] (+ x (ref-get r)))
=> defn add-r
[Compiled & loaded generation 2 in 52ms]

turmeric> (add-r 10)
=> 52
[Evaluated in 12ms]
```

---

## Implementation Phases

### Phase S0 -- Session File & Command Plumbing

- Add `bool compiled_mode` flag to `turi_repl_run` in `src/turi/repl.c`.
- Expose the `--compiled` CLI flag in `src/main.c` under the `repl` subcommand.
- Implement the Session File Manager in `src/turi/repl.c`:
  - Create and manage the temporary `/tmp/tur-repl-session-<pid>.tur` file.
  - Implement clean-up hooks on process exit (`SIGINT`, `SIGTERM`, `:quit`) to delete temporary artifacts.

### Phase S1 -- Transient Compilation Pipeline

- Implement the transient compilation bridge in `src/turi/repl.c`:
  - When an expression is parsed, generate a transient copy of the session file containing the wrapped evaluation stub `__tur_repl_eval_stub`.
  - Invoke `tur build --shared` as an in-process compiler invocation (or shell out to `build/tur` using `system()` / `fork()`) to produce `libeval-<pid>-<gen>.so` under `.tur-repl-cache/`.
  - Compile single-expression modules with `-O0 -g0` to minimize C compiler backend latency.
- Measure and print compile-and-load latency when `TUR_DEBUG=1` is set.

### Phase S2 -- Dynamic Loading and Execution

- Integrate `dlopen` and `dlsym` calls to load each compiled library generation.
- Resolve the evaluation stub's mangled symbol name (e.g. `turmeric__repl_session____tur_repl_eval_stub`).
- Call the resolved function pointer, marshal its return value (using the `TuriValue` marshalers from `src/turi/ffi_thunk.c`), and print it.
- Safely call `dlclose` on the previous generation to reclaim memory.

### Phase S3 -- State Registry Integration

- Implement the Global State Registry (`tur_repl_registry_*`) in `src/runtime/globals.c`.
- Modify C code emission (`src/compiler/emit_stmt.c` / `emit_module.c`) when compilation is triggered in "REPL compilation mode":
  - Emitted C global variables must resolve their initializers against the registry.
  - Expose registry functions as builtins or runtime preamble functions.

---

## Success Criteria

- Running `tur repl --compiled` starts up a fully working REPL session.
- Evaluating pure expressions (e.g., `(+ 2 3)`) typechecks, compiles to C, builds a `.so`, dynamically loads it, and returns `5`.
- Defining a function (e.g., `(defn f [x] (* x 2))`) compiles successfully, and subsequent evaluations of `(f 10)` return `20` using native, compiled execution.
- Inline-C blocks (e.g., `` (`c "printf(\"Hello from C\\n\")" `) ``) compile and run perfectly inside the REPL.

---

## Risks & Mitigations

### 1. Compilation Latency
*Risk*: Shelling out to `clang` or `gcc` on every entered REPL line might introduce a noticeable delay (~200-500ms), making the REPL feel laggy compared to the interpreter.
*Mitigation*: 
- Use compiler optimization flags `-O0` and disable debug symbol generation (`-g0`) for transient evaluations to speed up C compiler frontend processing.
- Cache pre-compiled headers of the Turmeric runtime environment (`tur_runtime.h`) so the C compiler doesn't have to re-parse standard headers on every line.
- In practice, standard compilations of ~50-line files with `clang -O0` take less than 60ms on modern macOS/Linux SSD environments, which is below the human perception threshold for interactive typing.

### 2. Shared Library Proliferation
*Risk*: Long-running REPL sessions will generate hundreds of `libeval-<pid>-<gen>.so` files, cluttering `/tmp` or `.tur-repl-cache/` and consuming system file descriptors.
*Mitigation*:
- Ensure that each generation's library is cleaned up on disk or unlinked immediately after `dlopen` finishes loading it (on Unix-like filesystems, a file can be safely unlinked from disk while its open file descriptor or dynamic load reference is active).
- Clean the entire local `.tur-repl-cache/` on REPL startup and shutdown.
