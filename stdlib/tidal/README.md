# TidalCycles DSL for Turmeric

A TidalCycles-inspired pattern library for live-coding music with Turmeric and hcsynth.

## Features

- **Pattern Types**: Core `Pattern<T>` type as a function from beats to values
- **Temporal Combinators**: slow, fast, shift, rot, mirror, rev, repeat
- **Structural Combinators**: seq, stack, when, every, euclid, select
- **Value Transformations**: add, mul, range, clamp, jitter, wave patterns, envelopes
- **Polyrhythm Support**: polymeter, polyrhythm, canon, spread
- **Synth Integration**: Pattern-to-synth parameter mapping
- **Sample-Accurate Timing**: Beat clock, scheduler, audio-rate patterns
- **Live-Coding**: Pattern replacement, state management, file watching
- **Mini-Notation**: Concise string-based pattern specification
- **Performance Optimizations**: Caching, inlining, fusion, zero-allocation patterns

## Quick Start

```turmeric
(import tidal/tidal :tidal)

;; Create a session
(def session (tidal/live-session-new))

;; Define a simple pattern
(def my-pattern (tidal/s "1 2 3 4"))

;; Play the pattern with a synth
(def player (tidal/play session "sin" "1 2 3 4" 120))

;; Stop all patterns
tidal/hush session
```

## Pattern Basics

### Creating Patterns

```turmeric
;; Constant pattern
(def const-pat (tidal/const 440.0))

;; Cycle through values
(def cycle-pat (tidal/cycle 440.0 550.0 660.0 880.0))

;; Use the P macro for concise syntax
(def p1 (tidal/P 1 2 3))  ;; Same as (cycle 1 2 3)
(def p2 (tidal/P 5))     ;; Same as (const 5)
```

### Temporal Transformations

```turmeric
;; Slow down a pattern
(def slow-pat (tidal/slow 2 (tidal/cycle 1 2 3)))

;; Speed up a pattern
(def fast-pat (tidal/fast 2 (tidal/cycle 1 2 3)))

;; Shift in time
(def shifted (tidal/shift 0.5 (tidal/cycle 1 2 3)))

;; Rotate phase
(def rotated (tidal/rot 0.25 (tidal/cycle 1 2 3)))
```

### Structural Combinations

```turmeric
;; Sequence patterns
(def seq-pat (tidal/seq (tidal/cycle 1 2) (tidal/cycle 3 4)))

;; Stack patterns (play simultaneously)
(def stack-pat (tidal/stack (tidal/const 440.0) (tidal/const 0.5)))

;; Only play every N beats
(def every-2 (tidal/every 2 (tidal/const 1)))

;; Euclidean rhythm
(def euclid-pat (tidal/euclid 8 3))  ;; 3 hits in 8 steps
```

### Value Transformations

```turmeric
;; Add a constant
(def add-100 (tidal/add (tidal/cycle 1 2 3) 100))

;; Multiply by a constant
(def scaled (tidal/mul (tidal/cycle 1 2 3) 2))

;; Map to a range
(def range-pat (tidal/range (tidal/cycle 0 1) 0 1 440 880))

;; Sine wave
(def sine-pat (tidal/sine 1.0 0.5))  ;; freq=1, amp=0.5
```

## Mini-Notation

Tidal's concise string-based pattern syntax:

```turmeric
;; Numbers
(tidal/s "1 2 3")  ;; (cycle 1 2 3)

;; Operators
(tidal/s "1*2 3")  ;; Multiply pattern by 2

;; Fast (brackets)
(tidal/s "[1 2 3]")  ;; (fast 2 (cycle 1 2 3))

;; Drums
(tidal/d "bd sd bd sd")  ;; Bass-snare pattern

;; Notes
(tidal/n "c4 e4 g4 c5")  ;; C major arpeggio

;; Chords
(tidal/chord "[c4 e4 g4]")  ;; C major chord

;; Shorthand rhythm
(tidal/r "x o x o")  ;; Alternating hits and rests
```

## Polyrhythm

```turmeric
;; 3 against 2
(def poly-3-2 (tidal/polyrhythm (tidal/cycle 1 2 3) 3 (tidal/cycle 4 5) 2))

;; Or use presets
(def poly (tidal/POLY_3_2))

;; Canon (delayed copies)
(def canon (tidal/canon (tidal/cycle 60 62 64) 3 0.5))

;; Spread copies
(def spread (tidal/spread (tidal/cycle 1 2 3) 4 1.0))
```

## Synth Integration

```turmeric
;; Create a pattern synth
(def synth (tidal/pattern-synth session "sin" 
                                    {0 (tidal/sine 1.0 100.0)  ;; freq
                                     1 (tidal/const 0.5)}))     ;; amp

;; Note patterns
(def notes (tidal/note-pattern (tidal/cycle 60 62 64) (tidal/const 100)))

;; Drum patterns
(def drums (tidal/drum-machine (tidal/kick 0 2) (tidal/snare 2 4) (tidal/hat 0 0.5 1 1.5)))
```

## Live-Coding

```turmeric
;; Create a live session
(def live (tidal/live-session-new))

;; Define a pattern
(tidal/def-pattern live "bass" (tidal/s "1 2 3"))

;; Play the pattern
(tidal/d1 live "bass" "1 2 3" "sin" 120)

;; Update the pattern live
(tidal/set-pattern live "bass" (tidal/s "3 2 1"))

;; Stop all patterns
tidal/hush live
```

## Project Structure

```
stdlib/tidal/
├── time.tur              # Time types and utilities
├── pattern.tur           # Core Pattern type and constructors
├── temporal.tur          # Temporal combinators (slow, fast, shift, etc.)
├── structural.tur        # Structural combinators (seq, stack, etc.)
├── transform.tur         # Value transformations (add, mul, wave, etc.)
├── polyrhythm.tur        # Polyrhythm and polymeter support
├── synth.tur             # Synth integration
├── timing.tur            # Sample-accurate timing
├── live.tur              # Live-coding integration
├── mini.tur              # Mini-notation parser
├── perf.tur              # Performance optimizations
└── tidal.tur             # Main module (re-exports all)
```

## Documentation

- [TidalCycles Original](https://tidalcycles.org/) - The Haskell library this is based on
- [docs/tidalcycles-dsl-plan.md](../../docs/tidalcycles-dsl-plan.md) - Detailed implementation plan

## Dependencies

- Turmeric core language
- scscm library (for synth integration)
- hcsynth (SuperCollider audio engine)

## License

MIT License
