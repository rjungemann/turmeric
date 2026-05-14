# SCSCM Library for Turmeric

SCSCM (SuperCollider Synth Control) is a Turmeric library for controlling the [hcsynth](https://github.com/hypercollider/hypercollider/tree/main/engine) SuperCollider audio engine. It provides a complete interface for live-coding audio synthesis with Turmeric.

## Features

- **FFI Bindings**: Low-level bindings to the hcsynth C API
- **OSC Message Building**: Ergonomic OSC message construction
- **Synth Control**: High-level API for creating and controlling synths
- **Type-Safe Parameters**: Parameter system with automatic warping and clamping
- **Live-Coding**: REPL-friendly evaluation with hot-reloading
- **Pattern Library**: TidalCycles-inspired pattern generation
- **Error Handling**: Production-ready error handling and connection monitoring
- **Performance Optimizations**: Inline-C functions and batching for high performance

## Quick Start

```turmeric
(import scscm/scscm :scscm)

;; Create a world (audio engine)
(def world (scscm/world-create 48000 512))

;; Create a synth (requires SynthDef to be loaded)
(def synth (scscm/synth-new world "sin" 1000))

;; Set parameters
(scscm/synth-set world synth 0 440.0)  ;; Frequency: 440 Hz
(scscm/synth-set world synth 1 0.5)    ;; Amplitude: 0.5

;; Render audio
(def buffer (malloc (* 512 2 (size-of float))))
(scscm/world-render world buffer)

;; Clean up
(scscm/synth-free world synth)
(scscm/world-destroy world)
```

## Live-Coding Example

```turmeric
(import scscm/scscm :scscm)

;; Create a live session
(def session (scscm/live-session-new))

;; Play a note
(scscm/play-note session "sin" 60 100 1.0)  ;; MIDI 60, vel 100, 1 sec

;; Create a pattern
(def bass-player
  (scscm/play-pattern session "sin"
    (fn [beats]
      (let [note (scscm/scale-pattern scscm/MAJOR_SCALE 60 beats)]
        {0 (scscm/midi->hz (or (unwrap note) 60))
         1 0.5
         2 0.0}))
    120 0.5))  ;; 120 BPM

;; Stop pattern
(scscm/pattern-player-stop bass-player)

;; Clean up
(scscm/live-session-destroy session)
```

## Module Structure

| Module | Description |
|--------|-------------|
| `ffi.tur` | Low-level FFI bindings to hcsynth C API |
| `types.tur` | Type definitions and constants |
| `msg.tur` | OSC message construction |
| `synth.tur` | High-level synth and node API |
| `params.tur` | Type-safe parameter system with warping |
| `live.tur` | Live-coding integration and scheduling |
| `pattern.tur` | Pattern library (TidalCycles-inspired) |
| `errors.tur` | Error handling and robustness |
| `perf.tur` | Performance optimizations |

## Documentation

See [docs/scscm-hcsynth-livecoding-plan.md](../../docs/scscm-hcsynth-livecoding-plan.md) for the detailed implementation plan and design decisions.

## Dependencies

- [hcsynth](https://github.com/hypercollider/hypercollider/tree/main/engine) - SuperCollider audio engine
- Turmeric Phase 2 FFI (`extern-c`, inline-C)

## Building

The SCSCM library requires linking against hcsynth. In your CMakeLists.txt:

```cmake
find_package(hcsynth REQUIRED)
target_link_libraries(your_target PRIVATE hcsynth)
```

Or include hcsynth as a submodule:

```cmake
add_subdirectory(../hypercollider/external/hcsynth)
```

## Testing

Run the FFI tests:

```bash
turmeric tests/scscm/ffi_test.tur
```

## Examples

| Example | Description |
|--------|-------------|
| `examples/scscm/basic.tur` | Basic world and synth management |
| `examples/scscm/live-coding.tur` | Live-coding with patterns |
| `examples/scscm/pattern-demo.tur` | Pattern library demonstration |

## License

MIT License
