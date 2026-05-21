---
title: Tidal API Reference
category: Music
description: Complete API documentation for all modules in the TidalCycles-inspired pattern library for Turmeric
---

# TidalCycles-like DSL for Turmeric - API Reference

> **Version**: 1.0  
> **Target**: Phase 20+ (Post-algebraic effects)

This document provides complete API documentation for the TidalCycles-inspired pattern library.

---

## Table of Contents

1. [Time Module (`tidal/time.tur`)](#1-time-module)
2. [Pattern Module (`tidal/pattern.tur`)](#2-pattern-module)
3. [Temporal Module (`tidal/temporal.tur`)](#3-temporal-module)
4. [Structural Module (`tidal/structural.tur`)](#4-structural-module)
5. [Transform Module (`tidal/transform.tur`)](#5-transform-module)
6. [Polyrhythm Module (`tidal/polyrhythm.tur`)](#6-polyrhythm-module)
7. [Synth Module (`tidal/synth.tur`)](#7-synth-module)
8. [Timing Module (`tidal/timing.tur`)](#8-timing-module)
9. [Live Module (`tidal/live.tur`)](#9-live-module)
10. [Mini Module (`tidal/mini.tur`)](#10-mini-module)
11. [Perf Module (`tidal/perf.tur`)](#11-perf-module)

---

## 1. Time Module (`tidal/time.tur`)

### Types

| Type | Description |
|------|-------------|
| `Beats` | `float` - Time in beats (1.0 = one quarter note) |
| `Seconds` | `float` - Time in seconds |
| `Samples` | `int64` - Time in samples at current sample rate |

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `bpm->beats-per-sec` | `(float) -> float` | Convert BPM to beats per second |
| `bpm->sec-per-beat` | `(float) -> float` | Convert BPM to seconds per beat |
| `beats->sec` | `(Beats, float) -> Seconds` | Convert beats to seconds at given BPM |
| `sec->beats` | `(Seconds, float) -> Beats` | Convert seconds to beats at given BPM |
| `samples->sec` | `(Samples, float) -> Seconds` | Convert samples to seconds at given sample rate |
| `sec->samples` | `(Seconds, float) -> Samples` | Convert seconds to samples at given sample rate |

### Tempo Tracking

| Function | Signature | Description |
|----------|-----------|-------------|
| `make-tempo` | `(float) -> Tempo` | Create tempo with BPM |
| `tempo->beats` | `(Tempo, Seconds) -> Beats` | Convert seconds to beats using tempo |
| `tempo-beats-per-sec` | `(Tempo) -> float` | Get beats per second from tempo |

---

## 2. Pattern Module (`tidal/pattern.tur`)

### Types

| Type | Description |
|------|-------------|
| `Pattern<T>` | `(fn [Beats] :T)` - Function from time to value |
| `NumPattern` | `Pattern<float>` - Numeric pattern |

### Constructors

| Function | Signature | Description |
|----------|-----------|-------------|
| `const` | `(T) -> Pattern<T>` | Constant pattern (always returns same value) |
| `cycle` | `(...T) -> Pattern<T>` | Cycle through values |
| `phase` | `(float = 1.0) -> NumPattern` | Phase position in cycle (0 to 1) |

### Macros

| Macro | Description |
|-------|-------------|
| `P` | `(...exprs) -> Pattern<T>` - Shorthand for const/cycle |

### Evaluation

| Function | Signature | Description |
|----------|-----------|-------------|
| `pat-at` | `(Pattern<T>, Beats) -> T` | Evaluate pattern at specific time |

---

## 3. Temporal Module (`tidal/temporal.tur`)

### Time Scaling

| Function | Signature | Description |
|----------|-----------|-------------|
| `slow` | `(float, Pattern<T>) -> Pattern<T>` | Slow down pattern by factor |
| `fast` | `(float, Pattern<T>) -> Pattern<T>` | Speed up pattern by factor |
| `stretch` | `(float, Pattern<T>) -> Pattern<T>` | Alias for slow |
| `compress` | `(float, Pattern<T>) -> Pattern<T>` | Alias for fast |

### Time Shifting

| Function | Signature | Description |
|----------|-----------|-------------|
| `shift` | `(Beats, Pattern<T>) -> Pattern<T>` | Shift pattern in time |
| `rot` | `(Beats, Pattern<T>) -> Pattern<T>` | Alias for shift |

### Time Mirroring

| Function | Signature | Description |
|----------|-----------|-------------|
| `mirror` | `(Pattern<T>) -> Pattern<T>` | Mirror pattern within cycle |
| `rev` | `(Pattern<T>) -> Pattern<T>` | Reverse time within cycle |

### Repetition

| Function | Signature | Description |
|----------|-----------|-------------|
| `repeat` | `(int, Pattern<T>) -> Pattern<T>` | Repeat pattern n times per cycle |
| `once` | `(Pattern<T>, T) -> Pattern<T>` | Play once, then return zero value |

---

## 4. Structural Module (`tidal/structural.tur`)

### Sequencing

| Function | Signature | Description |
|----------|-----------|-------------|
| `seq` | `(...Pattern<T>) -> Pattern<T>` | Sequence patterns |
| `seq-n` | `(...tuple<Pattern<T> float>) -> Pattern<T>` | Sequence with custom lengths |

### Stacking

| Function | Signature | Description |
|----------|-----------|-------------|
| `stack` | `(...Pattern<T>) -> Pattern<vec<T>>` | Stack patterns (play all simultaneously) |
| `stack-with` | `(fn[(vec<T>)]:U, ...Pattern<T>) -> Pattern<U>` | Stack with custom combiner |

### Convenience Combinators

| Function | Signature | Description |
|----------|-----------|-------------|
| `sum` | `(...NumPattern) -> NumPattern` | Sum all pattern values |
| `multiply` | `(...NumPattern) -> NumPattern` | Multiply all pattern values |

### Conditional Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `when` | `(Pattern<bool>, Pattern<T>, T) -> Pattern<T>` | Play when condition is true |
| `if-pat` | `(Pattern<bool>, Pattern<T>, Pattern<T>) -> Pattern<T>` | If-then-else for patterns |
| `every` | `(int, Pattern<T>, T) -> Pattern<T>` | Play on every nth beat |
| `nth` | `(int, Pattern<T>, T) -> Pattern<T>` | Play on nth beats (using floor mod) |

### Euclidean Rhythm

| Function | Signature | Description |
|----------|-----------|-------------|
| `euclid` | `(int, int, int = 0) -> NumPattern` | Bjorklund's algorithm for evenly distributed hits |

### Pattern Selection

| Function | Signature | Description |
|----------|-----------|-------------|
| `select` | `(Pattern<int>, ...Pattern<T>) -> Pattern<T>` | Select pattern based on index |
| `first-nonzero` | `(T, ...Pattern<T>) -> Pattern<T>` | First non-zero value from patterns |

---

## 5. Transform Module (`tidal/transform.tur`)

### Arithmetic Transformations

| Function | Signature | Description |
|----------|-----------|-------------|
| `add` | `(NumPattern, NumPattern \| float) -> NumPattern` | Add pattern/constant |
| `sub` | `(NumPattern, NumPattern \| float) -> NumPattern` | Subtract pattern/constant |
| `mul` | `(NumPattern, NumPattern \| float) -> NumPattern` | Multiply pattern/constant |
| `div` | `(NumPattern, NumPattern \| float) -> NumPattern` | Divide pattern/constant |

### Range and Clamping

| Function | Signature | Description |
|----------|-----------|-------------|
| `range` | `(NumPattern, float, float, float, float) -> NumPattern` | Map value range |
| `clamp` | `(NumPattern, float, float) -> NumPattern` | Clamp to range |

### Randomness

| Function | Signature | Description |
|----------|-----------|-------------|
| `jitter` | `(Pattern<T>, float, T) -> Pattern<T>` | Add random jitter |
| `rand-select` | `(...Pattern<T>) -> Pattern<T>` | Randomly select from patterns |
| `noise` | `(float, float) -> NumPattern` | Perlin noise pattern |
| `white-noise` | `() -> NumPattern` | White noise pattern |

### Waveform Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `sine` | `(float, float, float = 0.0) -> NumPattern` | Sine wave (freq, amp, phase) |
| `square` | `(float, float, float = 0.5) -> NumPattern` | Square wave (freq, amp, duty) |
| `saw` | `(float, float) -> NumPattern` | Sawtooth wave (freq, amp) |
| `tri` | `(float, float) -> NumPattern` | Triangle wave (freq, amp) |

### Envelopes

| Function | Signature | Description |
|----------|-----------|-------------|
| `adsr` | `(float, float, float, float, Pattern<bool> = (const true)) -> NumPattern` | ADSR envelope |
| `ar` | `(float, float, Pattern<bool> = (const true)) -> NumPattern` | Attack-Release envelope |

---

## 6. Polyrhythm Module (`tidal/polyrhythm.tur`)

### Polymeter

| Function | Signature | Description |
|----------|-----------|-------------|
| `polymeter` | `(float, Pattern<T>) -> Pattern<T>` | Pattern with different time signature |

### Polyrhythm

| Function | Signature | Description |
|----------|-----------|-------------|
| `polyrhythm` | `(Pattern<T>, float, Pattern<U>, float) -> Pattern<tuple<T U>>` | Two patterns with different cycle lengths |

### Nested Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `nest` | `(Pattern<Pattern<T>>, float = 1.0) -> Pattern<T>` | Nested pattern (pattern of patterns) |

### Polyrhythm Combinators

| Function | Signature | Description |
|----------|-----------|-------------|
| `stack-polymeter` | `(...tuple<Pattern<T> float>) -> Pattern<vec<T>>` | Stack patterns with different meters |
| `canon` | `(Pattern<T>, int, float = 0.25) -> Pattern<vec<T>>` | Delayed copies (canon) |
| `spread` | `(Pattern<T>, int, float = 1.0) -> Pattern<vec<T>>` | Phase-shifted copies |

### Time Signature

| Function | Signature | Description |
|----------|-----------|-------------|
| `timesig` | `(Pattern<T>, ...tuple<float int>) -> Pattern<T>` | Pattern with changing time signatures |

---

## 7. Synth Module (`tidal/synth.tur`)

### Note Conversion

| Function | Signature | Description |
|----------|-----------|-------------|
| `note->freq` | `(int) -> float` | Convert MIDI note to Hz |

### Note Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `note-pattern` | `(NotePattern, NumPattern = (const 100), NumPattern = (const 0.25), Pattern<bool> = (const true)) -> Pattern<map>` | Create note pattern |

### Types

| Type | Description |
|------|-------------|
| `NotePattern` | `Pattern<int>` - Pattern of MIDI notes |

### Chord Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `chord-pattern` | `(...NotePattern) -> NotePattern` | Create chord from note patterns |

### Arpeggio Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `arp-pattern` | `(Pattern<vec<int>>, float = 4.0) -> NotePattern` | Arpeggiate chord |

### Drum Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `drum-pattern` | `(...tuple<int float>) -> Pattern<map>` | Create drum pattern from hits |
| `kick` | `(vec<Beats>, ...kwargs) -> Pattern<map>` | Bass drum pattern |
| `snare` | `(vec<Beats>, ...kwargs) -> Pattern<map>` | Snare drum pattern |
| `hat` | `(vec<Beats>, ...kwargs) -> Pattern<map>` | Hi-hat pattern |
| `cymbal` | `(vec<Beats>, ...kwargs) -> Pattern<map>` | Cymbal pattern |

### Pattern to Synth Mapping

| Function | Signature | Description |
|----------|-----------|-------------|
| `pat->param` | `(NumPattern, Synth, cstr) -> unit` | Map pattern to synth parameter |
| `pats->params` | `(Synth, ...tuple<cstr NumPattern>) -> unit` | Map multiple patterns to parameters |

### Pattern Players

| Function | Signature | Description |
|----------|-----------|-------------|
| `pattern-synth` | `(ScscmSession, cstr, ...tuple<cstr NumPattern>, float) -> Synth` | Create synth with pattern-driven parameters |
| `make-player` | `(ScscmSession, cstr, Pattern<map>, float) -> PatternPlayer` | Create pattern player |
| `player-start` | `(PatternPlayer) -> unit` | Start pattern player |
| `player-stop` | `(PatternPlayer) -> unit` | Stop pattern player |
| `player-update` | `(PatternPlayer, Pattern<map>) -> PatternPlayer` | Update player with new pattern |
| `swap-patterns` | `(PatternPlayer, map<cstr Pattern<float>>) -> PatternPlayer` | Swap multiple patterns |

---

## 8. Timing Module (`tidal/timing.tur`)

### Beat Clock

| Type | Description |
|------|-------------|
| `BeatClock` | Struct with `bpm`, `sample-rate`, `samples-per-beat` |

| Function | Signature | Description |
|----------|-----------|-------------|
| `make-beat-clock` | `(float, float) -> BeatClock` | Create beat clock |
| `clock->beats` | `(BeatClock, Samples) -> Beats` | Convert samples to beats |
| `clock->samples` | `(BeatClock, Beats) -> Samples` | Convert beats to samples |

### Audio Pattern

| Function | Signature | Description |
|----------|-----------|-------------|
| `audio-pattern` | `(Pattern<float>, BeatClock) -> (fn [Samples] :float)` | Create audio-rate pattern |
| `mix-audio-patterns` | `(...AudioPattern) -> AudioPattern` | Mix audio patterns |

### Pattern Scheduler

| Type | Description |
|------|-------------|
| `PatternScheduler` | Struct with `clock`, `patterns`, `synths`, `events` |

| Function | Signature | Description |
|----------|-----------|-------------|
| `make-pattern-scheduler` | `(BeatClock) -> PatternScheduler` | Create scheduler |
| `schedule-pattern` | `(PatternScheduler, Pattern<map>, cstr, Beats) -> NodeID` | Schedule pattern |
| `schedule-event` | `(PatternScheduler, NodeID, Pattern<map>, Beats) -> unit` | Schedule event |
| `process-events` | `(PatternScheduler, Beats) -> unit` | Process scheduled events |

---

## 9. Live Module (`tidal/live.tur`)

### Live Session

| Type | Description |
|------|-------------|
| `LiveSession` | Struct with `bpm`, `patterns`, `state` |

| Function | Signature | Description |
|----------|-----------|-------------|
| `make-live-session` | `(float) -> LiveSession` | Create live session |
| `session-set-pattern` | `(LiveSession, cstr, Pattern<T>) -> LiveSession` | Set pattern in session |
| `session-get-pattern` | `(LiveSession, cstr) -> Pattern<T>` | Get pattern from session |
| `session-remove-pattern` | `(LiveSession, cstr) -> LiveSession` | Remove pattern from session |

### Pattern Replacement

| Function | Signature | Description |
|----------|-----------|-------------|
| `player-update` | `(PatternPlayer, Pattern<T>) -> PatternPlayer` | Update player pattern |
| `swap-patterns` | `(PatternPlayer, map<cstr Pattern<float>>) -> PatternPlayer` | Swap multiple patterns |

### Pattern Hot-Reloading

| Function | Signature | Description |
|----------|-----------|-------------|
| `watch-pattern` | `(cstr, ScscmSession, cstr, float) -> PatternPlayer` | Watch file for pattern changes |
| `eval-pattern` | `(cstr) -> Pattern<T>` | Evaluate pattern from string |

### Stateful Patterns

| Type | Description |
|------|-------------|
| `StatefulPattern<T>` | Struct with `pattern`, `state` |

| Function | Signature | Description |
|----------|-----------|-------------|
| `make-stateful-pattern` | `(T, fn[T Beats] : tuple<T Pattern<T>>) -> StatefulPattern<T>` | Create stateful pattern |
| `step-stateful` | `(StatefulPattern<T>, Beats) -> tuple<T StatefulPattern<T>>` | Step stateful pattern |
| `counter-pattern` | `(int) -> StatefulPattern<int>` | Counter pattern (mod n) |

### Session State Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `state-pattern` | `(cstr, fn[T] : Pattern<U>) -> Pattern<U>` | Pattern from session state |
| `updating-pattern` | `(cstr, T, fn[T Beats] : tuple<T U>) -> Pattern<U>` | Pattern that updates state |

---

## 10. Mini Module (`tidal/mini.tur`)

### Tokenization

| Type | Description |
|------|-------------|
| `MiniToken` | Enum: `:num`, `:op`, `:paren-open`, `:paren-close`, `:bracket-open`, `:bracket-close` |

| Function | Signature | Description |
|----------|-----------|-------------|
| `mini-tokenize` | `(cstr) -> vec<tuple<MiniToken cstr>>` | Tokenize mini-notation string |

### Parsing

| Function | Signature | Description |
|----------|-----------|-------------|
| `parse-mini` | `(cstr) -> Pattern<float>` | Parse mini-notation to pattern |
| `mini-parse` | `(cstr) -> Pattern<float>` | Parse mini-notation expression |
| `mini-parse-tokens` | `(vec<tuple<MiniToken cstr>>) -> Pattern<float>` | Parse tokens to pattern |

### Macros

| Macro | Description |
|-------|-------------|
| `s` | `(cstr) -> Pattern<float>` - Shorthand for `parse-mini` |
| `d` | `(cstr) -> Pattern<map>` - Shorthand for `parse-drums` |
| `n` | `(cstr) -> Pattern<float>` - Shorthand for `parse-notes` |

### Drum Notation

| Function | Signature | Description |
|----------|-----------|-------------|
| `parse-drums` | `(cstr) -> Pattern<map>` | Parse drum mini-notation |

### Operators

| Function | Signature | Description |
|----------|-----------|-------------|
| `mini-apply-op` | `(cstr, NumPattern, NumPattern) -> NumPattern` | Apply operator to patterns |

---

## 11. Perf Module (`tidal/perf.tur`)

### Pattern Caching

| Function | Signature | Description |
|----------|-----------|-------------|
| `cached` | `(Pattern<T>, int = 1024) -> Pattern<T>` | Cache pattern results |

### Pattern Inlining

| Function | Signature | Description |
|----------|-----------|-------------|
| `inline-pattern` | `(Pattern<T>) -> Pattern<T>` | Inline pattern for performance |

### Pattern Fusion

| Function | Signature | Description |
|----------|-----------|-------------|
| `fuse-pattern` | `(Pattern<T>) -> Pattern<T>` | Fuse consecutive operations |

### Benchmarking

| Function | Signature | Description |
|----------|-----------|-------------|
| `benchmark-pattern` | `(Pattern<T>, int, Beats) -> BenchmarkResult` | Benchmark pattern evaluation |
| `measure-latency` | `(fn[]:T, int) -> float` | Measure function latency |
| `measure-throughput` | `(fn[]:T, int) -> float` | Measure function throughput |

### Zero-Allocation Patterns

| Function | Signature | Description |
|----------|-----------|-------------|
| `zero-alloc-pattern` | `(Pattern<T>) -> Pattern<T>` | Create zero-alloc pattern |

---

## Main Module (`tidal/tidal.tur`)

The main module re-exports all functions from submodules:

```turmeric
(import tidal)  ;; Imports all tidal/* modules
```

```sweet-exp
import tidal  ;; Imports all tidal/* modules
```

### Convenience Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `play` | `(ScscmSession, cstr, cstr, float) -> PatternPlayer` | Play mini-notation pattern |
| `d1` | `(ScscmSession, cstr, Pattern<map>, float) -> PatternPlayer` | Set default pattern (d1) |
| `hush` | `(PatternPlayer) -> PatternPlayer` | Silence player (hush) |

---

## Index

### By Category

- **Time**: `bpm->beats-per-sec`, `beats->sec`, `bpm->sec-per-beat`, `sec->beats`, `samples->sec`, `sec->samples`
- **Patterns**: `const`, `cycle`, `phase`, `P`, `pat-at`
- **Temporal**: `slow`, `fast`, `shift`, `repeat`, `once`, `mirror`, `rev`
- **Structural**: `seq`, `stack`, `sum`, `multiply`, `when`, `every`, `euclid`, `select`
- **Transform**: `add`, `sub`, `mul`, `div`, `range`, `clamp`, `jitter`, `sine`, `square`, `saw`, `tri`, `adsr`, `ar`
- **Polyrhythm**: `polymeter`, `polyrhythm`, `nest`, `canon`, `spread`, `timesig`
- **Synth**: `note->freq`, `note-pattern`, `chord-pattern`, `arp-pattern`, `kick`, `snare`, `hat`
- **Live**: `make-live-session`, `player-update`, `swap-patterns`, `eval-pattern`, `watch-pattern`
- **Mini**: `parse-mini`, `s`, `parse-drums`, `d`

---

*Generated for Turmeric project - TidalCycles-like DSL API Reference*
