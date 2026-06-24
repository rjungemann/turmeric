# Turmeric Debugger (VS Code)

A thin VS Code extension that drives the Turmeric interpreter debugger through
the Debug Adapter Protocol. All the debugging work happens in the compiler --
this extension just declares the `turmeric` debug type and launches `tur dap`
as the debug adapter.

See [docs/upcoming/debugger-dap-phase3.md](../../docs/upcoming/debugger-dap-phase3.md)
for the protocol details and [docs/upcoming/debugger-plan.md](../../docs/upcoming/debugger-plan.md)
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

## Not yet

- Setting a value from the Variables view.
- Expanding structured values (structs / options / results render inline as a
  string rather than an expandable tree).
- Native (`tur build`) debugging -- that is debugger Phases 4-5 (gdb/lldb).
