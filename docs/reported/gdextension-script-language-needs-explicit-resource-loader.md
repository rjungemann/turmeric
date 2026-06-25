# Godot GDExtension: ScriptLanguageExtension does not auto-register a ResourceFormatLoader

> **Status:** Reported (informational; this is a Godot/godot-cpp gap, not a turmeric bug)
> **Severity:** Low (workaround known; affects turmeric-godot only)
> **Found by:** G1 slice of `turmeric-godot` -- trying `load("res://hello.tur")`
> **Date:** 2026-06-24

## Summary

Registering a `ScriptLanguageExtension` via
`Engine::register_script_language(...)` does **not** also register a
`ResourceFormatLoader` for the language's file extension. So a GDScript like:

```gdscript
var s = load("res://scripts/hello.tur")   # null + "No loader found for resource"
```

...fails with `ERR_FILE_UNRECOGNIZED`. The language is registered (you can
instantiate `TurmericScript` via `ClassDB.instantiate("TurmericScript")` and
its callbacks fire) -- but the *resource pipeline* never routes `.tur` to it.

This isn't a turmeric bug -- it's a piece of Godot's scripting plumbing
that an embedder has to do explicitly. Logging here so the turmeric-godot
plan reflects reality.

## What the language binding has to add

Beyond `ScriptLanguageExtension`, a `ResourceFormatLoaderExtension`
subclass that:

- `_get_recognized_extensions()` returns `["tur"]`.
- `_handles_type("Script")` returns `true`.
- `_load(path, original_path, use_sub_threads, cache_mode)` opens the file,
  builds a `TurmericScript`, calls `set_source_code(...)`, calls `reload()`,
  returns the script.

Registered with `ResourceLoader::add_resource_format_loader(loader)`.

Symmetrically, a `ResourceFormatSaverExtension` for editor-side "Save As"
flows.

## Impact on the v1 plan

Add to G2 ("Lifecycle + Inspector"):

> Implement and register `TurmericResourceLoader` and `TurmericResourceSaver`
> -- without these, `.tur` files can't be loaded via `load()` or attached as
> a script in the editor's "Attach Script..." dialog. Spec is straightforward
> (mirror GDScript's `ResourceFormatLoaderGDScript`).

The G1 demo for this session worked around it by instantiating
`TurmericScript` directly through `ClassDB.instantiate` and setting the
source by property. That validates the language end-to-end without the
resource-loader plumbing, which is the correct factoring -- the loader
is independent work.

## Why this is filed in turmeric/docs/reported/

Not a turmeric defect, but the turmeric-godot work depends on knowing
this exists. Filing here puts it on the same paper trail as the other
G1 reports (`libturi-embed-include-paths.md`,
`libturi-embed-interpret-mode-flag.md`) and keeps the Godot-binding
plan honest about scope.
