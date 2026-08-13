# Turmeric Debugger (VS Code)

A thin VS Code extension that drives the Turmeric interpreter debugger through
the Debug Adapter Protocol. All the debugging work happens in the compiler --
this extension just declares the `turmeric` debug type and launches `tur dap`
as the debug adapter.

See [docs/artifacts/debugger-dap-phase3.md](../../docs/artifacts/debugger-dap-phase3.md)
for the protocol details and [docs/archive/history/debugger-plan.md](../../docs/archive/history/debugger-plan.md)
for the staged plan.

## Prerequisites

- The `tur` binary on your `PATH` (or set `"tur": "/abs/path/to/tur"` in the
  launch config).

## Install (development)

```sh
# from the repo root
cp -r editors/vscode-turmeric ~/.vscode/extensions/turmeric-debug-0.1.0
# then reload VS Code (Developer: Reload Window)
```

No build step -- the extension is plain JavaScript with no dependencies.

## Use

Open a `.tur` file and press **F5**. With no `launch.json`, VS Code starts the
default configuration (debug the current file, stop on entry). To customize,
add a `.vscode/launch.json`:

```jsonc
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "turmeric",
      "request": "launch",
      "name": "Debug current Turmeric file",
      "program": "${file}",
      "args": [],
      "stopOnEntry": true
      // "tur": "/abs/path/to/tur"   // optional: override the tur binary
    }
  ]
}
```

## Supported features

- Source breakpoints (and conditional breakpoints of the form
  `<name> <op> <literal>`, e.g. `i == 3`).
- Step over / step into / step out / continue.
- Call stack, per-frame locals, and `evaluate` (hover / Debug Console) for
  single names.
- Program stdout forwarded to the Debug Console as output events.
- **Native Debugging (gdb/lldb)**: Step through `.tur` source files natively, resolve stack traces, and inspect Option/Result/Cons types beautifully in their original Turmeric representation.

## Native Debugging Setup (Phases 4-5)

To debug native compiled binaries produced by `tur build --debug` on macOS/Linux:

1. Install the **CodeLLDB** extension (`vadimcn.vscode-lldb`) or **lldb-dap** extension in VS Code.
2. Add an LLDB configuration to your project's `.vscode/launch.json` that imports our LLDB pretty-printers module via `initCommands`:

```jsonc
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "lldb",
      "request": "launch",
      "name": "Debug Turmeric Native (CodeLLDB)",
      "program": "${workspaceFolder}/build/${fileBasenameNoExtension}",
      "initCommands": [
        "command script import ${workspaceFolder}/tools/debug/turmeric_lldb.py"
      ]
    }
  ]
}
```

With this, when a breakpoint hits in your `.tur` source files, all local variables of type `Option`, `Result`, and `Cons` will render in polished, human-readable Turmeric formatting (e.g. `(some 42)` or `(ok 14)`) instead of raw C structures!

## Not yet

- Setting a value from the Variables view.
- Expanding structured values (structs render inline as a string rather than an expandable tree).
