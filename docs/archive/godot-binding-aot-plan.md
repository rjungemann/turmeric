## Turmeric Godot Binding -- AOT Execution Mode Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-25
> **Type:** Integration / Game Engine -- spun out of
> [godot-language-binding-plan.md](./godot-language-binding-plan.md)'s
> Phase G2 leftover ("AOT mode behind a project setting; cache layout
> under `.godot/`").

---

## Why this is its own plan

The parent binding plan promised "both modes ship in v1" -- interpreter
and AOT. G2 landed interpreter only, and AOT did not land in G3 / G4
either because the substrate gap is wider than the original phase
estimate. This plan scopes out what landing it actually entails so
the work can be sized honestly.

The current interpreter mode is performant enough for the
paddle-pong-tur demo's gameplay loop. AOT is *the* steady-state
performance story, not a correctness one -- a script that runs
correctly in interpreter mode and a script that runs correctly in
AOT mode must produce identical observable behaviour. The interpreter
ships; AOT is the optimization.

---

## What the AOT path actually is

The interpreter path today: `TurmericScript::reload()` reads the
.tur source, hands it to `libturi`'s interpreter, and stores the
resulting `TuriEnv`. `cb_call(method)` looks up `method` in the env
and `turi_call`s the closure.

The AOT path target:

1. `TurmericScript::reload()` invokes `tur build --shared <project>`
   on a transient directory wrapping the script. Output: a per-script
   `.dylib` / `.so` / `.dll` in a project-local cache.
2. `dlopen` the resulting shared library.
3. For each lifecycle method the script declares (`_ready`,
   `_process`, ...), `dlsym` the mangled symbol
   (`<module>__<method>`) and bind it.
4. `cb_call(method)` dispatches by calling the bound function pointer
   directly, with arguments marshalled per the manifest-declared
   signature.

The compiler already does most of the heavy lifting:

- `tur build --shared <dir>` produces `lib.<ext>` plus `exports.manifest`.
- The manifest lists each export as
  `<mod>/<defn> -> <mangled> :: (:args) -> :ret`.
- Symbols are statically typed; the manifest is the schema.

---

## Required substrate work

### A1. Per-script transient project

`tur build --shared` requires a directory, not a single file. Each
.tur script needs to be staged into a transient directory under the
project cache:

```
<godot project>/.godot/turmeric-cache/
  <script-hash>/
    src/                 ;; copy of script.tur as a one-file module
    build.tur            ;; minimal manifest
    build/lib/lib.dylib  ;; compile output
    build/lib/lib.manifest
```

The hash keys on `(script source path, file mtime, source bytes,
compiler version)` so cache lookups skip rebuild on unchanged files.

### A2. Subprocess plumbing for `tur build --shared`

The TurmericLanguage GDExtension needs to know where to find the `tur`
binary. Resolution order (highest first):

1. `TUR_BIN` environment variable.
2. A project setting `turmeric/tur_binary`.
3. Default: `tur` on `PATH`.

Subprocess errors (compile failure, missing binary, signing issues)
surface as `Script::set_source_code` errors in the editor's bottom
panel via the existing diag sink.

### A3. dlopen + dlsym wrapper

A small C++ wrapper over `dlopen` / `dlsym` (Windows: `LoadLibrary` /
`GetProcAddress`) abstracted in `turmeric_runtime.cpp`. Tracks each
loaded `.dylib`'s handle so reloads can `dlclose` the previous
generation before opening the new one.

### A4. Manifest parser

The `exports.manifest` format is one line per export:

```
script/main -> script__main :: () -> :int
script/_ready -> script___ready :: () -> :void
script/_process -> script___process :: (:float) -> :void
```

A small parser inside `turmeric_script.cpp` converts each line into a
`{turmeric_name, mangled_symbol, arg_types, return_type}` record.
Bind by `dlsym(mangled_symbol)`.

### A5. Direct-dispatch marshalling

For each manifest-declared signature, marshal Godot `Variant` args
into the C ABI the AOT function expects, then marshal the C return
back to `Variant`. This is the opposite direction from the
interpreter path's `TuriValue <-> Variant` marshaller, but the same
primitive table -- the existing `variant_marshal.cpp` is the
template.

A four-arg `(int, float, cstr, int) -> int` AOT export needs to be
called via a runtime-typed thunk; libffi or a hand-rolled
type-dispatched calling sequence are both viable. libffi is the
faster path to working code.

### A6. `__tur_godot_script_entry` is *not* needed

The original parent plan posited a well-known `__tur_godot_script_entry`
symbol the script library would export. Now that the manifest gives
per-export metadata, that contract is redundant -- the manifest IS
the script entry. Drop the symbol.

The `(godot-export ...)` / `(godot-signal ...)` calls from script
top-level still need to run at load time to populate the script-level
inspector view. Two options:

- **Compile-time emit:** `tur build` recognizes the godot-* form set
  and emits a `__tg_init` symbol that, when dlsym'd + invoked at load
  with `g_reloading_script` set as today, calls the registered
  natives in the right order. Requires teaching the compiler about
  these forms (or having them ship in a turmeric-godot spice that
  exposes the symbols normally).
- **Bytecode interpret-at-load:** Keep the existing
  `(godot-export ...)` interpreter native, and run a small init
  shim (still using libturi's interpreter) over JUST the top-level
  forms at load time, before dlopen handoff. Body-level `(defn ...)`
  routes through AOT after.

Option (b) is the smaller change and probably the v1 landing -- only
hot-path methods become AOT, top-level metadata declarations stay
interpreted.

---

## Phases

### Phase A1 -- Cache + subprocess (~1 week)

- Build cache directory under `.godot/turmeric-cache/`.
- Subprocess wrapper for `tur build --shared`.
- Stage per-script transient project.
- Per-script `script-hash` keying + cache hit/miss.

### Phase A2 -- Manifest + dlopen (~1 week)

- `exports.manifest` parser.
- `turmeric_runtime.cpp` dlopen / dlsym wrapper with reload-safe
  handle tracking.
- A per-script vector of `{turmeric_name, mangled_symbol,
  arg_types, return_type}` records.

### Phase A3 -- Direct-dispatch marshalling (~2 weeks)

- libffi integration (vendor it, or use the system one when present).
- Variant -> typed-C arg marshalling per manifest entry.
- Typed-C -> Variant return marshalling.
- cb_call routes to AOT dispatch when the script is in AOT mode,
  falls back to interpreter when not.

### Phase A4 -- Init shim (~1 week)

- At AOT script load, run an interpreter-only pass over the script's
  top-level forms (the `(godot-export ...)` / `(godot-signal ...)`
  calls) to populate script-level metadata BEFORE dlopen.
- Skip the pass for scripts that already have their export/signal
  decls cached.

### Phase A5 -- Project setting + reload (~1 week)

- Project setting `turmeric/execution_mode = aot | interpreter`,
  default interpreter for editor Play / aot for export.
- Per-script `#mode` directive override at the top of the file.
- Editor reload flow: dlclose previous handle, rebuild + reload.

### Phase A6 -- Polish + docs (~1 week)

- Diag surfacing for build / link / dlopen failures.
- A throughput benchmark comparing the same hot script under
  interpreter vs AOT.
- `docs/guides/godot-binding-guide.md` AOT section.

Total: ~7 weeks of focused work. A3 (marshalling) is the load-bearing
risk; A1 / A2 / A4 / A5 are mechanical once it's stable.

---

## Risks

- **libffi cross-platform.** macOS arm64 has libffi out of the box;
  Windows x86_64 ships a vendored copy via msys2 typically; Linux has
  it. Vendoring is the safe v1 choice -- pin a known-working release.
- **Init-shim duality.** Two evaluation engines per script (interpreter
  for top-level, AOT for methods) means two debugger code paths. The
  debugger plan ([godot-binding-debugger-plan.md](./godot-binding-debugger-plan.md))
  needs to be aware.
- **`tur build` latency in the editor loop.** Default to interpreter
  for editor Play; AOT only on export. Caching keeps incremental
  rebuilds tight, but a cold build is still 1-3s.
- **Cache invalidation.** Compiler-version-tag in the hash so a `tur`
  upgrade forces a rebuild. Anything less risks ABI mismatches.
- **macOS code-signing.** The cache-built dylibs need to be ad-hoc
  signed at minimum, distribution-signed for an exported game. The
  workflow's code-signing skeleton lands once for the GDExtension's
  own dylib; the per-script dylibs additionally need to be signed at
  export time (or the project ships with AOT off).
- **Hot-reload + stale handles.** Currently the interpreter env can
  be torn down and rebuilt cleanly. AOT requires `dlclose` ordering
  and there is no portable way to "guarantee no pointers from this
  dylib are live anywhere." For v1 hot-reload is restricted to
  editor-stopped scripts (the parent plan already says so), so the
  AOT path enforces "no reload while attached."

---

## Out of Scope

- **Linker-time link-time optimization across scripts.** Each script
  compiles in isolation; cross-script call sites still route through
  Godot's signal / callable machinery.
- **Stripped-symbol dylibs for release builds.** v1 keeps the
  manifest-symbol pairing visible; size optimization is a follow-up.
- **Web export.** Tracked separately (the [wasm-spices plan](./wasm-spices-plan.md)).
- **Per-method JIT** -- not in the v1 picture; AOT is enough.

---

## Success Criteria

- A user can flip `turmeric/execution_mode = aot` in project
  settings, hit Play, and the paddle-pong-tur demo runs identically
  to interpreter mode.
- A microbenchmark (1M `_process` calls, no Godot work) shows AOT at
  least 5x faster than interpreter.
- Editor-time builds are cached -- a no-source-change rebuild is a
  no-op (no `tur build` subprocess invocation).
- Export-time build produces signed AOT dylibs the exported binary
  loads without entitlement issues.
