# Spice Plan: tur-watch

> **Status:** Draft Plan
> **Last Updated:** 2026-05-25
> **Type:** Spice Design / Tooling
> **Depends on:** [notebook-spice-plan.md](../notebook-spice-plan.md) (extract after NB12 / `notebook-v0.1.0`, once notebook watch mode is complete and shipped)

---

## Overview

One new spice for the `turmeric-spices` monorepo:

| Spice | Tag | Depends on | Purpose |
|-------|-----|------------|---------|
| `tur-watch` | `watch-v0.1.0` | (none, pure Turmeric + inline-C) | Cross-platform filesystem watching with debounce / coalescing helpers |

`tur-watch` is a standalone spice extracted from the file-watching machinery
first built for `tur-notebook`'s `tur nb render --watch` flow. The notebook
plan currently treats watching as renderer-private functionality: run once,
listen for file changes via `kqueue` / `inotify`, debounce editor save bursts,
then re-render in a fresh session.

That behavior is useful well beyond notebooks:

- `tur-notebook` itself should not own the long-term watcher implementation
- `tur repl --watch` in [spice-repl-plan.md](../spice-repl-plan.md) needs the
  same cross-platform watch + debounce behavior
- future `tur build --watch`, `tur run --watch`, docs preview, and live-test
  workflows all want the same primitive

This plan turns the notebook watcher into a reusable library with a stable,
versioned API, then refactors notebook to depend on it rather than carrying
its own inline-C backend.

**No external C dependency.** The v0.1.0 implementation uses a small inline-C
bridge over platform facilities already assumed by the notebook plan:

- Linux: `inotify`
- Darwin / BSD: `kqueue`

Windows and macOS `FSEvents` are explicitly deferred.

---

## Why a separate spice

Keeping watch mode inside `notebook/cli` is fine for the first implementation,
but it is the wrong long-term ownership boundary.

- **One watcher implementation to debug.** Atomic-save editors, rename storms,
  temporary files, and duplicate notifications are all subtle. Fixing those
  once in one library is better than copying watch loops into every tool.
- **Shared semantics across CLIs.** `tur nb --watch`, `tur repl --watch`, and
  future build / test watch commands should agree on what "modified",
  "renamed", "deleted", and "debounced burst" mean.
- **Cleaner notebook code.** Notebook should decide *what to do* when a file
  changes; `tur-watch` should decide *how changes are observed*.
- **Versioned reuse for spices.** Any spice can depend on `watch-v0.1.0`
  instead of hand-rolling an `inotify` / `kqueue` bridge.

---

## Scope

`tur-watch` v0.1.0 covers the practical watch functionality already implied by
notebook watch mode, generalized enough for CLI tools:

| Area | Included | Deferred |
|------|----------|----------|
| Watch one file | yes | -- |
| Watch multiple explicit files | yes | -- |
| Watch one directory | yes | -- |
| Recursive directory watch | yes, via directory tree registration | -- |
| Event kinds (`write`, `create`, `delete`, `rename`, `attrib`, `overflow`) | yes | -- |
| Debounce bursty writes | yes | -- |
| Coalesce repeated events for same path | yes | -- |
| Blocking next-event API | yes | -- |
| Timed poll API | yes | -- |
| Batch drain API | yes | -- |
| Callback / async event loop abstraction | no | v0.2 |
| macOS `FSEvents` backend | no | v0.2 |
| Windows backend | no | future |
| Symlink cycle detection for recursive tree watch | minimal | richer policy later |
| Content hashing / "only notify when bytes changed" | no | future |

The key design rule: v0.1.0 should be strong enough to replace notebook's
watch loop directly, while staying small enough that it can actually be
extracted from working code rather than designed in the abstract.

---

## Conventions

Standard spice layout:

```
spices/watch/
  build.tur
  README.md
  src/watch/
    event.tur      -- "watch/event"   event struct, kind constants, accessors
    opts.tur       -- "watch/opts"    watcher options + defaults
    debounce.tur   -- "watch/debounce" coalescing / burst-collapse helpers
    watch.tur      -- "watch/watch"   public watcher API
    backend_linux.tur   -- inline-C bridge for inotify
    backend_darwin.tur  -- inline-C bridge for kqueue
    internal.tur   -- shared private helpers for path bookkeeping / tree refresh
  tests/watch/
    event_test.tur
    debounce_test.tur
    watch_test.tur
    recursive_test.tur
  examples/
    tail-events.tur
    rerun-command.tur
    watch-tree.tur
```

Public callers import only `watch/event`, `watch/opts`, and `watch/watch`.
Backend modules are internal implementation details and are not exported.

---

## Architecture

```
  client (notebook / repl / future build-watch)
                |
                v
          watch/watch
                |
        +-------+--------+
        |                |
        v                v
   watch/debounce    watch/event
        |
        v
     backend select
   (linux / darwin)
        |
        v
  inotify or kqueue handles
        |
        v
   normalized event stream
```

The library splits cleanly into three layers:

1. **Backend layer** -- reads raw platform events and converts them into a
   small internal event record.
2. **Normalization layer** -- canonicalizes duplicate / partial backend events
   into cross-platform event kinds with stable path strings.
3. **Debounce / batching layer** -- collapses bursty editor saves into the
   unit a CLI actually wants to react to.

Notebook should consume only the public normalized stream and never inspect
platform-specific event flags directly.

---

## Public API

### watch/event

```turmeric
;;; watch-event -- one normalized filesystem event.
(defstruct watch-event
  root      :cstr   ;; registered root that produced the event
  path      :cstr   ;; path relative to root when possible, else absolute
  full-path :cstr   ;; absolute path when known
  kind      :int    ;; see watch-kind-* constants
  is-dir    :int    ;; 0/1
  cookie    :int)   ;; rename correlation id or 0

(watch-kind-write)    ;; => :int
(watch-kind-create)   ;; => :int
(watch-kind-delete)   ;; => :int
(watch-kind-rename)   ;; => :int
(watch-kind-attrib)   ;; => :int
(watch-kind-overflow) ;; => :int
```

### watch/opts

```turmeric
;;; watch-opts -- watcher configuration.
(defstruct watch-opts
  recursive    :int  ;; 0 = explicit paths only, 1 = walk subdirs
  debounce-ms  :int  ;; default burst-collapse window
  coalesce     :int  ;; 0 = raw normalized stream, 1 = merge duplicates
  attrib       :int  ;; include metadata-only changes
  follow-moves :int) ;; try to preserve watched paths across rename/write-save flows

(default-watch-opts)  ;; => watch-opts
```

### watch/watch

```turmeric
;;; watch-open -- register one or more files/directories.
(watch-open paths opts)              ;; => result<watcher>

;;; watch-close -- free all OS resources.
(watch-close watcher)                ;; => :void

;;; watch-add-path / watch-remove-path -- update a live watcher.
(watch-add-path watcher path)        ;; => result<:void>
(watch-remove-path watcher path)     ;; => result<:void>

;;; watch-next -- block until next event or timeout.
;;; timeout-ms: -1 = forever, 0 = poll, N = bounded wait
(watch-next watcher timeout-ms)      ;; => option<watch-event>

;;; watch-drain -- gather a debounced batch.
;;; Reads until quiet for debounce-ms after the first event.
(watch-drain watcher timeout-ms debounce-ms)
                                     ;; => list<watch-event>

;;; watch-refresh -- re-scan recursive directory registrations.
;;; Used after directory create / rename events when the backend needs
;;; explicit child registration updates.
(watch-refresh watcher)              ;; => result<:void>
```

### Event semantics

The API should guarantee these normalization rules:

- saving a file through "write temp file + rename into place" produces a
  debounced `write` or `rename` batch for the logical target path, not a
  meaningless storm of temp-file noise
- duplicate backend writes for the same path inside the debounce window
  collapse to one event
- recursive watchers surface paths relative to the registered root
- backend queue overflow becomes an explicit `overflow` event rather than a
  silent drop

Clients remain responsible for policy. Example: notebook should still decide
that "any relevant event means re-render", while a build tool may choose to
ignore `attrib`.

---

## Example consumers

### tur-notebook

`tur nb render --watch` becomes:

1. call `watch-open` on the source file
2. render once
3. call `watch-drain` in a loop
4. on any non-empty batch, sleep through the debounce window, then re-render

No notebook-private `kqueue` / `inotify` code remains after the extraction.

### tur repl

`tur repl --watch` can watch `src/`, `tests/`, and `build.tur`, then queue
rebuild + reload when a debounced batch arrives.

### future build / test tools

`tur build --watch`, `tur run --watch`, or a docs preview server can all share
the same watcher and simply swap in different "what to do on a batch" logic.

---

## Implementation phases

- [ ] **WT0** -- Freeze the notebook watch behavior after `tur-notebook`
  reaches `notebook-v0.1.0`: document the exact event semantics notebook
  currently relies on (single-file watch, debounce window, fresh-session
  rerender behavior, atomic-save handling).

- [ ] **WT1** -- Create `spices/watch/build.tur`, public module skeletons, and
  the `watch-event` / `watch-opts` structs plus tests for constants and default
  options.

- [ ] **WT2** -- Extract the Linux backend from notebook's NB6 watch code into
  `backend_linux.tur`, preserving existing `inotify` semantics and validating
  file + directory events through targeted fixtures.

- [ ] **WT3** -- Extract the Darwin backend from notebook's NB6 watch code into
  `backend_darwin.tur`, preserving existing `kqueue` semantics and validating
  file replacement / rename-heavy editor saves.

- [ ] **WT4** -- Build the normalization layer in `watch/watch`: map backend
  events into `watch-event`, preserve root-relative paths, surface overflow,
  and hide backend flag differences from callers.

- [ ] **WT5** -- Build `watch/debounce`: burst-collapse, duplicate coalescing,
  and `watch-drain` batch semantics. Acceptance criterion: a simulated
  save-via-tempfile flow produces one stable debounced batch.

- [ ] **WT6** -- Add recursive directory watch support by enumerating tree
  contents at startup and refreshing registrations when directories are created
  or renamed. Keep policy conservative: correctness over minimal syscalls.

- [ ] **WT7** -- Refactor `tur-notebook` to depend on `tur-watch`; delete the
  watcher-specific inline-C from notebook; keep notebook behavior unchanged.

- [ ] **WT8** -- Add integration examples and docs: `README.md`, a short guide
  in `turmeric` docs if warranted, and examples demonstrating "tail events" and
  "rerun command on change".

- [ ] **WT9** -- Optional first follow-on adopter: wire `tur repl --watch`
  (from `spice-repl-plan.md`) to `tur-watch` instead of growing a second
  watcher implementation.

---

## Relationship to tur-notebook

The dependency direction after extraction should be:

```
tur-notebook  -->  tur-watch
```

Before WT7, notebook owns the watcher privately. After WT7:

- notebook imports `watch/watch` and `watch/opts`
- notebook keeps only its renderer-specific loop and rerender policy
- all OS watcher code moves under `spices/watch/`

This is the same extraction pattern proposed for `tur-ansi`: first let one real
application force the design, then promote the proven subsystem into its own
spice.

---

## Risks and open questions

1. **`kqueue` and recursive watch are awkward.** `kqueue` watches file nodes,
   not directory trees, so recursion means explicit child registration and
   refresh logic. That is acceptable for v0.1.0, but it should be called out
   as a complexity cost.

2. **Editor save behavior varies.** Some editors write in place; some rename a
   temp file over the original; some update permissions or timestamps
   separately. The debounce layer must be tested against at least the common
   atomic-save path notebook already expects.

3. **Overflow must be visible.** Silent event loss is worse than noisy behavior.
   If the backend queue overflows, clients need an explicit `overflow` event so
   they can fall back to full rebuild / rerender.

4. **Recursive watching can get expensive.** Large trees may mean many backend
   registrations. That is still better than every consumer inventing its own
   half-working watch loop, but the library should keep the implementation
   honest and measure cost on realistic spice trees.

5. **Cross-platform policy drift.** Linux and Darwin will not report identical
   raw signals. The library's job is to hide that difference behind a small,
   documented normalized event model. If a behavior cannot be normalized
   cleanly, the plan should prefer a narrower API over leaking backend details.

---

## Future follow-ons

Possible post-v0 work once the core watcher is shared:

- macOS `FSEvents` backend for lower-overhead deep tree watching
- Windows backend
- callback / async event-loop API
- ignore globs and path filters
- "run command on change" helper layer above the raw watcher
- integration into `tur run`, `tur build`, or docs live-preview tooling
