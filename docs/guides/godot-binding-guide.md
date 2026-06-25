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
