# FFI x spices: closing the gaps between the dynamic FFI and the package layer

> **Status:** proposed (2026-08-17). **Track:** post-v1, incremental --
> each phase is independently landable.
> **Builds on:** [jit-ffi-c2mir-plan.md](jit-ffi-c2mir-plan.md) (F1-F3
> implemented; F4/F5 trailing),
> [docs/guides/developing-spices-guide.md](../guides/developing-spices-guide.md),
> [docs/guides/ffi-guide.md](../guides/ffi-guide.md).

## 0. Summary

The dynamic FFI (JIT call thunks, thunk-backed extern-c, `call-ptr`) and
the spice layer (`:cmake-deps`, `:build-opts :link-libs`,
`:c-sources`/`:c-includes`, per-export `__ffi` shims) each work; this
plan is the survey of where they meet and don't. One genuine defect
(S1), one pre-planned marshalling gap that this work makes more visible
(S2), two phases that are cross-references into the jit-ffi plan's own
trailing phases (S3), and two small documentation/cleanup items (S4,
S5). Ordered by value.

## 1. What exists today (verified against the tree, 2026-08-17)

- A spice wraps a C library with `:cmake-deps` (fetch+build via CMake,
  flags recorded in `cmake/spice-deps-manifest.json`) and/or
  `:build-opts :link-libs`; `tur build`/`tur run` inject `-I/-L/-l`
  from the manifest (`collect_build_aux`, `src/main.c:1859`).
- Stdlib-style bare sources instead embed `/* __tur_autolink__: ... */`
  markers in inline-C; `scan_autolink_markers` (`src/main.c:1839`)
  collects them from the emitted C.
- The REPL loads a spice either by subprocess `tur build --shared` +
  dlopen, or -- in JIT builds -- in-process via the `TurSpiceJitHook`
  (`repl_jit_build`, `src/main.c:3823`).
- REPL calls into spice exports resolve through the FFI ladder
  (`src/turi/ffi_thunk.c`): JIT thunk (any signature) -> per-export
  `__ffi` shim -> generated shape table.

## 2. Phases

### S1 -- thread `:cmake-deps` / `:link-libs` flags into the REPL's in-process JIT hook

**The defect.** `repl_jit_build` collects only `__tur_autolink__`
markers (`src/main.c:3920-3922`) before handing the TU to
`tur_jit_compile_image`. A spice that declares its C dependency the
*recommended* way -- `:cmake-deps` or `:build-opts :link-libs`, with no
marker in any source -- compiles fine in-process, but nothing dlopens
the dependency, so `MIR_link`'s `dlsym(RTLD_DEFAULT)` resolver cannot
see its symbols and the load fails (or worse, resolves against an
unrelated homonym already in the process). The subprocess path is
unaffected because `tur build` injects the manifest flags into cc.

**Fix shape.** In `repl_jit_build`, call the same
`collect_build_aux`/`pkg_cmake_manifest_append_cc_flags` pair
`cmd_build` uses and append the collected `-L`/`-l` entries to the
autolink string handed to `tur_jit_compile_image`. Two engine-side
follow-ons in `jit_load_autolink` (`src/jit_engine.c:387`):

- It currently ignores `-L` entirely and probes `lib<name>.so` by
  soname only. Track `-L` dirs and try `<dir>/lib<name>.so` (and
  `.dylib` on macOS) before the bare soname.
- A static-only cmake dep (`.a`) cannot be dlopened; when resolution
  fails, fail the hook cleanly so the loader falls back to the
  subprocess path -- the behavior today's marker-less spices already
  get, minus the mystery.

**Test.** A fixture spice under `tests/turi/` with `:build-opts
{:link-libs ["-lm"]}` (universally present) whose export calls a libm
symbol the process does not otherwise pull in; assert it is callable at
the REPL with the JIT hook active.

### S2 -- variadic spice exports (cons-list marshalling)

`ffi_native_shim` still rejects variadic exports outright
(`src/turi/ffi_thunk.c`: "cons-list marshaling lands in a later RP") --
a promise predating the thunk engine. Thunks make this deliverable now:
a variadic call site *has* a concrete arity and class list at the
moment of the call, which is exactly a signature string -- render
"the fixed prefix + the walked rest-list classes", get a thunk, call.
No shape table entry could ever cover this; the thunk cache handles the
per-call-shape variety naturally. Non-JIT builds keep the clean
rejection.

### S3 -- struct-by-value and callbacks for spice APIs (cross-reference)

These are jit-ffi-c2mir-plan F4/F5, not new work, but the spice layer
is where they pay off; record the dependency here so neither plan
forgets the other:

- **F4 (struct-by-value)** retires the `'?'` class, which today makes a
  spice export returning/taking a small struct silently shim-less and
  REPL-uncallable. The signature registry F4 needs ("single source of
  truth shared between elaborator and turi") should be designed against
  the spice **exports manifest**, which already serializes per-export
  type information -- extending it with layout hashes is the natural
  transport.
- **F5 (callbacks)** is what lets a spice expose C APIs that take
  function pointers (`qsort`, reactor-style registrations, zmq socket
  monitors) with Turmeric closures from the REPL. Today's reactor
  spice-side workaround (fat-closure handles through `:int`) is the
  measure of the hole.

### S4 -- constants do not survive dynamic linking: make wrapping them a documented convention

`#define ZMQ_REP 4` never reaches a symbol table, so both the dynamic
FFI and REPL users of a spice must restate constants (see the libzmq
example in the FFI guide). No mechanism needed -- a spice should export
them as plain defns/defs next to its externs -- but the convention
belongs in developing-spices-guide.md's C-wrapping section, with a
sentence on keeping them adjacent to the `extern-c` block they belong
to so drift is reviewable.

### S5 -- shape-table retirement bookkeeping

jit-ffi-c2mir-plan section 2.4 demoted the generated table
(`tools/gen_ffi_dispatch.py`, `src/runtime/ffi_dispatch_thunk.c`) to
the non-JIT fallback rung. Once JIT builds are the default REPL
configuration, the table (and its generator, and the `--max-arity`
regeneration advice in the ladder's error message) can be deleted.
Blocked on the default flipping; record it so the 381-entry file does
not outlive its reason.

## 3. Explicitly not doing

- **A dlclose lifecycle for spice-loaded dependencies.** Autolinked and
  `dlopen`'d libraries stay mapped for the process lifetime, matching
  the interpreter's process-lifetime posture for closures and natives;
  spice `(reload)` already retires old images without unmapping. A
  library that must be unloaded mid-session is out of scope -- the
  thunk cache holds no per-target state, but user-held `dlsym` pointers
  dangle on dlclose and no marshalling layer can fix that.
- **Auto-generating extern-c declarations from C headers.** A binding
  generator is a tool, not a language feature, and c2mir's preprocessor
  is not a header-parsing service; revisit only if a spice author
  actually asks.
