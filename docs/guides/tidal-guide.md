---
title: Tidal Guide
category: Tidal
description: Introduction to the TidalCycles-inspired live-coding pattern library for Turmeric, covering core concepts, combinators, and synth integration
---

# TidalCycles-like DSL for Turmeric - User Guide

> **Version**: 1.0  
> **Target**: Phase 20+ (Post-algebraic effects)  
> **Status**: Implementation Complete  

This guide introduces the TidalCycles-inspired pattern library for Turmeric, enabling live-coding of musical patterns with a powerful, composable DSL.

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Core Concepts](#2-core-concepts)
3. [Pattern Types](#3-pattern-types)
4. [Temporal Combinators](#4-temporal-combinators)
5. [Structural Combinators](#5-structural-combinators)
6. [Value Transformations](#6-value-transformations)
7. [Polyrhythm & Polymeter](#7-polyrhythm--polymeter)
8. [Synth Integration](#8-synth-integration)
9. [Live Coding](#9-live-coding)
10. [Mini-Notation](#10-mini-notation)
11. [Performance Optimization](#11-performance-optimization)
12. [Examples](#12-examples)

---

## 1. Quick Start

### Installation

The tidal library is included in Turmeric's standard library. Import it to get started:

```turmeric
(import tidal)
```

Or import individual modules:

```turmeric
(import tidal/pattern tidal/temporal tidal/structural)
```

### Your First Pattern

```turmeric
;; Create a simple cycle pattern
(def my-pattern (cycle 1 2 3 4))

;; Evaluate at different times
(my-pattern 0.0)  ;; => 1
(my-pattern 1.0)  ;; => 2
(my-pattern 2.0)  ;; => 3
```

### Connecting to scsynth

To hear your patterns, connect to a running SuperCollider server (scsynth):

```turmeric
(import scscm tidal/synth)

;; Connect to server
(def session (scscm-connect))

;; Play a pattern
(def player (make-player session "sine" (cycle 220.0 440.0 880.0) 120.0))
(player-start player)

;; Stop playing
(player-stop player)
```

---

## 2. Core Concepts

### Time Representation

Tidal uses **beats** as the primary time unit. One beat is one quarter note at the current tempo.

```turmeric
(def-type Beats float)

;; Convert between beats and seconds
(bpm->beats-per-sec 120.0)  ;; => 2.0 (120 BPM = 2 beats/sec)
(beats->sec 1.0 120.0)       ;; => 0.5 (1 beat at 120 BPM = 0.5 seconds)
```

### Pattern Type

A pattern is a function from time (in beats) to a value:

```turmeric
(def-type Pattern<T> (fn [Beats] :T))

;; A pattern that returns 440 at any time
(def p (const 440))
(p 0.0)  ;; => 440
(p 100.0)  ;; => 440
```

### Pull-Based Evaluation

Patterns are **pull-based**: they evaluate on demand when you call them with a time value. This makes them easy to compose and reason about.

---

## 3. Pattern Types

### Constant Patterns

```turmeric
;; Always returns the same value
(def p (const 440.0))

;; Shorthand: P macro
(def p (P 440.0))  ;; Same as (const 440.0)
```

### Cycle Patterns

```turmeric
;; Cycles through values
(def p (cycle 1 2 3 4))

(p 0.0)  ;; => 1
(p 0.5)  ;; => 1
(p 1.0)  ;; => 2
(p 3.0)  ;; => 4
(p 4.0)  ;; => 1 (wraps)

;; Shorthand: P macro with multiple args
(def p (P 1 2 3 4))  ;; Same as (cycle 1 2 3 4)
```

### Phase Pattern

```turmeric
;; Returns position within cycle (0 to 1)
(def p (phase 1.0))

(p 0.0)   ;; => 0.0
(p 0.25)  ;; => 0.25
(p 0.5)   ;; => 0.5
(p 0.75)  ;; => 0.75
(p 1.0)   ;; => 0.0 (wraps)

;; Custom cycle length
(def p (phase 2.0))
(p 0.0)   ;; => 0.0
(p 1.0)   ;; => 0.5
(p 2.0)   ;; => 0.0
```

### Evaluating Patterns

```turmeric
;; Apply pattern at specific time
(pat-at (cycle 1 2 3) 1.0)  ;; => 2

;; Map pattern over a range of times
(map (fn [t] ((cycle 1 2 3) t)) [0.0 1.0 2.0])  ;; => [1 2 3]
```

---

## 4. Temporal Combinators

### Time Scaling

```turmeric
;; Slow down by factor (pattern takes longer to complete)
(def slow-melody (slow 2 (cycle 1 2 3)))
;; At time 0: 1, time 2: 2, time 4: 3

;; Speed up by factor (pattern completes faster)
(def fast-melody (fast 2 (cycle 1 2 3)))
;; At time 0: 1, time 0.5: 2, time 1: 3

;; Stretch and compress are aliases
(stretch 2 p)  ;; Same as (slow 2 p)
(compress 2 p) ;; Same as (fast 2 p)
```

### Time Shifting

```turmeric
;; Shift pattern forward in time
(def shifted (shift 0.5 (cycle 1 2 3)))
;; At time 0: evaluates at 0.5 => 2
;; At time 0.5: evaluates at 1.0 => 3

;; rot is an alias for shift
(rot 0.25 p)
```

### Repetition

```turmeric
;; Repeat pattern n times within its cycle
(def repeated (repeat 3 (cycle 1 2)))
;; Cycle: [1, 1, 1, 2, 2, 2]
;; At time 0-0.33: 1, 0.33-0.66: 1, 0.66-1: 1, etc.

;; Play once, then silence
(def one-shot (once (const 1) 0))
;; At time 0-1: 1, time >1: 0
```

### Time Mirroring

```turmeric
;; Mirror pattern within its cycle
(def mirrored (mirror (cycle 1 2 3)))
;; In a 1-beat cycle: [0, 0.5) forward, [0.5, 1) backward

;; Reverse time within each cycle
(def reversed (rev (cycle 1 2 3)))
```

---

## 5. Structural Combinators

### Sequencing

```turmeric
;; Play patterns in sequence
(def seq-pattern (seq (cycle 1 2) (cycle 3 4)))
;; At time 0-1: [1, 2], time 1-2: [3, 4], time 2-3: [1, 2]

;; Sequence with custom lengths
(def seq-n-pattern (seq-n [(cycle 1 2) 2.0] [(cycle 3 4) 1.0]))
;; First pattern plays for 2 beats, second for 1 beat
```

### Stacking

```turmeric
;; Play patterns simultaneously
(def stack-pattern (stack (cycle 1 2) (cycle 10 20)))
;; At time 0: [1, 10], time 1: [2, 20]

;; Stack with custom combiner
(def sum-pattern (stack-with (fn [v] (reduce + 0 v)) (cycle 1 2) (cycle 3 4)))
;; At time 0: 1+3=4, time 1: 2+4=6

;; Convenience combinators
(sum (cycle 1 2) (cycle 3 4))    ;; Add all values
(multiply (cycle 1 2) (cycle 3 4)) ;; Multiply all values
```

### Conditional

```turmeric
;; Only play when condition is true
(def when-pattern (when (cycle true false) (const 1) 0))
;; At time 0: 1, time 1: 0, time 2: 1

;; If-then-else for patterns
(def if-pattern (if-pat (cycle true false) (const 1) (const 2)))

;; Play on every nth beat
(def every-pattern (every 2 (const 1) 0))
;; At time 0: 1, time 1: 0, time 2: 1, time 4: 1

;; Play on specific beats (nth)
(def nth-pattern (nth 2 (const 1) 0))
;; Same as every but uses floor(time) mod n
```

### Euclidean Rhythm

```turmeric
;; Bjorklund's algorithm for evenly distributed hits
(def euclid-pattern (euclid 8 3))
;; 3 hits in 8 steps: 1 0 0 1 0 0 1 0

def euclid-pattern (euclid 8 5 1))
;; 5 hits in 8 steps with offset 1
```

### Pattern Selection

```turmeric
;; Select from patterns based on index
(def select-pattern (select (cycle 0 1 0 1) (const 10) (const 20)))
;; At time 0: 10, time 1: 20, time 2: 10

;; First non-zero value
(def first-pattern (first-nonzero 0 (const 0) (const 0) (const 5)))
;; At any time: 5
```

---

## 6. Value Transformations

### Arithmetic

```turmeric
;; Add constant or pattern
(add (cycle 1 2 3) 10)      ;; Add 10 to each value
(add (cycle 1 2) (cycle 3 4))  ;; Add two patterns

;; Multiply
(mul (const 10) 2)          ;; Multiply by 2
(mul (cycle 1 2) (const 2)) ;; Multiply pattern by 2

;; Subtract and divide
(sub (const 10) 3)
(div (const 10) 2)
```

### Range Mapping

```turmeric
;; Map value range [from-low, from-high] to [to-low, to-high]
(def scaled (range (cycle 0.0 0.5 1.0) 0.0 1.0 440.0 880.0))
;; 0.0 -> 440, 0.5 -> 660, 1.0 -> 880
```

### Clamping

```turmeric
;; Clamp values to range
(def clamped (clamp (cycle -5 5 15) 0.0 10.0))
;; -5 -> 0, 5 -> 5, 15 -> 10
```

### Randomness

```turmeric
;; Add random jitter
(def jittered (jitter (const 440.0) 10.0 440.0))
;; Randomly selects from patterns
(def random-select (rand-select (const 1) (const 2) (const 3)))

;; Noise patterns
(def perlin (noise 1.0 0.0))    ;; Perlin noise
(def white (white-noise))       ;; White noise
```

### Waveform Patterns

```turmeric
;; Sine wave (freq, amp, phase)
(def sine-pattern (sine 1.0 1.0 0.0))

;; Square wave (freq, amp, duty cycle)
(def square-pattern (square 1.0 1.0 0.5))

;; Sawtooth wave (freq, amp)
(def saw-pattern (saw 1.0 1.0))

;; Triangle wave (freq, amp)
(def tri-pattern (tri 1.0 1.0))
```

### Envelopes

```turmeric
;; Attack-Release envelope
(def ar-env (ar 0.01 0.1 (const true)))

;; ADSR envelope (attack, decay, sustain, release, gate)
(def adsr-env (adsr 0.01 0.1 0.5 0.1 (const true)))
```

---

## 7. Polyrhythm & Polymeter

### Polymeter

```turmeric
;; Play pattern with different time signature
(def polymeter-pattern (polymeter 2.0 (cycle 1 2 3)))
;; Pattern runs at 2x speed: at time 0: 1, time 0.5: 2, time 1: 3

(def half-meter (polymeter 0.5 (cycle 1 2 3)))
;; Pattern runs at 0.5x speed: at time 0: 1, time 2: 2, time 4: 3
```

### Polyrhythm

```turmeric
;; Two patterns with different cycle lengths
(def polyrhythm-pattern (polyrhythm (cycle 1 2) 1.0 (cycle 10 20) 2.0))
;; Returns tuple: (pattern1-value, pattern2-value)
```

### Nested Patterns

```turmeric
;; Pattern of patterns
(def outer (cycle (const 1) (const 2)))
(def nested (nest outer 1.0))
;; At time 0-1: pattern returns 1, so nested returns 1 at any time
;; At time 1-2: pattern returns 2, so nested returns 2 at any time
```

### Stack Polymeter

```turmeric
;; Stack patterns with different meters
(def stack-poly (stack-polymeter [
  [(cycle 1 2) 1.0]
  [(cycle 10 20) 2.0]
  [(cycle 100 200) 0.5]
]))
```

### Canon

```turmeric
;; Delayed copies of the same pattern
(def canon-pattern (canon (cycle 1 2 3) 4 0.25))
;; 4 copies, each delayed by 0.25 beats
```

### Spread

```turmeric
;; Phase-shifted copies
(def spread-pattern (spread (cycle 1 2 3) 4 1.0))
;; 4 copies, spread across 1 beat
```

### Time Signature

```turmeric
;; Pattern that respects time signatures
(def timesig-pattern (timesig (cycle 1 2 3) [(4.0 4) (3.0 4)]))
;; First 4 beats in 4/4, next 3 beats in 3/4
```

---

## 8. Synth Integration

### Note Patterns

```turmeric
;; Create note pattern with pitch, velocity, duration, gate
(def note-pattern (note-pattern 
  (cycle 60 62 64)    ;; Pitch (MIDI note)
  (const 100)         ;; Velocity
  (const 0.25)        ;; Duration (in beats)
  (const true)))      ;; Gate (on/off)

;; MIDI note to frequency
(note->freq 69)  ;; => 440.0 (A4)
```

### Chord Patterns

```turmeric
;; Create chord from multiple note patterns
(def chord (chord-pattern 
  (const 48)   ;; C3
  (const 52)   ;; E3
  (const 55)))  ;; G3 (C major)
```

### Arpeggio Patterns

```turmeric
;; Arpeggiate a chord
(def arpeggio (arp-pattern (const [48 52 55]) 4.0))
;; Plays notes [48 52 55] at 4x speed (16th notes)
```

### Drum Patterns

```turmeric
;; Drum pattern with (note, velocity) pairs
(def drum-pattern (drum-pattern [(36 127) (none 0) (38 100) (none 0)]))

;; Convenience helpers
(def kick (kick [0.0 2.0]))      ;; Bass drum on beats 0 and 2
(def snare (snare [1.0 3.0]))    ;; Snare on beats 1 and 3
(def hat (hat [0.0 0.5 1.0 1.5]))  ;; Hi-hat on 8th notes
```

---

## 9. Live Coding

### Setting up a Session

```turmeric
(import tidal/live)

;; Create a live session at 120 BPM
(def session (make-live-session 120.0))

;; Create a player
(def player (make-player "sine" (const 440.0) 120.0))

;; Start playing
(player-start player)

;; Stop playing
(player-stop player)
```

### Pattern Replacement

```turmeric
;; Replace pattern in a running player
(player-update player (cycle 220.0 440.0 880.0))

;; Swap multiple patterns
(swap-patterns player new-patterns-map)
```

### d1 and hush Shortcuts

```turmeric
;; Set default pattern (d1 in TidalCycles)
(def d1 (swap-patterns player my-pattern))

;; Silence all (hush in TidalCycles)
(def hush (swap-patterns player (const 0)))
```

### Pattern Hot-Reloading

```turmeric
;; Watch a file for changes and reload
(def watcher (watch-pattern "my-pattern.tur" session "sine" 120.0))

;; Evaluate pattern from string
(def p (eval-pattern "(cycle 1 2 3)"))
```

### Stateful Patterns

```turmeric
;; Pattern with internal state
(def counter (make-stateful-pattern 0 
  (fn [count time] 
    [(inc count) (const count)])))

;; Step the stateful pattern
(let [[new-state new-pat] (step-stateful counter 0.0)]
  ;; new-state: 1
  ;; new-pat: returns 0 at any time
  )

;; Counter with modulo
(def counter-10 (counter-pattern 10))
```

---

## 10. Mini-Notation

### Parsing Mini-Notation

```turmeric
;; Parse TidalCycles-style mini-notation
(def p (parse-mini "1 2 3"))    ;; Same as (cycle 1 2 3)
(def p (parse-mini "1*2 2 3"))   ;; Fast first element

def p (parse-mini "[1 2] [3 4]")  ;; Stack patterns
```

### s Macro

```turmeric
;; Shorthand for parse-mini
(def p (s "1 2 3"))
```

### Drum Notation

```turmeric
;; Parse drum mini-notation
(def p (parse-drums "bd sd"))     ;; Bass drum, snare
(def p (parse-drums "bd*2 sd"))   ;; Two bass drums, snare

def p (s "bd(5,8) sd(3,8)")      ;; Euclidean drum patterns
```

### Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Add | `"1+2"` = (add (const 1) (const 2)) |
| `-` | Subtract | `"1-2"` = (sub (const 1) (const 2)) |
| `*` | Multiply | `"1*2"` = (mul (const 1) (const 2)) |
| `/` | Divide | `"1/2"` = (div (const 1) (const 2)) |
| `<` | Slow | `"1<2"` = (slow 2 (const 1)) |
| `>` | Fast | `"1>2"` = (fast 2 (const 1)) |
| `[]` | Stack | `"[1 2]"` = (stack (const 1) (const 2)) |
| `(,)` | Euclidean | `"(3,8)"` = (euclid 8 3) |

---

## 11. Performance Optimization

### Pattern Caching

```turmeric
;; Cache pattern results
(def cached-pat (cached (cycle 1 2 3) 1024))
;; Caches 1024 values for fast lookup
```

### Pattern Inlining

```turmeric
;; Inline simple patterns for better performance
(def inlined (inline-pattern (slow 2 (cycle 1 2))))
```

### Pattern Fusion

```turmeric
;; Fuse consecutive operations into one function
(def fused (fuse-pattern (slow 2 (fast 2 (cycle 1 2)))))
;; slow 2 * fast 2 = identity, so fuses to (cycle 1 2)
```

---

## 12. Examples

### Basic Example

```turmeric
(import tidal)

;; Simple melody
(def melody (cycle 60 62 64 65 67 69 71 72))

;; Add harmony
(def harmony (stack 
  melody
  (add melody (const 12))  ;; Octave up
  (add melody (const 7))))  ;; Fifth

;; Add rhythm
(def rhythm (stack 
  (kick [0.0 2.0])
  (snare [1.0 3.0])
  (hat [0.0 0.5 1.0 1.5 2.0 2.5 3.0 3.5])))

;; Combine everything
(def full-pattern (stack melody harmony rhythm))
```

### Polyrhythmic Example

```turmeric
(import tidal/polyrhythm)

;; Kick in 4/4, snare in 3/4, hats in 6/8
(def poly-beat (stack 
  (polymeter 1.0 (kick [0.0 1.0 2.0 3.0]))
  (polymeter 1.333 (snare [0.0 1.0 2.0]))
  (polymeter 0.667 (hat [0.0 0.5 1.0 1.5 2.0 2.5]))))
```

### Generative Example

```turmeric
(import tidal/polyrhythm tidal/transform)

;; Euclidean melody
(def euclid-melody (when (fn [t] (> ((euclid 16 5) t) 0.5)) 
  (cycle 60 64 67 72) 0))

;; With random variation
(def generative (jitter euclid-melody 2.0 60.0))

;; Add harmony
(def full-generative (stack 
  generative
  (add generative (const 12))
  (add generative (const 7))))
```

---

## API Reference

See [tidal-api.md](tidal-api.md) for complete API documentation.

## Cookbook

See [tidal-cookbook.md](tidal-cookbook.md) for common patterns and recipes.

---

## Support

- GitHub Issues: [turmeric2](https://github.com/rjungemann/turmeric2)
- Related: [TidalCycles](https://tidalcycles.org/), [SuperCollider](https://supercollider.github.io/)

## License

MIT License

*Generated for Turmeric project - TidalCycles-like DSL*
