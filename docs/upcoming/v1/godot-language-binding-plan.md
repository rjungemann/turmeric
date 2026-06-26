# Turmeric as a Godot 4 Scripting Language -- Plan

> **Status:** In progress -- G0-G3, G5, G6, G7 done; G4 partial
> (debugger deferred). defgodot-script ships the plan-shape
> :exports / :signals block surface; verified headless against
> stock Godot 4.x.
> **Last Updated:** 2026-06-25
> **Type:** Integration / Game Engine

## Implementation Status (2026-06-25)

Phase work has been landing in the sibling repo `../turmeric-godot/`.
Headless verification runs via
`/Applications/Godot.app/Contents/MacOS/Godot --headless --path . --script ...`.

| Phase | Status | Notes |
|---|---|---|
| G0 -- spike | done | ScriptLanguageExtension registered; .gdextension loads in stock Godot 4.3. |
| G1 -- hello node | done | Interpreter-mode only; `_ready` dispatches; primitive marshalling. |
| G2 -- lifecycle + inspector | done | All lifecycle hooks; `:exports` round-trip; `:signals` visible; AOT mode spun out into its own plan ([godot-binding-aot-plan.md](../../archive/godot-binding-aot-plan.md)). |
| G3 -- ClassDB coverage | done | classdb_proxy + curated prelude + extension_api.json codegen (Codegen v2 typed variants). |
| G4 -- editor niceties | partial | Syntax highlighter + completion landed; debugger spun out into its own plan ([godot-binding-debugger-plan.md](./godot-binding-debugger-plan.md)). |
| G5 -- polish + release | done | macOS / Linux / Windows CI workflow; paddle-pong-tur demo; docs guide. |
| G6 -- typing follow-ups | done | Typed signal-connect, class-hierarchy handles, curated one-shot prelude. See [archived plan](../../archive/v1/godot-binding-typing-followups-plan.md). |
| G7 -- `defgodot-script` shell | done | Plan-shape block surface: `(defgodot-script Name :extends Parent :exports ((n : T v) ...) :signals (bare-name (name (a : T) ...)) body...)`. Body walked at macro expansion via an accumulator-threaded helper (`tg-script-walk__`); `:exports`/`:signals` keywords consume the following list, every other form passes through. Multi-arg signals supported via a second accumulator helper (`tg-signal-acc__`) that folds `(arg : type)` decls into the flat `godot-signal` arg list. Per-decl `defgodot-export` / `defgodot-signal` remain available outside the block surface. Verified headless via three fixtures: `defgodot_script.gd` (legacy native calls), `defgodot_script_richer.gd` (per-decl macros), `defgodot_script_block.gd` (block surface, multi-arg signal `hit (damage : int) (impulse : float)`). Backed by `^syntax`, `type-ann-inner`, and structural-`=` on F_KEYWORD in `src/compiler/elab_macros.c`. |

Outstanding v1-scope work: **none.** G7 closed the
`defgodot-script` plan-shape surface this session. The body-walking
`:exports` / `:signals` block macro, plus multi-arg signal support,
landed in `../turmeric-godot/src/bridge/prelude.cpp` and is
headless-verified. The five `tur`-side language gaps the original
investigation surfaced are all closed (2026-06-25):

- `(quote sym)` produces a first-class `:Sym` literal --
  [archived report](../../archive/defgodot-script-macro-vec-quote-semantics.md).
- `^syntax` per-param marker landed (`& ^syntax body` hands the
  macro raw AST instead of pre-elaborated forms) --
  [archived report](../../archive/macro-args-elaborated-before-expansion.md).
- `elab_call` now treats an F_QUOTE-wrapped F_SYM in call-head
  position as the bare symbol, so `(list 'foo args...)` dispatches
  through the normal scope/special-form/macro path --
  [archived report](../../archive/list-macro-quote-vs-syntactic-symbol.md).
- Nested `[...]` literals inside macro args are navigable as AST
  (the homogeneity check only fires when a heterogeneous `[...]`
  reaches value position) --
  [archived report](../../archive/nested-vec-literals-collapse-to-runtime-vec.md).
- `type-ann-inner` / `type-ann?` CT primitives unwrap the
  `F_TYPE_ANN` node the reader emits for structural `name : type`
  annotations, so a macro can read the inner type symbol --
  [archived report](../../archive/ct-primitives-cannot-walk-type-ann-nodes.md).

**Spun out into their own plans:**

- **AOT execution mode** -- `tur build --shared` subprocess, dlopen,
  libffi marshalling. Multi-week scope. Lives in
  [godot-binding-aot-plan.md](../../archive/godot-binding-aot-plan.md).
- **In-editor debugger** -- `ScriptLanguageExtension::debug_*` +
  libturi substrate (frame stack, fiber-based eval, span tracking).
  Lives in [godot-binding-debugger-plan.md](./godot-binding-debugger-plan.md).

**Already landed in this session:**

- `defgodot-script` minimum-viable shell macro.
- `defgodot-export` / `defgodot-signal` ergonomic sub-macros in
  `../turmeric-godot/src/bridge/prelude.cpp`. Each lifts the
  per-decl `(godot-export "name" "type" default)` /
  `(godot-signal "name" ...)` ceremony into a `(name : type default)` /
  bare-symbol-or-`(name (arg : type)...)` shape, batched and
  variadic. `defgodot-signal` handles both zero-arg signals and
  signals with declared args (multi-arg flattened via
  `tg-signal-acc__`). Spike fixture:
  `examples/spike/scripts/defgodot_script_richer.{tur,gd}` --
  headless-verified PASS against stock Godot 4.x (macOS arm64).
- `defgodot-script` block surface
  (`tg-script-walk__` body walker in the same prelude) -- accepts
  `:extends` / `:exports` / `:signals` keyword-led blocks
  interleaved with normal body forms, expands to a `(do ...)` of
  the lowered `godot-export` / `godot-signal` calls plus
  pass-through forms. Spike fixture:
  `examples/spike/scripts/defgodot_script_block.{tur,gd}` --
  headless-verified PASS (also asserts the multi-arg
  `hit (damage : int) (impulse : float)` signal registered with
  the right arg names).
- `type-ann-inner` / `type-ann?` CT primitives in
  `src/compiler/elab_macros.c` (this repo). Unwraps the
  `F_TYPE_ANN` node the reader emits for structural `name : type`
  so macros can read the inner type symbol; lets `defgodot-export`
  consume the plan-shape `(name : type default)` decls directly.
  Fixture: `tests/fixtures/macro-type-ann-walk/`. See
  [archived report](../../archive/ct-primitives-cannot-walk-type-ann-nodes.md).
- Build hygiene in `../turmeric-godot/`:
  `src/aot/aot_metadata.cpp` swapped its two `try/std::stoll` blocks
  for `std::strtoll` with `endptr`, since `godot-cpp` builds with
  `-fno-exceptions` and the AOT sources were added to
  `SConstruct`'s glob locally. Unblocks `scons platform=macos
  arch=arm64 target=template_debug` clean.
- String arena reallocation hazard -- switched `g_str_arena` from
  `std::vector` to `std::deque` for pointer stability across pushes.
- macOS code-signing skeleton in `.github/workflows/build.yml` --
  ad-hoc signing always; distribution signing + notarization gated
  on the certificate secrets being configured in the repo.

---

## Overview

This plan scopes out making Turmeric usable as a scripting language inside
Godot 4 -- not "Godot bindings for Turmeric programs that embed the engine,"
but the inverse: a Godot editor user can attach a `.tur` script to a `Node`,
edit it inline (or in their preferred editor), hit Play, and have it drive
gameplay just like GDScript or C#.

Two delivery paths exist in Godot 4. They differ enough to be worth choosing
explicitly:

1. **GDExtension (`ScriptLanguageExtension`)** -- the modern path. Ships as a
   `.gdextension` shared library; users drop it into `addons/` and it appears
   alongside GDScript. **No engine recompile.** Works with stock Godot
   binaries from godotengine.org. This is the path Godot itself recommends
   for new languages today (the old `ScriptLanguage`-subclass-as-module path
   the user's brief describes is still supported but is the legacy route).
2. **Engine module (custom `ScriptLanguage`)** -- statically compiled into a
   fork of the engine via SCons. Lower call overhead, full access to internal
   APIs, but every user needs our custom Godot build. Useful as a fallback
   only if GDExtension's surface turns out to be insufficient.

**This plan targets GDExtension.** Module-mode is documented as a contingency
in "Risks" but is not the primary track.

---

## Goals / Non-Goals

### Goals (v1)

- A `turmeric-godot` repo (sibling to `turmeric-spices`) that builds a
  `.gdextension` for macOS (arm64), Linux (x86_64), and Windows (x86_64).
- `.tur` files are first-class scripts in the Godot editor:
  - Attach to a node via "Attach Script... -> Turmeric."
  - Inspector shows exported variables declared in the script.
  - Signals declared in Turmeric appear in the Node dock's Signals tab.
  - `_ready`, `_process(dt)`, `_physics_process(dt)`, `_input(event)`
    lifecycle hooks are called.
- Turmeric scripts can:
  - Call any Godot ClassDB method (`(node/get-position self)`,
    `(node/queue-free self)`).
  - Receive and emit signals.
  - Read/write `@export`-style properties from the inspector.
- One end-to-end demo project ("paddle-pong-tur") in the repo's `examples/`,
  driven entirely by Turmeric scripts, runs in a stock Godot 4.3 editor.

### Non-Goals (v1)

- **No in-editor REPL** inside Godot. The standalone `tur repl` is enough.
- **No hot-reload of running scenes.** Edit -> Stop -> Play is fine for v1.
  Editor-time reload of stopped scripts is in scope; runtime patching is not.
- **No Turmeric-side type-checked Godot API.** v1 ships an untyped facade
  (everything is `:Variant` / `:int` handle); a typed layer (generated from
  Godot's `extension_api.json`) is a follow-up.
- **No Android / iOS / web-export targets.** Desktop only for v1. (Web is
  blocked on the [wasm-spices plan](./wasm-spices-plan.md) landing first.)
- **No visual scripting.** Text scripts only.
- **No `.NET`-style cross-language inheritance** (a GDScript class extending
  a Turmeric class). One-way is fine: Turmeric extends Godot built-ins.

---

## How Turmeric Code Reaches Godot

Turmeric compiles to C, then to a native shared library. The GDExtension
already *is* a shared library loaded by Godot. The shapes line up:

```
  user-script.tur
        |
        v  (tur build --shared)
  user-script.so / .dylib / .dll
        |
        v  (dlopen'd at editor load by the
            turmeric-godot GDExtension shim)
  Godot ScriptLanguageExtension -- compiles/loads/runs Turmeric scripts
```

The GDExtension is *not* a single static binary that ships every user's
script. It ships:

1. The **language shim** -- a thin C++ `ScriptLanguageExtension` that knows
   how to invoke the Turmeric compiler, load the resulting `.so`, and route
   Godot lifecycle calls into Turmeric entry points.
2. The **Turmeric runtime** -- `libturi`-equivalent (the same support code
   the AOT-compiled output already needs). Statically linked into the
   GDExtension so user scripts dlopen against it.

User `.tur` files are compiled on demand by the shim (invoking `tur build
--shared` as a subprocess, or via an embedded `libturi` for interpreter
mode). Output `.so` files cache under `<project>/.godot/turmeric-cache/`.

**Two execution modes**, gated by a project setting:

- **AOT (default)**: shim shells out to `tur build --shared <script.tur>`,
  dlopens the result. Fast steady-state, slower edit-compile-play cycle.
- **Interpreter**: shim embeds `libturi` and runs the AST. Instant reload,
  slower per-call. Useful for jam-style fast iteration.

Both modes ship in v1; the shim picks one per script via a `#lang` hint or
project default.

---

## Architecture

### Repository layout

```
turmeric-godot/                          (new repo, sibling to turmeric-spices)
  README.md
  src/
    extension.cpp                        ;; GDExtension entry point
    turmeric_language.{h,cpp}            ;; ScriptLanguageExtension subclass
    turmeric_script.{h,cpp}              ;; ScriptExtension subclass (one per .tur file)
    turmeric_instance.{h,cpp}            ;; ScriptInstanceExtension (one per Node attachment)
    turmeric_runtime.{h,cpp}             ;; embeds libturi, dlopens AOT .so files
    bridge/
      variant_marshal.{h,cpp}            ;; Variant <-> Turmeric value
      classdb_proxy.{h,cpp}              ;; Godot ClassDB method dispatch
      signal_router.{h,cpp}              ;;
  godot-cpp/                             ;; submodule, pinned to a 4.x release
  spice/
    build.tur                            ;; turmeric-side facade spice
    src/godot/                           ;; (node/get-position ...), etc.
    examples/paddle-pong-tur/
  bin/                                   ;; built .gdextension + libs land here
  SConstruct                             ;; godot-cpp's stock SCons setup
  turmeric-godot.gdextension             ;; manifest pointed at bin/
  tests/                                 ;; headless Godot --script tests
```

The **spice** (`spice/`) is the Turmeric-facing half: a normal `turmeric-spices`
entry that exports the `(godot/...)` namespace. It depends only on the C
shims emitted by the GDExtension's runtime.

### Lifecycle: script attachment

1. User clicks "Attach Script..." in Godot, picks `player.tur`.
2. Godot asks the registered languages "who handles `.tur`?" -- our
   `TurmericLanguage` answers.
3. `TurmericLanguage::create_script()` returns a new `TurmericScript`
   pointing at `player.tur`.
4. `TurmericScript::reload()` invokes either:
   - AOT path: `tur build --shared --build-dir .godot/turmeric-cache/
     player.tur` -- on success, dlopens the resulting `.so`, looks up the
     well-known `__tur_godot_script_entry` symbol, calls it to get a script
     descriptor (exports, signals, lifecycle table).
   - Interp path: `turi_load_file("player.tur")` against the embedded
     interpreter, runs top-level forms, harvests the same descriptor from a
     conventional global.
5. The descriptor populates Godot's view of the script: exported props
   visible in the inspector, signals visible in the Node dock, method list
   queryable via `Script::get_method_list()`.

### Lifecycle: per-node instance

1. The node enters the scene; Godot asks `TurmericScript` for a
   `ScriptInstance`.
2. `TurmericInstance` allocates a Turmeric-side instance value (a struct
   handle, opaque to Godot) and stores the mapping `Godot Object* <-> tur
   handle`.
3. Lifecycle calls (`_ready`, `_process(dt)`) become calls into the script's
   exported entry table with the Turmeric handle as the first argument.
4. Property reads/writes route through `variant_marshal` (Godot
   `Variant` <-> a tagged Turmeric value).
5. Outbound calls from Turmeric (`(node/queue-free self)`) hit
   `classdb_proxy`, which looks up the method via Godot's `MethodBind`
   table and invokes it with marshalled args.

### Script-side surface (the spice)

A Turmeric script looks like:

```turmeric
;; player.tur
(defgodot-script Player :extends Node2D
  :exports ((speed   : float 200.0)
            (texture : Texture2D))

  :signals ((hit (damage : int))
            (died))

  (defmethod _ready [self] : void
    (println "Player ready"))

  (defmethod _process [self dt : float] : void
    (let [pos (node/get-position self)
          dx  (* (* speed dt) (input/axis "ui_right" "ui_left"))]
      (node/set-position self (vec2-add pos (vec2 dx 0.0)))))

  (defmethod take-damage [self amount : int] : void
    (emit-signal self :hit amount)))
```

`defgodot-script` is a macro that expands to:

- A `defstruct` for `Player`'s instance state.
- A descriptor table consumed by `__tur_godot_script_entry`.
- Lifecycle method shims with the correct C signature for the GDExtension to
  call through.

The macro lives in the spice (`spice/src/godot/script.tur`) and is the only
new piece of language-side surface required.

---

## Open Design Questions

1. **Interpreter vs AOT default.** AOT is faster at runtime but pays a
   compile cost on every edit-Play cycle. Default proposal: AOT for `release`
   export, interpreter for editor Play.
2. **`Variant` marshalling for HAMTs / vecs.** Godot has `Dictionary` and
   `Array`. Cheap shallow conversion (copy on boundary) is fine for v1; a
   zero-copy proxy is a follow-up.
3. **Threading.** Godot calls `_process` on the main thread; long-running
   Turmeric work would block. Out of scope for v1 -- document the constraint.
4. **Where does `(spawn ...)` go?** Probably disabled at the Godot boundary
   (the engine owns threading); revisit when v1 ships.
5. **Error reporting.** Compile errors need to surface in Godot's debugger
   panel, not just the terminal. Plumbing: capture `tur build` stderr,
   parse the existing `TUR-E####` format, push as `ScriptError` entries.

---

## Phases

### Phase G0 -- Spike (1-2 weeks)

- Stand up `turmeric-godot/` with `godot-cpp` submodule, SCons build.
- Implement a no-op `ScriptLanguageExtension` that registers `.tur` and
  prints to stdout on every callback Godot makes.
- Goal: prove the loader, the registration path, and the cross-platform
  build all work before touching language semantics.

### Phase G1 -- "Hello, Node" (2-3 weeks)

- Embed `libturi`; load `.tur` files in interpreter mode.
- Implement `_ready` dispatch only (no `_process`, no properties, no
  signals).
- Marshal `int`, `float`, `bool`, `String` between `Variant` and Turmeric.
- Ship the `(println ...)` -> Godot output panel route.
- Demo: attach a script to a Node, hit Play, see "hello" in the output.

### Phase G2 -- Lifecycle + Inspector (3-4 weeks)

- Full lifecycle: `_ready`, `_process`, `_physics_process`, `_input`,
  `_unhandled_input`, `_notification`.
- `:exports` -> inspector properties (round-trip read/write).
- `:signals` -> Node dock; `(emit-signal ...)` works.
- AOT mode behind a project setting; cache layout under `.godot/`.
- Demo: paddle-pong, 1P, no audio.

### Phase G3 -- ClassDB Coverage (3-4 weeks)

- `classdb_proxy` calls **any** ClassDB method by name with marshalled args.
- Hand-curated `(godot/...)` facade for the ~30 most-used types
  (`Node2D`, `Sprite2D`, `CollisionShape2D`, `Input`, `RigidBody2D`, ...).
- Codegen path from `extension_api.json` for the long tail (untyped in v1).
- Compile-error surfacing in the debugger panel.

### Phase G4 -- Editor Niceties (2-3 weeks)

- Syntax highlighter (`SyntaxHighlighterExtension`) -- minimal token-coloring.
- Code completion (`ScriptLanguageExtension::complete_code`) -- defer to
  `tur check` for symbol info; v1 returns top-level binds only.
- Debugger: breakpoints + stack frames via `ScriptLanguageExtension::debug_*`.
  Stretch for v1; if it slips, ship without it and document the gap.

### Phase G5 -- Polish + Release (2 weeks)

- macOS / Linux / Windows release binaries via GitHub Actions.
- `paddle-pong-tur` demo polished; recorded gif in the README.
- Docs page under `docs/guides/godot-binding-guide.md` (in *this* repo)
  with quickstart, the `defgodot-script` reference, and known gaps.
- Announce alongside the v1 cut.

Total: roughly **3-4 months** of focused work for one developer. Phases G0-G2
are the load-bearing risk; G3-G5 are mostly grinding through API surface.

---

## Risks

- **GDExtension surface gaps.** A `ScriptLanguageExtension` that lives in a
  shared library is younger than the in-tree-module path; some lifecycle
  hooks may not be exposed at the GDExtension boundary yet. Mitigation:
  Phase G0 is specifically a spike to find these. Fallback: build a custom
  Godot fork as an engine module (the path described in the user's brief).
  Painful for distribution but technically straightforward and well
  documented.
- **AOT compile latency in the editor loop.** Mitigation: interpreter mode
  for editor Play; AOT only on export.
- **`libturi` lifetime / leak posture under Godot.** The interpreter
  intentionally never frees process-lifetime closures (see CLAUDE.md). Godot
  starts and stops scripts repeatedly within one editor session; we'll need
  a per-script arena reset, or ASan will scream. Plan: a `turi_reset()`
  entry point added alongside this work.
- **godot-cpp ABI churn.** Godot 4 is still moving; pin a specific 4.x
  release and let users on newer versions wait for a rebuild.
- **Cross-platform code signing on macOS** (the `.dylib` inside a
  `.gdextension` needs to be signed for distribution). Handled in Phase G5.

---

## Out of Scope (Tracked Elsewhere / Later)

- **Typed Godot facade** -- a `:Node2D` etc. with real `defopaque` types
  and typed method signatures, generated from `extension_api.json`. Worth a
  follow-up plan after v1 ships.
- **Web export.** Blocked on [wasm-spices-plan.md](./wasm-spices-plan.md);
  Godot's web export uses its own Emscripten toolchain that would need to
  agree with ours.
- **Mobile (Android / iOS).** Godot supports both via GDExtension but the
  toolchain story is harder; defer.
- **Visual debugger inspector for Turmeric values.** v1 shows them stringly;
  a real expander is later.
- **Cross-language inheritance** (GDScript class extending Turmeric class).

---

## Success Criteria

- A user with stock Godot 4.3 can:
  1. Drop `turmeric-godot.gdextension` + binaries into `addons/`.
  2. Right-click a node, "Attach Script", pick "Turmeric", get a working
     template.
  3. Edit the `.tur` file in their editor of choice, save, hit Play in
     Godot, and see the script run.
  4. Export the project for desktop and ship a binary that runs without
     Turmeric installed on the target machine.
- `examples/paddle-pong-tur` runs end-to-end with no GDScript.
- CI builds the extension for three desktop platforms on every push.
