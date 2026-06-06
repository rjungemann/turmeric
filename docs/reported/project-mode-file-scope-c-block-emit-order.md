---
title: Project-mode emit places file-scope ```c blocks after dependent defn bodies
category: Bug Report
status: FIXED
severity: medium (silently breaks the "hoist headers/typedefs to file scope" pattern that 3f86faf established for httpd/stats/tourist; works around with per-defn redeclarations)
description: A top-level ```c ... ``` block placed inside `defmodule` after `(export ...)` is supposed to land at C file scope, before any defn body, so its `#include`s and `typedef`s are visible everywhere. In project mode (`tur build .`), the compiler reorders defns by call-dependency and can emit individual defn bodies BEFORE the file-scope block, producing "use of undeclared identifier" errors against typedefs that were supposed to be in scope.
---

# Project-mode emit places file-scope ```c blocks after dependent defn bodies

## Summary

The `defmodule`-level file-scope C block convention introduced in
`3f86faf` (which fixed `httpd`/`stats`/`tourist` for macOS 14+ / Xcode 15+
by hoisting `#include <sys/socket.h>` etc. out of defn bodies) silently
fails when project-mode emits a defn body before the file-scope block.
The generated `.c` then references typedefs/helpers that are declared
later in the same file.

## Repro

`turmeric-spices` HEAD `3f86faf` against turmeric `main` HEAD `9d2df584`,
build the `spices/watch/` spice with a file-scope-c-block hoisted right
after `(export ...)` in `spices/watch/src/watch/watch.tur`:

```turmeric
(defmodule watch/watch
  (import ...)
  (export ...)

```c
#include <stdint.h>
#include <stdlib.h>
typedef struct __tur_cons_s { int64_t head; int64_t tail; } __tur_cons_cell;
/* ...other typedefs, static helpers... */
``​`

;;; many defns follow; near the end of the file:

(defn __watch-make-cons [head : int] #{Unsafe} : int
  ```c
  if (head == 0) return 0;
  __tur_cons_cell *cell = (__tur_cons_cell *)calloc(1, sizeof(*cell));
  ...
  ``​`)

(defn watch-drain [...]
  ;; ... calls __watch-make-cons ...
  )
)
```

Run `tur build .`. Observed `cc` failure in the generated `watch__watch.c`:

```
watch__watch.c:47:5: error: use of undeclared identifier '__tur_cons_cell'
watch__watch.c:47:22: error: use of undeclared identifier 'cell'
```

Inspect `watch__watch.c` and the layout is:

```
1..43   forward decls (static int64_t watch__watch__...)
45..53  defn body of __watch-make-cons   <-- USES __tur_cons_cell
55..65  defn body of watch_drain
67+     #include <stdint.h> ... typedef ... __tur_cons_cell;   <-- DECLARED HERE
```

The file-scope-c-block is at lines 67+, *after* the defn that references
its typedefs.

## Expected vs observed

**Expected:** the source ordering

```
(export ...)
``​`c <file-scope-c-block> ``​`
(defn ...) (defn ...) ...
```

produces an emit where the file-scope-c-block appears before any defn
body, matching how the same pattern works in `httpd__server.c`:

```
1..50    forward decls
51..107  #include <stdlib.h> ... struct __httpd_rawreq { ... } ...
110+     static int64_t httpd__server__srv_hymake_hylistener(...) { ... }
```

**Observed:** project-mode appears to reorder defns by call-dependency
(callee-before-caller), and the file-scope-c-block lands at whatever
position the *first* emitted defn was originally sourced at. If any
defn that depends on the typedefs is emitted earlier in the .c than
the block, it fails to compile.

In the `watch` case, `__watch-make-cons` is defined near the end of
`watch.tur` (line 984) but is called by `watch-drain` (exported), so
it gets emitted near the top of the .c -- before the file-scope-c-block
which was sourced between `(export ...)` and the *first* defn in source
order.

## Workaround currently in tree

Per-defn local redeclaration of the typedef inside the offending defn
body:

```turmeric
(defn __watch-make-cons [head : int] #{Unsafe} : int
  ```c
  /* Project-mode emit may place this defn before the file-scope-c-block,
     so redeclare the cons-cell type locally. */
  typedef struct __tur_cons_s_local { int64_t head; int64_t tail; } __tur_cons_cell_local;
  if (head == 0) return 0;
  __tur_cons_cell_local *cell = (__tur_cons_cell_local *)calloc(1, sizeof(*cell));
  ...
  ``​`)
```

This is fine for one-off types but defeats the whole point of the
file-scope-c-block (deduplicating headers/typedefs across many defns).

## Why httpd works and watch does not

`httpd/server.tur` happens to define its defns in source-order such that
the very first emitted defn appears *after* the file-scope-c-block in
the input. Dependency reordering does not perturb the relative position
of the C block. `watch/watch.tur` has the file-scope block immediately
after `(export ...)` (correct per the established pattern), but its
defns are dependency-shuffled in the emit, hoisting a late-source defn
to a position earlier than the block.

The difference is purely accidental -- both spices follow the same
documented pattern.

## Root-cause hypothesis

Project-mode (`compile_to_implementation`) likely walks the defn list
in dependency / call-graph order to emit forward decls + bodies, while
treating bare top-level inline-C blocks as ordinary forms emitted at
their original source position. The two orderings can interleave
incorrectly. See `src/main.c` around the `load_project_prelude`
neighborhood and the per-defn emit loop.

## Proposed fix directions

1. **Always emit top-level inline-C blocks before any defn body in the
   same TU.** This matches the contract the file-scope-c-block pattern
   relies on. Simple: bucket forms into (a) file-scope-c-blocks and
   (b) defns; emit (a) first, then (b) in dependency order.

2. **Preserve source order for top-level inline-C only**, but emit each
   block at the *minimum* source position across all defns that could
   move past it. Conservative; equivalent to (1) if every block is
   meant to be visible to every defn.

3. **Stop reordering defns in project mode.** If forward decls already
   resolve all cross-references, the emit order doesn't matter --
   keep source order so the file-scope-c-block contract holds
   trivially. (Check whether the reordering is load-bearing for any
   feature; if not, drop it.)

(1) is the smallest change with the clearest semantics.

## Validation

A fixture under `tests/fixtures/` that:

- defines a `defmodule` with `(export A)` where A is the *first* defn
  in source,
- declares a file-scope ```c typedef``` block between `(export)` and
  the first defn,
- defines B (uses the typedef) *after* A in source,
- has A call B so dependency-order would emit B first,
- builds via `tur build .` (not `tur emit-c`),
- expects zero `cc` errors.

This currently fails for the watch case and should fail for the new
fixture too.

## Related

- `3f86faf` in `turmeric-spices`: the commit that established the
  file-scope-c-block pattern for `httpd`/`stats`/`tourist`.
- `docs/upcoming/spices-v0.18-typing-migration-plan.md` P1/P2: the
  context in which this was hit while migrating `spices/watch/`.
