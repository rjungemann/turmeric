---
title: Developing Spices Guide
category: Package Management
description: Creating, testing, and publishing Turmeric spice packages
---

# Developing Spices Guide

A *spice* is a Turmeric package that other projects depend on. This guide
covers every step of authoring one: project structure, declaring exports,
wrapping C libraries with `:cmake-deps`, testing, versioning, and publishing.

See [Consuming Spices](consuming-spices-guide.md) if you only need to add
an existing spice to your project.

---

## When to Create a Spice

Create a spice when you want to:

- Share a library across multiple projects.
- Wrap a C library and expose it as a clean Turmeric API.
- Contribute to the official
  [turmeric-spices](https://github.com/rjungemann/turmeric-spices) monorepo.
- Distribute a library that others can add with `tur add`.

For code shared only within a single project, use the module system -- no
separate package is needed.

---

## Scaffolding the Project

```sh
tur new tur-mylib --lib
cd tur-mylib
```

This creates:

```
tur-mylib/
  build.tur        -- package manifest
  tur.lock         -- empty lock file (commit to VCS)
  src/
    lib.tur        -- stub exported function
  .gitignore
  README.md
```

By convention, name the package with a `tur-` prefix. Consumers drop the
prefix as the import alias: `tur-geom` becomes `geom`, `tur-math` becomes
`math`.

---

## The `build.tur` Manifest

A complete library manifest:

```turmeric
(defpackage tur-mylib
  :name        "tur-mylib"
  :version     "0.1.0"
  :description "A brief description of my library"
  :license     "MIT"
  :authors     ["Your Name <you@example.com>"]
  :repository  "https://github.com/you/tur-mylib"

  :exports {
    "mylib/core" ["some-fn" "another-fn"]
    "mylib/util" ["helper-fn"]
  })
```

```sweet-exp
defpackage tur-mylib
  :name        "tur-mylib"
  :version     "0.1.0"
  :description "A brief description of my library"
  :license     "MIT"
  :authors     ["Your Name <you@example.com>"]
  :repository  "https://github.com/you/tur-mylib"

  :exports {
    "mylib/core" ["some-fn" "another-fn"]
    "mylib/util" ["helper-fn"]
  }
```

### Key manifest fields

| Field | Required | Notes |
|---|---|---|
| `:name` | Yes | Must match `[a-z][a-z0-9-]*` |
| `:version` | Yes | Semver: `MAJOR.MINOR.PATCH` |
| `:description` | Recommended | One-line summary |
| `:license` | Recommended | SPDX identifier (e.g. `"MIT"`) |
| `:authors` | Recommended | `"Name <email>"` list |
| `:repository` | Recommended | URL to the canonical Git repo |
| `:exports` | Yes (library) | Map of module path to exported symbol names |
| `:spices` | If needed | Turmeric package dependencies |
| `:cmake-deps` | If needed | C/C++ library dependencies |
| `:build-opts` | Rarely | Extra C compiler flags or additional link libs |

---

## Declaring Exports

The `:exports` map controls what is visible to consumers. Only listed
symbols are part of the public API; everything else is private.

```turmeric
:exports {
  "mylib/types" ["Coord" "Rect" "Color"]
  "mylib/draw"  ["draw-rect" "draw-circle" "draw-line"]
  "mylib/io"    ["read-file" "write-file"]
}
```

Each key (`"mylib/types"`) becomes an importable path for consumers:

```turmeric
(import mylib/types :refer [Coord Rect])
(import mylib/draw  :refer [draw-rect])
```

```sweet-exp
import mylib/types :refer [Coord Rect]
import mylib/draw  :refer [draw-rect]
```

Internal helpers that should not be exposed are simply omitted from
`:exports`.

---

## Source Layout

Follow this layout so that module paths and file paths align without
extra configuration:

```
tur-mylib/
  build.tur
  tur.lock
  src/
    types.tur       -- exports "mylib/types"
    draw.tur        -- exports "mylib/draw"
    io.tur          -- exports "mylib/io"
    internal/
      helpers.tur   -- not exported; only imported by other src files
  tests/
    types_test.tur
    draw_test.tur
```

The module path `"mylib/types"` resolves to `src/types.tur`. A nested path
`"mylib/net/http"` resolves to `src/net/http.tur`.

---

## Multi-file Spices: `defmodule` + `import`

When a spice spans more than one source file, use `defmodule` with an explicit
`(export ...)` list and `(import <other-module> :refer [...])` to wire the
files together. The module system handles cross-file symbol sharing correctly
at every scale.

### The correct approach

Each file declares its own `defmodule`, exports what it offers, and imports
what it needs from siblings:

```turmeric
;; src/mylib/types.tur
(defmodule mylib/types
  (export Widget WidgetResult)
  (defstruct Widget [value :int])
  ...)
```

```turmeric
;; src/mylib/core.tur
(defmodule mylib/core
  (export make-widget widget-value)
  (import mylib/types :refer [Widget WidgetResult])

  (defn make-widget [v :int] :Widget ...)
  (defn widget-value [w :Widget] :int ...))
```

`tur check`, `tur emit-c`, and `tur build` all resolve intra-spice imports
automatically through the auto-spice include path -- no `-I` flags needed.
See [Per-file Commands Inside a Spice](#per-file-commands-inside-a-spice).

### Anti-pattern: cross-file type stubs

An older pattern copies function bodies across files so each file compiles
in isolation without imports:

```turmeric
;; src/mylib/io.tur -- ANTI-PATTERN
;; Stub copy of make-widget from core.tur; real body is return NULL.
(defn make-widget [v :int] #{Unsafe} :Widget
  ```c
  return NULL;
  ```)

(defn read-widget [path :cstr] :Widget ...)  ;; real implementation
```

This pattern fails in three ways:

- **Silent breakage**: stubs with `return NULL` or no-op bodies mean any
  code path that hits the stub produces garbage or does nothing -- with no
  diagnostic.
- **Linker errors when combined**: two files that both define `make-widget`
  as a `static` C function produce duplicate-symbol link failures the moment
  they are compiled together (via `(load ...)` or `tur build`).
- **Per-file checks appear clean**: each file is self-contained so
  `tur check`/`tur emit-c` on a single file passes while the combined build
  fails.

The `scscm` spice used this pattern across all five of its source files.
It was eliminated in the 2026-05 import refactor by converting each file to
`defmodule` + `import`. See
[scscm-spice-import-refactor-plan.md](../scscm-spice-import-refactor-plan.md)
for the full migration. If you encounter the stub pattern in other spices,
the fix is the same: add a `(defmodule ...)` + `(export ...)` header and
replace each stub block with `(import <module> :refer [...])`.

---

## Depending on Other Spices

Add Turmeric spice dependencies the same way any project does:

```sh
tur add https://github.com/rjungemann/turmeric-spices \
  --ref math-v0.1.0 --subdir spices/math --name math
```

This produces:

```turmeric
:spices {
  "math" {:url    "https://github.com/rjungemann/turmeric-spices"
          :ref    "math-v0.1.0"
          :subdir "spices/math"}
}
```

```sweet-exp
:spices {
  "math" {:url    "https://github.com/rjungemann/turmeric-spices"
          :ref    "math-v0.1.0"
          :subdir "spices/math"}
}
```

Mark spices that are only needed for tests `:optional true` so consumers
are not forced to fetch them:

```turmeric
:spices {
  "test" {:url    "https://github.com/rjungemann/turmeric-spices"
          :ref    "test-v0.1.0"
          :subdir "spices/test"
          :optional true}
}
```

```sweet-exp
:spices {
  "test" {:url    "https://github.com/rjungemann/turmeric-spices"
          :ref    "test-v0.1.0"
          :subdir "spices/test"
          :optional true}
}
```

---

## Cross-spice Development in a Workspace

When your spices live in the same workspace (a parent directory with a `build.tur`
that lists all members under `:members`), you can import one sibling from another
**without publishing a release or running `tur fetch`**.

### Option A: workspace-member auto-resolution (no manifest entry needed)

If both spices are already listed in the workspace `build.tur`:

```turmeric
;; turmeric-spices/build.tur
(defpackage turmeric-spices
  :members ["spices/watch" "spices/notebook" ...])
```

Then `notebook` can `(import watch/watch ...)` directly -- the resolver walks up
to the workspace `build.tur`, finds that `watch` is a listed member, and adds its
`src/` to the search path automatically.

```sh
# No tur fetch, no symlink, no lock entry required:
cd spices/notebook
tur check src/notebook/cli.tur   # resolves watch/watch via workspace
```

The first time an undeclared sibling import resolves, `tur` prints a one-time
advisory:

```
warning: import 'watch/watch' resolved via workspace sibling
         'spices/watch'; declare it in :spices for release builds.
         (set TUR_DEBUG_RESOLVER=1 for full resolver tracing)
```

To confirm a name is a workspace member before importing:

```sh
tur add --workspace watch   # exits 0, prints "no manifest entry needed"
tur add --workspace typo    # fails if 'typo' is not in :members
```

### Option B: explicit `:path` dep (works outside workspaces too)

Add a `:path` entry pointing at the sibling spice directory:

```sh
tur add ../watch --path
```

This writes to `build.tur`:

```turmeric
:spices {
  "watch" {:path "../watch"}
}
```

```sweet-exp
:spices {
  "watch" {:path "../watch"}
}
```

The resolver immediately adds `../watch/src` to the include path.
No `tur fetch` step is required, and no lock entry is written for this dep.
The path is resolved relative to the directory containing `build.tur`.

```sh
tur check src/notebook/cli.tur   # resolves watch/watch via :path
```

If the declared `:path` does not exist on disk (or contains no `build.tur`),
`tur fetch` reports a hard error rather than silently ignoring the missing dep.

### `tur fetch --dry-run` for verification

To confirm how each dep will be classified before committing to a real fetch:

```sh
tur fetch --dry-run
# skip :path          watch
# skip workspace member  alpha
# fetch  URL          ansi  https://github.com/...  ansi-v0.1.4
# summary: 1 fetch, 2 skipped (local)
```

Local-source deps are never contacted or locked. Only URL deps appear in `tur.lock`.

### Release: switch to a URL dep for external consumers

The workspace auto-resolution and `:path` entries are local-development shortcuts.
For a spice that will be published and consumed outside the workspace, add a URL
entry alongside (or instead of) the local one:

```turmeric
:spices {
  "watch" {:url    "https://github.com/rjungemann/turmeric-spices"
           :ref    "watch-v0.1.0"
           :subdir "spices/watch"}
}
```

```sweet-exp
:spices {
  "watch" {:url    "https://github.com/rjungemann/turmeric-spices"
           :ref    "watch-v0.1.0"
           :subdir "spices/watch"}
}
```

External consumers run `tur fetch` once to clone the ref and populate `tur.lock`.
Inside the workspace, a URL `:spices` entry whose name matches a workspace member
is resolved locally instead -- no spurious fetch occurs for deps you are developing
in the same workspace.

---

## Wrapping a C Library with `:cmake-deps`

The `:cmake-deps` block tells `tur build` to fetch and compile a C library
via CMake, then inject the resulting include dirs and link flags into the
compilation step. You write only Turmeric; CMake is an implementation detail.

### Declaring the dependency

```turmeric
:cmake-deps {
  "sqlite3" {:url     "https://github.com/sqlite/sqlite"
             :ref     "version-3.47.2"
             :options {:BUILD_SHARED_LIBS "OFF"}}
}
```

```sweet-exp
:cmake-deps {
  "sqlite3" {:url     "https://github.com/sqlite/sqlite"
             :ref     "version-3.47.2"
             :options {:BUILD_SHARED_LIBS "OFF"}}
}
```

`:options` sets CMake cache variables (`-DKEY=VALUE`). Common patterns:

- Disable shared libs: `:BUILD_SHARED_LIBS "OFF"`
- Disable examples or tests: `:BUILD_EXAMPLES "OFF"`, `:YYJSON_BUILD_TESTS "OFF"`
- Control feature sets: depends on the upstream library's own CMake options

### Declaring C symbols in Turmeric

Use `include-c` to pull in the C header and `extern-c` to declare symbols:

```turmeric
;; src/db.tur
(include-c "sqlite3.h")

(extern-c sqlite3_open   [:cstr :ptr] :int)
(extern-c sqlite3_close  [:ptr]       :int)
(extern-c sqlite3_exec   [:ptr :cstr :ptr :ptr :ptr] :int)
(extern-c sqlite3_errmsg [:ptr] :cstr)

(defn db-open [path :cstr] (result :ptr)
  (let [db-ptr (make-ptr)]
    (let [rc (sqlite3_open path db-ptr)]
      (if (= rc 0)
        (ok (deref-ptr db-ptr))
        (err (cstr->str (sqlite3_errmsg (deref-ptr db-ptr))))))))
```

```sweet-exp
;; src/db.tur
include-c "sqlite3.h"

extern-c sqlite3_open   [:cstr :ptr] :int
extern-c sqlite3_close  [:ptr]       :int
extern-c sqlite3_exec   [:ptr :cstr :ptr :ptr :ptr] :int
extern-c sqlite3_errmsg [:ptr] :cstr

defn db-open [path :cstr] (result :ptr)
  let [db-ptr (make-ptr)]
    let [rc (sqlite3_open path db-ptr)]
      if {rc = 0}
        ok(deref-ptr(db-ptr))
        err(cstr->str(sqlite3_errmsg(deref-ptr(db-ptr))))
```

`extern-c` trusts the signature you give it -- double-check it against the
actual C header. No `-I` or `-L` flags are needed in your source; `tur build`
injects them from `cmake/spice-deps-manifest.json`.

### Inline C for small wrappers

When a binding is simpler to write directly in C, use an inline-C block:

```turmeric
(defn db-last-insert-rowid [db :ptr] :int
  ```c
  return (int)sqlite3_last_insert_rowid((sqlite3*)db);
  ```)
```

The closing ` ``` ` and its enclosing `)` must be on the same line. See
the [inline-C style rule](../../CLAUDE.md) for why.

### `:cmake-name` and `:targets` overrides

When the CMake `find_package` name or target name differs from the key in
`:cmake-deps`, supply overrides:

```turmeric
:cmake-deps {
  "sqlite" {:url        "https://github.com/sqlite/sqlite"
            :ref        "version-3.47.2"
            :cmake-name "SQLite3"
            :targets    ["SQLite::SQLite3"]}
}
```

For the full `:cmake-deps` field reference, the generated `SpiceDeps.cmake`
format, the `spice-deps-manifest.json` schema, and hash locking, see the
[CMake/CPM integration notes](../archive/cmake-cpm-integration-plan.md).

---

## Writing the Public API

Every exported symbol should have a `;;;` docstring immediately above its
definition. The standard format (from CLAUDE.md):

```turmeric
;;; db-open -- open an SQLite database file.
;;;
;;; Parameters:
;;;   path -- filesystem path to the database file (created if absent)
;;;
;;; Returns:
;;;   (ok db) on success, (err message) if the file cannot be opened
;;;
;;; Example:
;;;   (match (db-open "app.db")
;;;     (ok db) (println "opened")
;;;     (err m) (println "failed:" m))
;;;
;;; Since: Phase P2
(defn db-open [path :cstr] (result :ptr)
  ...)
```

Exported symbols without docstrings will be omitted from `just docs` output.

### Module docstring (optional)

A spice module can optionally include a module-level docstring at the very top
of the file, before the first `defn`, `defmacro`, `defstruct`, `definstance`,
or `defopaque`. Place a contiguous `;;;` block followed by a `;;` comment line
(which acts as the separator):

```turmeric
;;; myspice/db -- SQLite database bindings.
;;;
;;; Thin wrapper around libsqlite3; provides open/close/query/exec with
;;; result-typed error handling.
;;;
;;; Since: Phase P2
;; ---- SQLite bindings ----
(extern-c sqlite3_open ...)
```

Without a module docstring the page renders with no description block -- the
per-symbol cards still appear normally.

### Style: avoid `cons ... 0` chains in examples

In README quick-starts and docstring examples, do not show runtime list values
as `(cons x (cons y 0))` chains. The trailing `0` is the nil-of-list footgun --
new readers have no way to tell that `0` means "end of list," and the chain
itself reads in reversed nesting order.

Use the `list` macro instead. It expands to the same `tcons`/`tnil` cells, so
it's a drop-in for any API that today takes a `:int` cons list:

```turmeric
;; Avoid
(group-by f (cons "g" 0))
(plot (cons (axes) (cons (function f) 0)))

;; Prefer
(group-by f (list "g"))
(plot (list (axes) (function f)))
```

Pair-cons (`(cons key value)` with no trailing `0`) is fine -- a two-element
pair is a clear, idiomatic data shape. The footgun is the nil-terminated chain.

If your spice API can take a `Vec` instead of a cons list, prefer that and
document it with `(vec-of ...)`. The end goal is for `cons` to disappear from
quick-start surfaces entirely.

---

## Testing Your Spice

Add `tur-test` as an optional dependency and place test files in `tests/`:

```sh
tur add https://github.com/rjungemann/turmeric-spices \
  --ref test-v0.1.0 --subdir spices/test --name test
```

A test file using `tur-test`:

```turmeric
(import test/assert :refer [assert-eq assert-ok assert-err])
(import test/suite  :refer [describe it])
(import test/runner :refer [run-all])
(import mylib/core  :refer [some-fn])

(describe "some-fn"
  (it "returns the expected value"
    (assert-eq (some-fn 10) 42))
  (it "returns err for invalid input"
    (assert-err (some-fn nil))))

(run-all)
```

```sweet-exp
import test/assert :refer [assert-eq assert-ok assert-err]
import test/suite  :refer [describe it]
import test/runner :refer [run-all]
import mylib/core  :refer [some-fn]

describe "some-fn"
  it "returns the expected value"
    assert-eq(some-fn(10), 42)
  it "returns err for invalid input"
    assert-err(some-fn(nil))

run-all()
```

Run the test suite:

```sh
tur test
```

For the full testing API see [test-runner-contract.md](test-runner-contract.md).

---

## Versioning

- Use semver: `MAJOR.MINOR.PATCH`.
- Git tags must be `vMAJOR.MINOR.PATCH` for standalone repos (`v0.1.0`).
- For spices inside the `turmeric-spices` monorepo use the per-spice tag
  format `<spice>-vMAJOR.MINOR.PATCH` (`math-v0.1.0`, `sqlite-v0.2.1`).
- Pre-releases: `v0.2.0-alpha.1`.
- Bump `MAJOR` for breaking API changes, `MINOR` for additive changes,
  `PATCH` for bug fixes.

---

## Publishing

### As a standalone repository

```sh
git tag v0.1.0
git push && git push --tags
```

Consumers then add your spice with:

```sh
tur add https://github.com/you/tur-mylib --ref v0.1.0
```

### Contributing to turmeric-spices

The [turmeric-spices](https://github.com/rjungemann/turmeric-spices) monorepo
accepts spices that meet the bar for the ecosystem. To contribute:

1. Fork the monorepo and add your spice under `spices/<name>/`.
2. Add it to the workspace root `build.tur` `:members` list.
3. Include a `README.md` and at least one test file.
4. Open a pull request.

After merging, tag the spice's first release:

```sh
git tag math-v0.1.0
git push --tags
```

Consumers use `--subdir spices/<name>` when adding:

```sh
tur add https://github.com/rjungemann/turmeric-spices \
  --ref myspice-v0.1.0 --subdir spices/myspice --name myspice
```

### Spice registry (future)

Once `pkg.turmeric-lang.org` launches, `tur publish` will register the
package and consumers will use `tur add spice/<name>` without a Git URL.

---

## Per-file Commands Inside a Spice

`tur build <dir>` and `tur run` (project mode) know about the spice they
live in because they start by reading `build.tur`. For `tur build <dir>`
that means: descend into `src/` (recursively, including nested
`src/<pkg>/` trees), skip the manifest itself, compile every module,
resolve the include path from the project's own `src/` plus each
`:spices` dep's `src/`, and verify each declared `:exports` module has a
backing source file -- failing loudly otherwise. (See
[manifest-driven-build-descent-plan.md](../manifest-driven-build-descent-plan.md).)
The per-file subcommands `tur check`, `tur emit-c`, `tur emit-h`, and
`tur run <file>` get the same module resolution automatically -- they
walk up from the input file looking for a sibling `build.tur` and add
that spice's `src/` (and its `:spices` deps' `src/`) to the include path.

This means editors, format-on-save hooks, LSP clients, and quick
"compile this one file" loops work without per-spice configuration:

```sh
cd spices/frame
tur check src/frame/frame.tur     # resolves intra-spice imports
tur emit-c src/frame/schema.tur   # same
tur run src/frame/quickstart.tur  # builds and executes
```

The walk-up is capped at 16 ancestor directories, so a stray
`build.tur` far above your working tree won't accidentally win.

### `-I` for extra directories

Explicit `-I <dir>` flags still work and take priority over
auto-discovered paths -- useful for fixtures, vendored copies, or when
you want to test against a different version of a dep:

```sh
tur check -I vendor/alternate src/main.tur
```

`-I` accepts both the spaced (`-I path`) and concatenated (`-Ipath`)
forms.

### `--no-auto-spice` escape hatch

If you need to compile a file as if no spice exists around it (rare --
typically only useful for resolver-fixture tests or when an unrelated
`build.tur` is in the ancestor chain), pass `--no-auto-spice`:

```sh
tur --no-auto-spice check tests/fixtures/resolver/input.tur
```

This restores the pre-auto-discovery behavior: only the input file's
own directory, the stdlib, and any explicit `-I` paths are searched.

### When auto-discovery does not apply

- `tur build <dir>` -- already configures itself from the directory's
  `build.tur` (reads the manifest, descends into `src/`, and resolves the
  include path); the per-file walk-up does not apply.
- `tur build <file>` -- the single-file build doesn't auto-discover;
  use `tur run <file>` if you want the same convenience for a one-off
  invocation, or pass `-I` explicitly.
- `tur format <file>` -- the formatter doesn't resolve imports, so
  include paths are irrelevant.

---

## Supporting CMake Consumers

If you want C and C++ projects to consume your spice via CMake or CPM
without knowing anything about Turmeric, run:

```sh
tur emit-cmake
```

This reads `build.tur` and generates `CMakeLists.txt`,
`<name>Config.cmake`, and helper modules. Commit those files, tag the
release, and a CPM consumer can add your library with:

```cmake
CPMAddPackage(
  NAME    tur-mylib
  URL     https://github.com/you/tur-mylib/archive/refs/tags/v0.1.0.tar.gz
  VERSION 0.1.0
)
target_link_libraries(my_app PRIVATE tur-mylib::all)
```

For the complete step-by-step see
[Using a Turmeric library from CMake](using-turmeric-from-cmake.md).

---

## Emscripten / WASM Support

Once `tur build --target wasm` lands, all cmake-deps spices will get WASM
builds automatically when the underlying C library supports Emscripten.
To make your spice Emscripten-compatible:

- Prefer C libraries with documented Emscripten support (yyjson, mbedTLS,
  PCRE2, and the SQLite amalgamation all qualify).
- Avoid libraries that require native threads or sockets without WASM
  fallbacks.
- Use `#ifdef __EMSCRIPTEN__` in inline-C blocks to handle differences
  (e.g. skipping native TLS when the browser provides its own TLS stack).

---

## Global Spices as Libraries (v2)

A spice installed globally with `tur install` is currently usable as a
**command-line tool only**: its `:bin` entries are symlinked into
`~/.local/bin/` and become available as `tur-<cmd>` (or via the
`tur <cmd>` fallthrough). The same install is **not** automatically
visible as a library to other projects -- the global `spices/` root is
left out of the default module-resolution path to preserve build
reproducibility.

A future v2 will let a project opt in to consuming a globally-installed
spice as a library by naming it in its `build.tur`:

```turmeric
:spices {
  "notebook" {:global true}
}
```

`tur fetch` would then validate the global install exists at a matching
version and record its resolved SHA in `tur.lock`. A project-level
`:global-policy` knob would decide whether a missing global install gets
auto-installed or errors out.

This is **deferred**; until it ships, a spice that wants to be reused as
a library should be added the normal way with `tur add`. See the
[global-spice-install plan](../global-spice-install-plan.md#imports-from-global-spices)
for the full design sketch.

---

## Release Checklist

- [ ] `tur build --release` passes with no warnings
- [ ] `tur test` passes
- [ ] All public symbols are listed in `:exports`
- [ ] Each exported symbol has a `;;;` docstring (summary + params + returns + example)
- [ ] cmake-deps spices build cleanly on macOS and Linux
- [ ] `tur emit-cmake` succeeds if you intend to support CMake consumers
- [ ] Generated CMake files are committed if supporting CMake consumers
- [ ] `CHANGELOG.md` entry written
- [ ] Git tag pushed: `git tag v0.1.0 && git push --tags`

---

## See Also

- [Consuming spices](consuming-spices-guide.md) -- adding and using spices in a project
- [Package management guide](package-management-guide.md) -- full `build.tur` manifest reference and `tur` CLI
- [C integration guide](c-integration-guide.md) -- `extern-c`, `include-c`, inline-C blocks
- [CMake/CPM integration notes](../archive/cmake-cpm-integration-plan.md) -- cmake-deps internals, generated files, hash locking, security
- [Using a Turmeric library from CMake](using-turmeric-from-cmake.md) -- step-by-step guide for `tur emit-cmake`
- [Test runner contract](test-runner-contract.md) -- full testing API
