# Turmeric as a Godot 4 Scripting Language -- Binding Guide

> **Status:** v1 in progress (Phase G3+G4 MVPs shipped)
> **Plan:** [docs/upcoming/v1/godot-language-binding-plan.md](../upcoming/v1/godot-language-binding-plan.md)
> **Repo:** `../turmeric-godot/` (sibling to this one)

This guide is for people who want to attach `.tur` scripts to Godot
nodes and have them drive gameplay. It covers install, the curated
facade, the codegen'd ClassDB facade, the editor plugin, the typed
`godot-call` variants, and the known gaps you'll trip over while the
binding matures.

The plan in `docs/upcoming/v1/` is the source of truth for *what's
coming*; this guide is the source of truth for *what works today*.

---

## Install

The binding ships as a GDExtension built from the
[`turmeric-godot`](https://github.com/rjungemann/turmeric-godot) repo.
Two pieces drop into your project:

1. **`bin/`** -- the per-platform GDExtension shared libraries plus
   `turmeric-godot.gdextension`. Drop in `<your-project>/bin/`.
2. **`addons/turmeric-godot-editor/`** -- the `@tool` GDScript plugin
   that registers the editor-side syntax highlighter. Drop in
   `<your-project>/addons/turmeric-godot-editor/`, then enable in
   *Project Settings -> Plugins*.

Without (1) the language doesn't register. Without (2) `.tur` files
open in the script editor uncolored but otherwise work.

For development, build from source with SCons:

```sh
cd turmeric-godot
python3 -m SCons platform=macos arch=arm64 target=template_debug -j4
```

The build produces a framework under
`examples/spike/bin/libturmeric-godot.macos.template_debug.framework/`
ready for the bundled `examples/spike/` test project.

---

## Hello, Node

Attach `player.tur` to a `Node2D`:

```turmeric
(defn _ready []
  (godot-println "[player] ready"))

(defn _process [delta : float]
  (let [self (node/self)
        pos  (node/get-position self)]
    ;; delta is :float; vec2 components are :float; full type checking
    (node/set-position self (godot-vec2 (+ (node/vec2-x pos) 1.0)
                                        (node/vec2-y pos)))))
```

Hit Play and the node drifts right one unit per frame. The
`_ready`/`_process` callbacks dispatch through the script-instance
shim Godot sees as a normal script; the engine doesn't know anything
unusual is happening.

---

## The two facades

Script-side names come from two sources, evaluated in order at every
script load:

1. **`bridge/prelude.{h,cpp}`** -- a small, hand-written curated
   facade under the `node/` prefix. Covers the most common Node
   methods (`node/get-name`, `node/set-position`, `node/get-modulate`,
   ...) and the value builders / accessors (`node/vec2`,
   `node/vec2-x`, `node/color`, `node/rect2-w`, `node/get-class`,
   ...).

2. **`bridge/generated_facade.{h,cpp}`** -- a class-prefixed facade
   generated from `godot-cpp/gdextension/extension_api.json` by
   `tools/gen_godot_facade.py`. Today ships wrappers for an
   allow-listed 15 classes (~672 methods): Node, Node2D, CanvasItem,
   Sprite2D, Area2D, RigidBody2D, StaticBody2D, PhysicsBody2D,
   CollisionShape2D, Input, Viewport, SceneTree, Timer,
   AnimationPlayer, Object.

The generated names are `classname/method-name` (kebab-case methods,
lowercase class prefix): `node2d/set-skew`, `sprite2d/is-centered`,
`canvasitem/is-y-sort-enabled`. The prelude wins on collision; method
names the prelude owns are skipped by the generator (`set_position`,
`get_modulate`, `get_name`, etc.).

To grow the generated coverage, add a class name to `ALLOWLIST` in
`tools/gen_godot_facade.py` and re-run.

---

## Typed `godot-call` variants

Every method dispatches through one of five typed natives. The codegen
picks the variant per JSON return type; you can pick manually too:

| native         | TUR_NRT  | JSON return                    | wrapper type |
|----------------|----------|--------------------------------|--------------|
| `godot-call`   | `INT`    | everything else                | `:int`       |
| `godot-call-v` | `VOID`   | (no `return_value`)            | (none)       |
| `godot-call-f` | `FLOAT`  | `"float"`                      | `:float`     |
| `godot-call-b` | `BOOL`   | `"bool"`                       | `:bool`      |
| `godot-call-c` | `CSTR`   | `"String"` / `"StringName"` / `"NodePath"` | `:cstr` |

The plain `:int` variant covers Object handles, arena handles
(Vector2/Vector3/Color/Rect2/Transform2D/Transform3D/Array/Dictionary
all live in the per-call Variant arena and surface as tagged
`:int`), and `Variant`-typed returns. Distinguishing those at
compile time would need `defopaque` newtypes the v1 codegen doesn't
emit yet.

---

## Value marshalling

Arena-backed types come back as a tagged `:int` handle (high bit set
so it can't collide with a plain int or an Object pointer). Build
them with the builders, decompose with the accessors:

```turmeric
;; Vector2 round-trip
(let [pos (godot-vec2 100.0 50.0)
      x   (godot-vec2-x pos)
      y   (godot-vec2-y pos)]
  ...)

;; Color
(godot-color 0.5 0.25 0.75 1.0)
(godot-color-r c)   ;; -> :float

;; Array (read-only for v1)
(godot-array-len arr)        ;; -> :int
(godot-array-get arr i)      ;; -> marshalled element

;; Dictionary (cstr keys for v1)
(godot-dict-has d "key")     ;; -> :bool
(godot-dict-get d "key")     ;; -> marshalled value
```

Handles are valid for the duration of the **outer `cb_call`** (one
script method invocation). They're reclaimed when control returns to
Godot. Don't stash a handle in a global expecting it to survive
across calls -- the next call's arena lookup will see garbage and
print a "stale arena handle" warning.

String returns go through a parallel string arena with the same
lifetime: `(node/get-name self)` is valid for the rest of the current
method; copy if you need it longer.

---

## Inspector + signals

```turmeric
(godot-export "speed"  "float" 200.0)         ;; appears in the inspector
(godot-export "active" "bool"  1)
(godot-signal "hit" "damage" "int")            ;; appears in the Signals tab

(defn _ready []
  (let [s (godot-prop-get "speed")]
    (godot-println s)))

(defn take-damage [amount : int]
  (emit-signal "hit" amount))
```

Inspector property values are read with `godot-prop-get` and written
with `godot-prop-set`. Both are dynamic (`:int`-typed by the
elaborator) because the property type varies per name; type your
handle at the use site if you need it stricter.

---

## Editor wiring

The `addons/turmeric-godot-editor/` plugin is a six-line
`EditorPlugin` that instantiates `TurmericEditorSyntaxHighlighter`
and registers it with the Script Editor. Theme integration is
automatic via `_update_cache` -- the highlighter pulls
`text_editor/theme/highlighting/{keyword,comment,string,number,
symbol,text}_color` from `EditorSettings` whenever the theme changes.

Code completion is wired through `_complete_code` (the standard
ScriptLanguageExtension hook). It collects symbol names by scanning
the user's source plus the prelude plus the generated facade, filters
by the in-progress prefix, and dedupes (user defs win). No live
interpreter introspection -- can't see runtime-only bindings, but
catches the vast majority of useful completions cheaply.

---

## Validation

`_validate` spins up a throwaway `TuriEnv`, installs a TLS diag-sink
collector, runs `turi_eval`, and returns the standard Godot
completion-result Dictionary:

```
{
  "valid":      bool,
  "errors":     [{ "path", "line", "column", "message" }, ...],
  "warnings":   [...],
  "functions":  [],
  "safe_lines": PackedInt32Array()
}
```

The error message gets the `TUR-E####` code prefixed. Notes/help drop
in v1; only errors and warnings reach the editor's error list panel.

---

## AOT execution mode

The interpreter path runs every script body through libturi at call time.
That's plenty for editor-time iteration but leaves throughput on the
table. The AOT path compiles each script to a shared library via
`tur build --shared`, dlopens it, and routes `cb_call` directly through
the dlsym'd function pointer. Same observable behaviour either way --
AOT is the optimisation, not a separate language.

Plan reference: [docs/upcoming/v1/godot-binding-aot-plan.md](../upcoming/v1/godot-binding-aot-plan.md).

### Opting in

Three ways to flip a script (or the whole project) to AOT. Highest wins:

1. **`TURMERIC_GODOT_AOT` env var** -- dev-loop override. Values
   `1`/`true`/`aot` enable; `0`/`false`/`interpreter` disable. Set this
   when you want one editor session in AOT without touching
   `project.godot`.
2. **`#mode <mode>` directive at the top of the .tur file** -- the
   per-script knob. Tolerates leading blank lines, `;` comments, and a
   `#lang sweet-exp` line above it:

   ```turmeric
   #lang sweet-exp
   #mode aot

   defn _ready []
     godot-println("AOT-compiled")
   ```
3. **`turmeric/execution_mode` project setting** -- string enum
   (`interpreter` / `aot`). Set under *Project -> Project Settings ->
   Advanced -> Turmeric -> Execution Mode*.
4. **Default**: `interpreter`.

A second project setting, `turmeric/tur_binary`, overrides the path to
the `tur` compiler. Precedence for that is `TUR_BIN` env > project
setting > `tur` on `PATH`.

### What gets AOT'd, what falls back

`cb_call` tries AOT first. It dispatches when:

- The method is declared `(defmodule script :exports [...])` *or* lives
  at top-level (the `"_"` module the dispatcher matches against).
- Every arg/ret type is one of `:void`, `:bool`, `:int` (any width),
  `:cstr`, `:float` / `:float32` / `:float64`.
- The arity matches the manifest exactly.
- The call is not variadic.

Everything else (`Ptr`/`Any`/struct types, mismatched arity, variadic,
methods not in the manifest) falls through to the interpreter path
silently -- so curated facade calls (`node/set-position`, ...) and
methods you didn't AOT-export keep working unchanged.

### Cache layout

```
<godot_project>/.godot/turmeric-cache/<fnv64-hash>/
  build.tur                       ;; staged defpackage
  build.log                       ;; stdout/stderr of tur build
  src/<module>.tur                ;; verbatim copy of the script source
  build/lib/lib<pkg>.so           ;; the dlopen'd shared library
  build/lib/lib<pkg>.so.manifest  ;; one line per export, see below
  exports.metadata                ;; inspector exports + signals (A4 sidecar)
```

The hash mixes `(script abs-path, source bytes, tur realpath + mtime)`,
so any of those changing forces a rebuild. A `.gitignore` is dropped in
the cache root automatically.

Manifest line format (from `tur build --shared`):

```
mod/name -> mangled_symbol :: (:t1 :t2 ...) -> :ret
```

Two reload paths:

- **Cold / source changed** -- run the interp prelude + facade + source
  pass (which populates `(godot-export ...)` / `(godot-signal ...)`
  decls), invoke `tur build --shared`, dlopen, then persist the
  inspector exports + signals into `exports.metadata`.
- **Warm cache** -- when `lib.so`, the manifest, and `exports.metadata`
  all exist, skip the interp eval entirely: restore the export/signal
  list from the sidecar and dlopen the image. The Output panel logs
  `[turmeric res://… AOT] fast-path: N exports + …`.

### Microbenchmark

`examples/aot-bench/` from the turmeric-godot repo runs
`hot(x) -> (+ x 1)` one million times. Compare two headless runs:

```sh
/Applications/Godot.app/Contents/MacOS/Godot --headless --path examples/aot-bench --quit
TURMERIC_GODOT_AOT=1 /Applications/Godot.app/Contents/MacOS/Godot \
    --headless --path examples/aot-bench --quit
```

Each run prints `mode=… iters=… ns_per_call=…`. Plan target: AOT >= 5x
faster than interpreter at this workload.

### Troubleshooting

Watch the Godot Output panel; every AOT diagnostic prefixes
`[turmeric res://path/to/script.tur AOT] `.

- **`tur build --shared failed (exit N): ...`** -- the staged compile
  rejected your source. Read the full log at
  `<cache>/<hash>/build.log`.
- **`fast-path: image load failed (... dlopen failed for ...)`** -- the
  cached `.so` mismatches the loader; usually fixed by removing
  `.godot/turmeric-cache/<hash>/` and reloading.
- **`fast-path: metadata read failed (...)`** -- the sidecar is from
  an older binary or got truncated; the slow path will rebuild and
  rewrite it.
- **Stuck in interpreter mode** -- env var beats everything; check
  `TURMERIC_GODOT_AOT` isn't set to `0`/`false`. Then check the
  `#mode` directive, then the project setting.

### Reload + hot-reload

The AOT image owns a `dlopen` handle, dropped before the next image
loads -- generations never overlap. v1 restricts AOT hot-reload to
editor-stopped scripts (matches the parent plan's hot-reload story);
there is no runtime guard against reload during a live call because
Godot only fires `Script::_reload` outside Play.

### Not yet

- Variadic dispatch.
- `Ptr` / opaque-handle / struct args and returns.
- Cross-script (one AOT script calling another's defn).
- Export-time signed dylibs.
- Per-method JIT -- AOT is the v1 story; JIT is not in the v1 picture.

---

## Known gaps

1. **No debugger.** Phase G4.3 (`debug_*` hooks) was called "stretch"
   in the plan and didn't ship. Breakpoints / stepping / stack frames
   need an interpreter-pause story libturi doesn't have today.

2. **Eval-mode unknown calls defer to runtime.** A typo in a method
   name passes `_validate` and only fires `TURI_ERROR` when the line
   is actually executed. Tracked in
   [docs/reported/eval-mode-unknown-call-deferred-to-runtime.md](../reported/eval-mode-unknown-call-deferred-to-runtime.md).

3. **No Object/arena handle distinction at the type level.** Both
   are `:int` to the elaborator. Mistyped opaque args (passing a
   Vector2 handle where an Object is expected) are caught at runtime
   via `(godot-call)`'s shape check, not at compile time. A
   `defopaque NodeHandle` / `defopaque Vec2Handle` pass would tighten
   this.

4. **Inspector property types limited to `float`/`int`/`bool`/`string`**.
   Vector2 / Color / Resource refs are next.

5. **No Android / iOS / web export.** Desktop only for v1; web is
   blocked on [`wasm-spices-plan.md`](../upcoming/wasm-spices-plan.md).

6. **`signals.gd` driver in `examples/spike/` fails to parse** as a
   GDScript file (type inference complaint on `var sigs := ...`).
   Pre-existing; one-line `var sigs: Array =` fix waiting to be
   landed.

7. **Generated facade allowlist is 15 of 920 classes.** Tactical
   growth as demos demand.

---

## Quickstart for a new game

1. `git clone https://github.com/rjungemann/turmeric-godot ../turmeric-godot`
2. `(cd ../turmeric-godot && python3 -m SCons platform=macos arch=arm64 target=template_debug -j4)`
3. Copy `examples/spike/bin/` and `examples/spike/addons/` into a
   fresh Godot project.
4. Enable the addon in *Project Settings -> Plugins*.
5. Right-click a node, *Attach Script...*, set language to Turmeric,
   pick a `.tur` path.
6. Edit the script in your editor of choice; save; hit Play in Godot.

The headless smoke test from the spike repo doubles as a quick
"is everything wired?" check:

```sh
cd ../turmeric-godot/examples/spike
/Applications/Godot.app/Contents/MacOS/Godot --headless --path . --script scripts/classdb_call.gd
```

Should print `[classdb_call.gd] all assertions passed`.
