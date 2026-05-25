# Plan: Transitive reader-macro coverage through module loading

> **Status:** Draft Plan
> **Last Updated:** 2026-05-25
> **Type:** Compiler / Reader / Elaborator
> **Depends on:** [reader-macros-plan.md](reader-macros-plan.md) (RM0-RM4 complete)

---

## Overview

RM4 wired the spice manifest's `:reader-macros [...]` entry into every
compile entry point, so the entry file always sees the manifest's macros.
But module loading -- the path that runs when an `import` statement
pulls in another `.tur` file from the same spice -- still uses the
no-registry `read_all` and is therefore "blind" to user reader macros.

The result: a spice can register `#sql{...}` in `build.tur`, and the
entry file can use it freely, but a sibling module that *also* uses
`#sql{...}` errors out with "unexpected character '#'" the moment it's
imported.

This plan threads a registry through the elaborator's module-loading
path so reader macros declared at the spice level (and via in-source
`#use-reader-macros` directives) are visible to every file in the
spice's import graph.

---

## Current state

`src/compiler/elab_module.c::187` reads each imported module with the
bare `read_all`:

```c
uint32_t nforms = 0;
Form **forms = read_all(e->arena, e->st, sfile, &nforms);
```

The same pattern appears for `(load "...")` in
`elab_toplevel.c::~489`. Neither has access to the entry file's
reader-macro registry, so user macros silently stop working at the
module boundary.

The entry file's registry already exists -- it's allocated in
`compile_to_c` (and friends) and passed to `read_all_with_registry` for
the top-level read. What's missing is plumbing it through the `Elab`
struct so module loaders can see it.

---

## Non-goals

- **Per-module registries.** All files in a single compilation share
  one registry. A macro defined in one module is visible to every
  module that imports (transitively) into the same compile. This
  matches how the manifest entry already behaves at the spice level
  and avoids reasoning about "macro visibility across module
  boundaries."
- **Macro export annotations.** No `^export-reader-macro` or
  `(reader-macros/define-public ...)` shorthand. Either a macro is
  registered before reading begins (manifest / `#use-reader-macros` /
  inline define) or it isn't.
- **Topological re-reading.** If module B imports A and A defines a
  reader macro, B's source is *not* re-read after A elaborates --
  macros must be available *at the time B is read*, which means they
  have to be registered before the compile starts (or by an earlier
  in-source directive in B itself).
- **REPL-style cross-turn sharing inside a compile.** REPL persistence
  is already handled separately (RM Q#5, `TuriEnv->reader_macros`).
  This plan covers batch compilation only.

---

## Design

### 1. Add `user_macros` to the `Elab` struct

`src/compiler/elab_internal.h::150` already houses the elaborator
context. Add an opaque-pointer field:

```c
typedef struct Elab {
    Arena       *arena;
    SymbolTable *st;
    ...
    /* RM transitive: shared reader-macro registry, set by the driver
     * before elaborate_program runs. Module-loader reads use this so
     * imported files see the same user macros the entry file did.
     * May be NULL (no user macros), in which case loaders fall back
     * to the no-registry read_all. */
    struct ReaderMacroRegistry *user_macros;
} Elab;
```

Forward-declare `ReaderMacroRegistry` in `elab_internal.h` to keep the
header from pulling in `reader_macros.h`.

### 2. Thread the registry from compile to elaborator

The compile functions (`compile_to_c`, `compile_to_h`,
`compile_to_implementation`) already build a per-compile registry and
pass it to `read_all_with_registry` for the entry file. They also call
`elaborate_program` (or `run_core_passes`, which calls it indirectly).

`elaborate_program` constructs the `Elab` struct internally. Add an
optional pointer parameter, or thread it via `PassContext`:

```c
typedef struct PassContext {
    Arena       *arena;
    SymbolTable *st;
    Form       **forms;
    uint32_t     nforms;
    ...
    /* RM transitive */
    struct ReaderMacroRegistry *reader_macros;  /* may be NULL */
} PassContext;
```

`run_core_passes` copies `ctx->reader_macros` into the freshly
allocated `Elab`. Compile-side: add `&reader_macros_reg` to the
`PassContext` literal.

### 3. Switch module loaders to `read_all_with_registry`

Two sites:

**`elab_module.c::~187`** -- the main module-load path used by `import`:

```c
- Form **forms = read_all(e->arena, e->st, sfile, &nforms);
+ Form **forms = read_all_with_registry(e->arena, e->st, sfile,
+                                       e->user_macros, &nforms);
```

**`elab_toplevel.c::~489`** -- the `(load "...")` directive:

Same one-line swap.

Both calls already pass `e->arena` and `e->st`; adding
`e->user_macros` is the only change. Behavior is identical when
`e->user_macros == NULL` (which is what the per-file read_all already
does internally).

### 4. Inline define forms in imported modules

When the loader reads an imported file with a non-NULL registry, any
top-level `(reader-macros/define ...)` form in that file registers
into the *shared* registry. That means:

- Macros defined in `lib/a.tur` become visible to anything imported
  *after* `lib/a.tur` is loaded -- including `lib/a.tur`'s own
  remaining forms.
- They do *not* retroactively become visible to files loaded *before*
  `lib/a.tur`. Imports run in dependency order, so this matches the
  natural "define before use" expectation.

This is the same single-pass semantics RM1 already gives within a
single file; it just now spans files within a compile.

### 5. `#use-reader-macros` inside imported modules

This already works without further changes: the loader calls
`read_all_with_registry` with the shared registry, which goes through
`try_consume_use_directive`, which calls `reader_macros_load_file` on
the shared registry. A module that opens with
`#use-reader-macros "macros.tur"` will register its macros into the
shared registry just like a stand-alone file does.

---

## Phasing

### T0 -- Plumb the registry pointer + add strict mode

- Add `ReaderMacroRegistry *user_macros` to `Elab` (forward-declared).
- Add `ReaderMacroRegistry *reader_macros` to `PassContext` (already
  the seam between compile and elaborate).
- Default both to NULL; `compile_to_c` (and `_h`/`_implementation`)
  sets `ctx.reader_macros = &reader_macros_reg`.
- `run_core_passes` propagates into `Elab`.
- **Strict-mode flag** (decision #2): add `bool strict` to
  `ReaderMacroRegistry` (default false). Have `compile_to_c` & co.
  set it to true after `reader_macros_init`; leave the
  TuriEnv-allocated REPL registry as false. Update
  `reader_macros_register` to emit "already registered, previously
  here" with a span note when the existing entry collides and the
  registry is strict; otherwise keep the silent-update path.
- **No loader behavior change yet** -- both module loaders still use
  bare `read_all`. Test: existing suite passes unchanged; REPL smoke
  unchanged; compile-mode collision now errors out (add a fixture).

### T1 -- Switch elab_module.c + import-chain notes

- One-line swap to `read_all_with_registry(... e->user_macros ...)`.
- **Import-chain notes** (decision #4): pass the `import` site's span
  into `reader_macros_load_file` / `read_all_with_registry` so any
  reader error emitted while loading the sub-module attaches a
  `note: while loading module 'X' imported from Y:L:C`. Simplest
  shape: a new optional `Span parent_import_site` parameter (or a
  thread-local "load chain" the reader push/pops). Use the existing
  `diag_emit_with_notes`.
- Add a fixture: spice with two modules, manifest declares
  `:reader-macros`, both modules use `#foo{...}` invocations.
  Without T1 the imported module errors; with T1 it succeeds.
- Add a negative fixture: imported module uses an unregistered
  `#bogus{...}`; the diagnostic should include the import-chain note.

### T2 -- Switch elab_toplevel.c (`load`) + matching notes

- Same one-line swap for the `(load "...")` directive.
- Same import-chain note treatment (the `(load ...)` site span flows
  the same way).
- Fixture: entry uses `(load "lib.tur")`, lib.tur uses
  manifest-declared macros.

### T3 -- Cross-module define visibility

- Verify that `(reader-macros/define ...)` in an imported module
  registers into the shared registry and is visible to subsequent
  imports.
- Fixture: `lib/syntax.tur` contains only define forms, `main.tur`
  does `(import lib/syntax)` then uses the macros.

### T4 -- Docs

- Update `docs/reader-macros-plan.md` with a "Loading semantics"
  section covering the order-dependent visibility (decision #3) and
  the `import` / `(load ...)` equivalence (decision #1, doc half).
- Add inline comments at both `read_all_with_registry` call sites in
  `elab_module.c` and `elab_toplevel.c` cross-referencing the doc
  (decision #1, code half).

---

## Where the changes go

| File | Change |
|------|--------|
| `src/compiler/reader_macros.h` | Add `bool strict` to `ReaderMacroRegistry` (decision #2). |
| `src/compiler/reader_macros.c` | In `reader_macros_register`, when an entry collides and `reg->strict` is true, emit `error: reader macro '#X' already registered` with a `note: previously registered here` carrying the existing entry's `site`. When false, keep the silent update path. |
| `src/compiler/elab_internal.h` | Add `ReaderMacroRegistry *user_macros` field to `Elab`; forward-declare the struct. |
| `src/compiler/elab.h` (or wherever `PassContext` lives) | Add `ReaderMacroRegistry *reader_macros` to `PassContext`. |
| `src/compiler/elab_*.c` | In `elaborate_program` / `run_core_passes`, copy `ctx->reader_macros` into the new `Elab` field. |
| `src/compiler/elab_module.c::~187` | (a) Swap to `read_all_with_registry(... e->user_macros ...)`. (b) Thread the import-site span into the sub-read so reader diagnostics attach a `while loading module X imported from Y:L:C` note (decision #4). (c) Add the comment cross-referencing the docs (decision #1). |
| `src/compiler/elab_toplevel.c::~489` | Same three changes for `(load ...)`. |
| `src/main.c` | In `compile_to_c` / `compile_to_h` / `compile_to_implementation`, set `ctx.reader_macros = &reader_macros_reg` *and* set `reader_macros_reg.strict = true` before invoking the passes. |
| `src/turi/env.c` | Leave `env->reader_macros->strict` false (default) -- REPL keeps the silent-update behavior. |
| `docs/reader-macros-plan.md` | New "Loading semantics" section covering order-dependent visibility and the `import`/`(load ...)` equivalence (decisions #1, #3). |
| `tests/fixtures/reader-macros-transitive/` | New fixture exercising cross-module macro visibility (positive: shared registry works). |
| `tests/fixtures/errors/reader-macros-import-chain/` | Negative fixture: unregistered macro use in an imported module triggers the `while loading` note (decision #4). |
| `tests/fixtures/errors/reader-macros-strict-collision/` | Negative fixture: two `(reader-macros/define ...)` for the same name in a compile fail in strict mode (decision #2). |
| `tests/spice-resolver-tests.sh` | New cases running the three new fixtures. |

---

## Resolved decisions

The four originally-open questions were resolved interactively before
implementation. Recording them inline here so the plan is
self-contained.

1. **`(load ...)` vs `import` equivalence — *document in both places*.**
   Reader macros are top-level by nature, so the namespace distinction
   between `import` and `(load ...)` is irrelevant -- both populate the
   shared registry. Add (a) a short note to the user-facing
   `reader-macros-plan.md` (under "Composition with regular macros" or
   a new "Loading semantics" section), and (b) inline comments next to
   both `read_all_with_registry` call sites in `elab_module.c` and
   `elab_toplevel.c` so a future maintainer doesn't have to dig.

2. **Macro collision — *mode-dependent: strict in compile, silent in
   REPL*.** Add a `strict` flag to `ReaderMacroRegistry` (or a
   parallel `reader_macros_register_strict` entry point). Batch-compile
   registries (`compile_to_c` and friends) initialise with strict=true,
   so a second `(reader-macros/define 'json ...)` errors with a
   `previously registered here` note pointing at the first
   registration's `site` span. REPL/TuriEnv registry keeps strict=false
   so `src_acc` replay and iterative redefinition stay smooth. About
   ~15 LOC in `reader_macros.{h,c}` plus the flag init at each
   construction site.

3. **Order-dependent visibility — *yes, single-pass*.** A module's
   macros are visible only to imports that come *after* it. Matches
   Lisp `(require)` and Clojure `(use)` traditions; matches how
   regular code imports already work; avoids the double-read cost of a
   collect-then-reparse pre-pass. Manifest-level macros and
   `#use-reader-macros` already give effectively-global scope when the
   user wants it, so the order constraint is rarely binding in
   practice.

4. **Diagnostics for errors in imported modules — *fold the
   import-chain notes into T1/T2*.** Use the existing
   `diag_emit_with_notes` machinery to attach a
   `note: while loading module 'X' imported from Y:L:C` chain to any
   reader error emitted from a sub-read. The import site already has
   the parent span; thread it through to the sub-read so
   `try_read_user_macro` and `read_raw_body` can include it in their
   diagnostics. About ~10 LOC in `elab_module.c` /
   `elab_toplevel.c` plus a small extension to either `Reader` or the
   load helper. No separate T4 phase -- this lands with the loader
   switch.

---

## Risk and scope

- **Risk: low** for T0. Adding the `strict` flag is a small, local
  change in `reader_macros.c`; REPL behavior is preserved because
  `strict` defaults to false and only compile entry points set it
  true. The registry-pointer plumbing is shape-neutral.
- **Risk: low-medium** for T1/T2. Both module loaders already
  arena-allocate the parsed Forms with `e->arena`, so threading a
  registry that lives in the same arena is allocation-shape-neutral.
  The import-chain notes are the only mildly fiddly bit (passing the
  parent span into the reader); use a thread-local or a small
  per-load stack rather than retrofitting every reader helper.
- **No risk** to the REPL path -- `TuriEnv->reader_macros` is
  explicitly left non-strict so `src_acc` replay continues to work.

LOC estimate: ~15 lines for strict-mode (T0), ~50 lines of plumbing
across `elab.h`, `elab_internal.h`, `elab_module.c`,
`elab_toplevel.c`, `main.c` (T0-T2), plus ~120 lines of fixtures and
test cases (T1-T3) and ~30 lines of docs (T4).
