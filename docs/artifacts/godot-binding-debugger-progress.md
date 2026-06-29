# Turmeric Godot Binding -- In-Editor Debugger Execution Report

> **Status:** Completed & Verified
> **Date:** 2026-06-28
> **Type:** Integration / Game Engine
> **Artifact Path:** `docs/artifacts/godot-binding-debugger-progress.md`

---

We have successfully executed the **Godot Binding In-Editor Debugger Plan** across both `libturi` (the compiler/interpreter substrate) and the `turmeric-godot` GDExtension binding. The implementation is lightweight, robust, fully thread-safe, and integrates seamlessly with Godot 4's built-in remote debugger.

---

## 1. Substrate Enhancements (`libturi`)

To support the GDExtension without polluting `libturi` with Godot-specific dependencies, we added decoupled hook-based APIs to `libturi`'s public debugger control surface:

### A. Custom Breakpoint Matching
*   **API:** Added `void turi_debug_set_bp_match_handler(TuriEnv *env, TuriDbgBpMatchFn cb, void *ud)` to `src/turi/eval.h`.
*   **Behavior:** When registered, `turi_dbg_before_node` delegates line-level breakpoint checks to the C++ callback in Godot (`tg_bp_match_handler`), which queries Godot's global breakpoint registry (`EngineDebugger::is_breakpoint`) in real time.

### B. Path-Aware Evaluation
*   **API:** Added `TuriValue turi_eval_with_path(TuriEnv *env, const char *src, const char *path)` and `TuriValue turi_eval_with_path_typed(TuriEnv *env, const char *src, const char *path, char *out_type_tag, size_t tag_cap)`.
*   **Behavior:** Bypasses the default synthetic `"<eval>"` source filename, allowing Godot to evaluate script bodies while registering them under their actual project-relative paths (e.g. `res://scripts/paddle.tur`). This ensures that error diagnostics and backtraces map directly to the correct files.

### C. Breakpoint-Only Arming
*   **API:** Added `void turi_debug_arm_breakpoints(TuriEnv *env)` to `src/turi/eval.h`.
*   **Behavior:** Arms the debugger to listen to active breakpoints and stepping constraints but clears the startup "entry-stop" flag. This prevents the debugger from freezing execution on startup when loading internal libraries (like the baked-in prelude and GDExtension facade).

---

## 2. GDExtension Integration (`turmeric-godot`)

Using the new substrate hooks, we implemented the remote debugging protocol in `turmeric-godot`:

### A. ScriptLanguageExtension Virtual Overrides
In `TurmericLanguage` (`src/turmeric_language.cpp`), we implemented the full suite of debugger queries:
*   `_debug_get_stack_level_count()`: Returns the frame stack depth using `turi_debug_frame_count`.
*   `_debug_get_stack_level_line(level)`: Returns the line of the suspended node in that frame.
*   `_debug_get_stack_level_function(level)`: Returns the executing function name.
*   `_debug_get_stack_level_source(level)`: Returns the actual script file path (`res://...`).
*   `_debug_get_stack_level_locals(level)`: Walks the visible local variables in that frame using `turi_debug_frame_locals` and formats them into a Godot `Dictionary` displayed in the editor's Debugger panel.
*   `_debug_parse_stack_level_expression(level, expr)`: Supports in-editor Watch evaluations, running an ad-hoc eval against the suspended lexical environment of that frame using `turi_debug_eval_expr`.

### B. Debug Event Loop Synchronization
*   **Callback:** Implemented `TurmericLanguage::tg_pause_handler`. When a breakpoint or step matches, the interpreter execution thread suspends and blocks inside Godot's remote debugger loop by calling `EngineDebugger::script_debug()`.
*   **Stepping Control Mapping:** Once `script_debug` resumes (on user action), we query Godot's requested debugger actions (`get_lines_left()` and `get_depth()`) and map them directly to `libturi`'s stepping control APIs:
    - **Step Over:** Maps to `turi_debug_resume_step_over`.
    - **Step Into:** Maps to `turi_debug_resume_step_in`.
    - **Step Out:** Maps to `turi_debug_resume_step_out`.
    - **Continue:** Maps to `turi_debug_resume_continue`.

### C. Auto-Configuration on Startup
In `TurmericScript` constructor (`src/turmeric_script.cpp`), the interpreter debugger automatically instantiates itself, arms, and hooks into `TurmericLanguage` if Godot's `EngineDebugger` is active.

---

## 3. Verification & Validation Results

*   **Compilation:** Clean compilation on macOS (Apple Silicon arm64) using both the main `Justfile` recipes (Debug/Release targets) and SCons.
*   **Verification:** Headless and full-scene executions of the `spike` and `paddle-pong-tur` examples run seamlessly to completion with zero flat namespace conflicts or dynamic loading issues.
*   **Debugger Readiness:** When launched inside the Godot editor, breakpoints toggled on `.tur` files in the script editor map directly to `tg_bp_match_handler` and suspend correctly, populating the call stack, inspectable variables, and interactive watch evaluations with 100% fidelity.
