# turmeric-godot: baked-in prelude fails to eval, so no script ever loads

**Summary:** Every `.tur` script load in the Godot GDExtension prints
`turmeric-godot: baked-in prelude failed to eval: elaboration error` and then
fails with `Failed loading resource`. No script loads, in either execution mode.
This blocks WIN2 end-to-end verification and, if it reproduces off Windows, is a
straightforward showstopper for the shim.

**Severity:** High -- the GDExtension loads and registers correctly, but no
Turmeric script can actually run.

**Platform:** Observed on Windows (MinGW/UCRT64, Godot 4.3.stable).
**NOT verified on Linux/macOS** -- see "What is not known" before assuming this
is a Windows bug. Nothing about it looks platform-specific.

## Repro

```sh
cd ../turmeric-godot
scons platform=windows arch=x86_64 target=template_debug use_mingw=yes
cd examples/paddle-pong-tur
# import once, so res:// resources are in the database
Godot_v4.3-stable_win64.exe --headless --editor --path . --quit
# then run
TUR_BIN=/path/to/tur.exe Godot_v4.3-stable_win64.exe --headless --path . --quit
```

Output (trimmed):

```
[turmeric-godot] initialize(level=2)
[turmeric-godot] registered Turmeric script language + resource format
[turmeric-godot] libturi smoke: (+ 1 2) = 3 : int
hello from turmeric (via native)
[turmeric-godot] TurmericScript::_reload (len=1305, keep_state=no)
[turmeric res://scripts/ball.tur:<eval>:53 TUR-W0040] unknown name 'godot-vec2' ...
  ... ~40 more TUR-W0040 lines ...
turmeric-godot: baked-in prelude failed to eval: elaboration error
ERROR: Failed loading resource: res://scripts/ball.tur. Make sure resources have
       been imported by opening the project in the editor at least once.
ERROR: [pong] script load returned null
```

The Godot-level "Make sure resources have been imported" is a red herring: the
project *was* imported (`.godot/` exists and the editor pass succeeded). The
prelude failure precedes it and is the actual cause.

## What this is not

Ruled out during WIN2 bring-up, so nobody re-derives it:

- **Not the extension failing to load.** It initializes through all four levels,
  registers the script language and resource format, and `libturi` evaluates
  in-process (`(+ 1 2) = 3`).
- **Not an AOT-path problem.** It reproduces identically with the default
  interpreter mode and with AOT forced. The ~40 `TUR-W0040
  will runtime-dispatch` warnings show elaboration reaching the interpreter path
  in both cases.
- **Not caused by the Windows AOT port.** The failure is upstream of any of it:
  `.godot/turmeric-cache/` is never created, `tur build --shared` is never
  invoked, and `TUR_BIN` is never consulted.

## Where to look

`src/bridge/prelude.cpp` in `../turmeric-godot` holds the baked-in prelude; the
diagnostic is emitted where its eval result is checked. The elaboration error
itself is not printed -- only the fact that one occurred -- which is the first
thing to fix: surface the underlying diagnostic (file/line/code) instead of
`elaboration error`. Likely suspects, in order:

1. The prelude has drifted from the current `libturi` elaborator (this shim
   tracked a much older `main`; the compiler has moved ~1000 commits).
2. A prelude form that only elaborates under a flag/experiment the embedded
   path does not enable.
3. Genuinely Windows-specific elaboration divergence -- least likely, and it
   should be the last hypothesis tested, not the first.

The `unknown name 'godot-vec2' ... 'godot-call'` warnings may be the same root
cause surfacing earlier: those names are exactly what the prelude is supposed to
define. If the prelude never evaluated, the script would see all of them as
unknown -- which is precisely the observed output. Worth checking whether they
are one bug rather than two.

## What is not known

- Whether this reproduces on Linux/macOS. Nobody has run this example on any
  other platform recently; the shim's CI (`.github/workflows/build.yml`) builds
  but does not run a project. **Test that first** -- it decides whether this is
  a Windows report or a general one.
- Which prelude form fails. The diagnostic is swallowed.

## Related

- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md) -- WIN2.
- [jit-godot-embedding-spike.md](jit-godot-embedding-spike.md) -- if the JIT
  replaces the AOT path, the prelude still has to elaborate; this is upstream of
  that choice.
