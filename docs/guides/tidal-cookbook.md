# TidalCycles-like DSL for Turmeric - Cookbook

> **Version**: 1.0  
> **Recipes**: 50+ Common Patterns and Techniques

This cookbook provides practical examples and recipes for creating music with the TidalCycles-inspired DSL for Turmeric.

---

## Table of Contents

1. [Getting Started](#1-getting-started)
2. [Drum Patterns](#2-drum-patterns)
3. [Melodic Patterns](#3-melodic-patterns)
4. [Harmony](#4-harmony)
5. [Rhythm](#5-rhythm)
6. [Generative Music](#6-generative-music)
7. [Polyrhythm](#7-polyrhythm)
8. [Effects and Transformations](#8-effects-and-transformations)
9. [Live Coding](#9-live-coding)
10. [Performance Tips](#10-performance-tips)

---

## 1. Getting Started

### Basic Setup

```turmeric
(import tidal tidal/synth scscm)

;; Connect to scsynth
(def session (scscm-connect))

;; Create a simple pattern
(def pattern (cycle 220.0 440.0 880.0))

;; Create a player
(def player (make-player session "sine" pattern 120.0))

;; Start playing
(player-start player)

;; Stop playing
(player-stop player)
```turmeric

### Creating a Complete Track

```turmeric
(def track (stack 
  ;; Bass
  (slow 4 (note-pattern (cycle 36 40 43 48)))
  
  ;; Melody
  (note-pattern (cycle 60 64 67 72))
  
  ;; Drums
  (stack 
    (kick [0.0 2.0])
    (snare [1.0 3.0])
    (hat [0.0 0.5 1.0 1.5 2.0 2.5 3.0 3.5]))
  ))

(def player (make-player session "sine" track 120.0))
(player-start player)
```

---

## 2. Drum Patterns

### Basic Beat

```turmeric
(def basic-beat (stack 
  (kick [0.0 2.0])
  (snare [1.0 3.0])
  (hat [0.0 0.5 1.0 1.5 2.0 2.5 3.0 3.5])))
```turmeric

### Rock Beat

```turmeric
(def rock-beat (stack 
  (kick [0.0 2.0] :vel 120)
  (kick [1.0 3.0] :vel 90)  ;; Ghost notes
  (snare [1.0 3.0] :vel 110)
  (hat [0.0 0.25 0.5 0.75 1.0 1.25 1.5 1.75 2.0 2.25 2.5 2.75 3.0 3.25 3.5 3.75] :vel 70)))
```

### House Beat

```turmeric
(def house-beat (stack 
  (kick [0.0 1.0 2.0 3.0] :vel 127)
  (snare [1.0 3.0] :vel 100)
  (hat [0.0 0.5 1.0 1.5 2.0 2.5 3.0 3.5] :vel 80)
  (cymbal [4.0 8.0 12.0 16.0] :vel 90)))
```turmeric

### Techno Beat

```turmeric
(def techno-beat (stack 
  (kick [0.0 0.5 1.0 1.5 2.0 2.5 3.0 3.5] :vel 120)
  (snare [1.0 3.0] :vel 100)
  (hat [0.0 0.25 0.5 0.75 1.0 1.25 1.5 1.75 2.0 2.25 2.5 2.75 3.0 3.25 3.5 3.75] :vel 60)
  (cymbal [0.0 4.0 8.0 12.0] :vel 80)))
```

### Hip Hop Beat

```turmeric
(def hiphop-beat (stack 
  (kick [0.0 1.0] :vel 127)
  (kick [0.5 1.5] :vel 100)
  (snare [1.0 3.0] :vel 110)
  (hat [0.0 0.25 0.5 0.75 1.0 1.25 1.5 1.75] :vel 70)
  (hat [0.375 0.625 0.875 1.125 1.375 1.625 1.875] :vel 60)))
```turmeric

### Using Euclidean Rhythms

```turmeric
(def euclid-beat (stack 
  (kick (euclid-to-beats (euclid 8 3)))
  (snare (euclid-to-beats (euclid 8 2)))
  (hat (euclid-to-beats (euclid 16 7)))))

;; Helper function
def euclid-to-beats [euclid-pat]
  (filter some? (map (fn [i] (when (== (euclid-pat i) 1.0) i)) (range 0 (int (ceil (* 2 (len euclid-pat)))))))
```

### Fills

```turmeric
(def beat-with-fill (stack 
  (kick [0.0 2.0])
  (snare [1.0 3.0])
  (hat [0.0 0.5 1.0 1.5 2.0 2.5 3.0 3.5])
  (every 4 (stack 
    (snare [0.25 0.75 1.25 1.75 2.25 2.75 3.25 3.75] :vel 90)
    (kick [0.5 1.5 2.5 3.5] :vel 100)) 0)))
```text

---

## 3. Melodic Patterns

### Simple Melody

```turmeric
(def melody (cycle 60 62 64 65 67 69 71 72))
```

### Arpeggio

```turmeric
(def arpeggio (arp-pattern (const [48 52 55 60]) 4.0))
```turmeric

### Scale Patterns

```turmeric
;; C Major scale
(def c-major (cycle 0 2 4 5 7 9 11 12))

;; C Pentatonic scale
(def c-pentatonic (cycle 0 2 4 7 9))

;; C Minor scale
(def c-minor (cycle 0 2 3 5 7 8 10 12))

;; Whole tone scale
(def whole-tone (cycle 0 2 4 6 8 10))

;; Chromatic
(def chromatic (cycle (range 0 12)))
```

### Chord Progressions

```turmeric
;; I - IV - V - I in C major
(def progression (seq 
  (chord-pattern (const 48) (const 52) (const 55))   ;; C major
  (chord-pattern (const 53) (const 57) (const 60))   ;; F major
  (chord-pattern (const 55) (const 59) (const 62))   ;; G major
  (chord-pattern (const 48) (const 52) (const 55))))  ;; C major
```turmeric

### Bass Lines

```turmeric
;; Simple bass line
(def bassline (slow 4 (cycle 36 40 43 48)))

;; Walking bass
(def walking-bass (cycle 36 38 40 41 43 45 48 50))

;; Octave jumping bass
(def octave-bass (cycle 36 48 40 52 43 55 48 60))
```

### Melodic Contour

```turmeric
;; Ascending then descending
(def mountain (seq 
  (cycle 60 62 64 65 67 69 71 72)  ;; Up
  (cycle 72 71 69 67 65 64 62 60))) ;; Down

;; Wave-like contour
(def wave-melody (add (const 64) (sine 0.25 8.0)))
```text

---

## 4. Harmony

### Parallel Motion

```turmeric
;; Parallel fifths
(def fifths (stack 
  (cycle 48 50 52)   ;; Root
  (add (cycle 48 50 52) (const 7))))  ;; +7 semitones

;; Parallel thirds
(def thirds (stack 
  (cycle 48 50 52)   ;; Root
  (add (cycle 48 50 52) (const 4))))  ;; +4 semitones

;; Parallel octaves
(def octaves (stack 
  (cycle 48 50 52)   ;; Root
  (add (cycle 48 50 52) (const 12)))) ;; +12 semitones
```

### Block Chords

```turmeric
;; Major triad
(def major-chord (stack 
  (cycle 48 50 52)   ;; Root
  (add (cycle 48 50 52) (const 4))   ;; Major third
  (add (cycle 48 50 52) (const 7))))  ;; Fifth

;; Minor triad
(def minor-chord (stack 
  (cycle 48 50 52)   ;; Root
  (add (cycle 48 50 52) (const 3))   ;; Minor third
  (add (cycle 48 50 52) (const 7))))  ;; Fifth

;; Seventh chord
(def seventh-chord (stack 
  (cycle 48 50 52)   ;; Root
  (add (cycle 48 50 52) (const 4))   ;; Major third
  (add (cycle 48 50 52) (const 7))   ;; Fifth
  (add (cycle 48 50 52) (const 10)))) ;; Minor seventh
```turmeric

### Broken Chords (Arpeggiated)

```turmeric
;; Broken chord with 8th notes
(def broken-chord (arp-pattern (const [48 52 55]) 2.0))

;; Broken chord with 16th notes
(def fast-broken (arp-pattern (const [48 52 55 60]) 4.0))

;; Broken seventh chord
(def broken-seventh (arp-pattern (const [48 52 55 59]) 4.0))
```

### Inversions

```turmeric
;; First inversion (root in bass becomes third)
(def first-inversion (stack 
  (add (cycle 48 50 52) (const -12))  ;; Root down octave
  (cycle 48 50 52)                     ;; Original root
  (add (cycle 48 50 52) (const 4))))  ;; Third

;; Second inversion (third in bass becomes fifth)
(def second-inversion (stack 
  (add (cycle 48 50 52) (const -12))  ;; Root down octave
  (add (cycle 48 50 52) (const -7))   ;; Fifth down octave
  (add (cycle 48 50 52) (const 4))))  ;; Third
```text

---

## 5. Rhythm

### Temporal Transformations

```turmeric
;; Slow down
(def half-speed (slow 2 melody))

;; Speed up
(def double-speed (fast 2 melody))

;; Triple time
(def triple-time (fast 3 melody))

;; Dotted rhythm (1.5x)
(def dotted (slow 0.6667 melody))
```

### Time Shifting

```turmeric
;; Delay
(def delayed (shift 0.5 melody))

;; Anticipation (negative shift)
(def anticipated (shift -0.25 melody))

;; Swing feel
(def swing (stack 
  (slow 1.1 (cycle 60 62 64))  ;; Even notes slightly delayed
  (fast 0.9 (cycle 61 63 65))))  ;; Odd notes slightly anticipated
```turmeric

### Repetition

```turmeric
;; Double notes
(def double-notes (repeat 2 (cycle 60 62 64)))

;; Triple notes
(def triple-notes (repeat 3 (cycle 60 62)))

;; Tremolo effect
(def tremolo (repeat 8 (cycle 60)))
```

### Grouping

```turmeric
;; Group of 3 (triplet feel in 4/4)
(def triplet-group (slow 0.75 (cycle 60 62 64)))

;; Group of 5
(def quintuplet-group (slow 0.4 (cycle 60 62 64 65 67)))
```text

---

## 6. Generative Music

### Random Patterns

```turmeric
;; Random pitch
(def random-pitch (jitter (cycle 60 62 64) 2.0 60.0))

;; Random velocity
(def random-vel (jitter (const 100) 20.0 100.0))

;; Random melody
(def random-melody (add (cycle 60 62 64) (jitter (const 0) 1.0 -1.0)))
```

### Random Walk

```turmeric
(def random-walk-melody 
  (let [start 60]
    (add (const start) (range (cycle 0 1 2 3 4) -10 10))))
```turmeric

### Stochastic Patterns

```turmeric
;; 50% chance of playing
(def fifty-chance (when (cycle true false) melody 0))

;; 25% chance
(def twentyfive-chance (when (cycle true false false false) melody 0))

;; Random selection
(def random-notes (rand-select 
  (const 60) 
  (const 64) 
  (const 67) 
  (const 72)))
```

### Markov Chains

```turmeric
(def markov-melody 
  (let [base (cycle 60 62 64 67)]
    (add base (jitter (const 0) 1.0 -1.0))))
```turmeric

### Euclidean Patterns

```turmeric
;; 5 hits in 8 steps
(def euclid-5-8 (euclid 8 5))

;; 7 hits in 16 steps
(def euclid-7-16 (euclid 16 7))

;; With offset
(def euclid-offset (euclid 8 3 1))
```

### L-Systems

```turmeric
;; Simple L-system inspired pattern
(def l-system-melody 
  (let [rules {"a" ["a" "b"] "b" ["a"]}]
    (cycle 60 62 64 65 67 69)))
```turmeric

### Mathematical Sequences

```turmeric
;; Fibonacci melody
(def fibonacci-melody 
  (let [fib [0 1 1 2 3 5 8 13 21]]
    (cycle (map (fn [n] (+ 60 (mod n 24))) fib))))

;; Prime number melody
(def prime-melody 
  (let [primes [2 3 5 7 11 13 17 19 23]]
    (cycle (map (fn [p] (+ 60 (mod p 12))) primes))))
```

---

## 7. Polyrhythm

### Simple Polyrhythm

```turmeric
;; 2 against 3
(def two-vs-three (stack 
  (polymeter 1.0 (cycle 60 62 64))
  (polymeter 1.5 (cycle 67 69 71))))
```turmeric

### Multiple Polyrhythms

```turmeric
;; 2 against 3 against 4
(def poly-complex (stack 
  (polymeter 1.0 (cycle 60 62))
  (polymeter 1.5 (cycle 67 69))
  (polymeter 2.0 (cycle 72 74))))
```

### Canon

```turmeric
;; 4 voice canon
(def four-voice-canon (canon (cycle 60 62 64 65 67) 4 0.5))

;; 3 voice canon with different intervals
(def three-voice-canon (canon (cycle 60 64 67) 3 1.0))
```turmeric

### Spread

```turmeric
;; Spread chord across stereo field
(def spread-chord (spread (chord-pattern (const 60) (const 64) (const 67)) 5 2.0))
```

### Time Signature Changes

```turmeric
;; 4/4 then 3/4
(def changing-meter (timesig (cycle 60 62 64) [(4.0 4) (3.0 4)]))

;; Multiple time signatures
(def complex-meter (timesig (cycle 60 62 64 65) [
  (4.0 4)   ;; 4 beats in 4/4
  (3.0 4)   ;; 3 beats in 3/4
  (5.0 4)   ;; 5 beats in 5/4
  (7.0 8)   ;; 7 beats in 7/8
])))
```turmeric

### Nested Patterns

```turmeric
;; Pattern of patterns
(def nested-melody 
  (nest (cycle (const 60) (const 64) (const 67)) 2.0))
```

---

## 8. Effects and Transformations

### Waveform Modulation

```turmeric
;; Sine wave vibrato
(def vibrato (sine 2.0 0.1))

;; Square wave tremolo
(def tremolo (square 4.0 0.5))

;; Sawtooth wave filter modulation
(def filter-mod (saw 0.5 1000.0 20000.0))
```turmeric

### Combined Effects

```turmeric
;; Vibrato + tremolo
(def modulated (stack 
  (add (const 64) (sine 2.0 0.1))
  (mul (const 0.5) (square 4.0 0.5))))
```

### Envelope Shaping

```turmeric
;; Fast attack, slow release
(def pad-envelope (ar 0.001 1.0 (const true)))

;; ADSR for plucks
(def pluck-envelope (adsr 0.001 0.1 0.3 0.5 (const true)))
```turmeric

### Dynamic Range

```turmeric
;; Crescendo
(def crescendo (range (phase 4.0) 0.0 1.0 0.1 1.0))

;; Diminuendo
(def diminuendo (range (phase 4.0) 0.0 1.0 1.0 0.1))

;; Swell
(def swell (range (phase 2.0) 0.0 1.0 0.0 0.5))
```

---

## 9. Live Coding

### Basic Live Coding

```turmeric
(def session (make-live-session 120.0))
(def player (make-player session "sine" (const 440.0) 120.0))

;; Update pattern live
(player-update player (cycle 220.0 440.0 880.0))

;; Update to transformed pattern
(player-update player (slow 2 (cycle 220.0 440.0 880.0)))
```turmeric

### d1 and hush

```turmeric
;; Set default pattern (d1)
(def d1-pattern (stack 
  (cycle 220.0 440.0)
  (kick [0.0 2.0])))

(def d1 (swap-patterns player d1-pattern))

;; Silence all (hush)
(def hush (swap-patterns player (const 0)))
```

### Pattern Swapping

```turmeric
(def patterns {
  :bass (slow 4 (cycle 36 40 43 48))
  :melody (cycle 60 64 67 72)
  :drums (stack (kick [0.0 2.0]) (snare [1.0 3.0]))
})

;; Swap to bass
(swap-patterns player (get patterns :bass))

;; Swap to melody
(swap-patterns player (get patterns :melody))
```turmeric

### Mini-Notation Live

```turmeric
;; Use mini-notation for quick pattern entry
(player-update player (s "1 2 3 4"))

;; With operators
(player-update player (s "1*2 3*0.5"))

;; Drum notation
(player-update player (d "bd sd hh cp"))
```

### Stateful Patterns

```turmeric
;; Counter
(def counter (counter-pattern 10))

;; LFO with accumulating phase
(def lfo (make-stateful-pattern 0.0 
  (fn [phase time] 
    [(+ phase 0.01) (const (* 10.0 (math/sin phase)))])))

;; Use in live coding
(let [[new-state new-pat] (step-stateful counter 0.0)]
  (player-update player (add (const 60) new-pat)))
```text

---

## 10. Performance Tips

### Pattern Caching

```turmeric
;; Cache expensive patterns
(def cached-melody (cached (complex-generative-pattern) 1024))
```

### Pattern Inlining

```turmeric
;; Inline simple patterns
(def inlined (inline-pattern (slow 2 (cycle 1 2))))
```turmeric

### Pattern Fusion

```turmeric
;; Fuse consecutive operations
(def fused (fuse-pattern (slow 2 (fast 2 (cycle 1 2)))))
```

### Avoid Unnecessary Computations

```turmeric
;; Pre-compute values
(def precomputed (const (map (fn [x] (* x 2)) (range 100))))

;; Use simple patterns when possible
(def simple (const 440.0))  ;; Faster than (cycle 440.0)
```text

---

## Quick Reference

### Common Patterns Cheat Sheet

| Pattern | Code |
|---------|------|
| Constant | `(const x)` or `(P x)` |
| Cycle | `(cycle 1 2 3)` or `(P 1 2 3)` |
| Phase | `(phase)` or `(phase n)` |
| Slow | `(slow 2 p)` |
| Fast | `(fast 2 p)` |
| Shift | `(shift 0.5 p)` |
| Sequence | `(seq p1 p2 p3)` |
| Stack | `(stack p1 p2)` |
| Sum | `(sum p1 p2)` |
| Every | `(every 2 p 0)` |
| Euclidean | `(euclid 8 3)` |
| Jitter | `(jitter p amount base)` |
| Sine | `(sine freq amp)` |
| Note | `(note-pattern pitch vel dur gate)` |
| Chord | `(chord-pattern n1 n2 n3)` |
| Arpeggio | `(arp-pattern chord speed)` |
| Kick | `(kick [0 2])` |
| Snare | `(snare [1 3])` |
| Hat | `(hat [0 0.5 1 1.5])` |

---

## Further Reading

- [User Guide](tidal-guide.md) - Detailed introduction and concepts
- [API Reference](tidal-api.md) - Complete API documentation
- [SCSCM Library](https://github.com/rjungemann/turmeric2/blob/main/docs/scscm-hcsynth-livecoding-plan.md) - SuperCollider integration

---

*Generated for Turmeric project - TidalCycles-like DSL Cookbook*
