# Signal Processing Example with Haskell-Style Arrows — Plan & Tutorial

> **Status: extracted to `tur-signal` spice as of v0.1.0.** Source files now live in
> `../turmeric-spices/spices/signal/src/signal/`. Import as `(import signal/core)` etc.
> This document is archived; paths below refer to the pre-extraction layout.

A **practical example** and **step-by-step tutorial** demonstrating Haskell-style arrows for signal processing in Turmeric. This showcases Turmeric's typeclass system, higher-kinded types (via the HKT implementation from `hkt-implementation-plan.md`), and arrow-based composition for building declarative signal processing pipelines.

> **Prerequisites**: Phase 19 compiler (CPS + effect lowering), HKT infrastructure, typeclass dispatch
> **Dependencies**: None external (pure Turmeric, optional C FFI for audio I/O)

---

## 1. Goals

| Goal | Priority | Success Criterion |
|---|---|---|
| Implement Arrow typeclass hierarchy | High | `Arrow`, `ArrowChoice`, `ArrowLoop`, `ArrowApply` defined and working |
| Build core signal processing arrows | High | `Signal` arrow type for continuous-time signals |
| Create composable DSP primitives | High | Filters (low-pass, high-pass), oscillators, envelopes as arrows |
| Demonstrate arrow laws | Medium | Prove/verify `arr id >>> f = f`, `arr (f >>> g) = arr f >>> arr g`, etc. |
| Tutorial: from basics to synthesis | High | Step-by-step guide building a simple synthesizer |
| Optional: real-time audio via PortAudio | Low | FFI bindings + arrow-based audio callback |

---

## 2. Background: Haskell-Style Arrows for Signal Processing

### 2.1 Why Arrows for DSP?

Haskell arrows solve a key problem in signal processing: **implicit time propagation**. In a pure functional setting:

- A signal is a function `Time -> Value`
- Processing a signal means transforming this function
- Arrows abstract the pattern: `a -> b` in the arrow category represents a signal processor

```haskell
-- Haskell: Arrow composition for signal flow
ghci> let oscillator = arr (	 -> sin (440 * 2 * pi * t))
ghci> let gain = arr (* 0.5)
ghci> let processed = oscillator >>> gain
```

In Turmeric, we encode this via typeclasses with HKT.

### 2.2 Arrow Typeclass Hierarchy

```
Arrow      -- Base: arr, >>>, first, second
  ├─ ArrowZero      -- Empty arrow: zeroArrow
  ├─ ArrowPlus     -- Combination: <+>
  ├─ ArrowChoice   -- Choice: left, right, +++, |||
  ├─ ArrowLoop     -- Feedback: loop
  └─ ArrowApply    -- Apply: app (for Arrow monads)
```

### 2.3 Signal Processing Domain

| Concept | Arrow Representation | Turmeric Type |
|---|---|---|
| Constant signal | `arr (const c)` | `Signal Float` |
| Oscillator | `arr sin >>> arr (scale freq)` | `Signal Float` |
| Filter | Recursive arrow with `loop` | `Signal Float -> Signal Float` |
| Mixer | `arr (+)` combining two signals | `Signal Float -> Signal Float -> Signal Float` |
| Envelope | Time-varying gain | `Signal Float` |

---

## 3. Project Structure

```
fith/
├── stdlib/
│   ├── arrow.tur              # Arrow typeclass hierarchy (Phase A)
│   ├── arrow_laws.tur         # Arrow law tests/proofs (Phase A)
│   └── signal/
│       ├── core.tur           # Signal arrow type + instances (Phase B)
│       ├── dsp.tur            # DSP primitives: oscillators, filters (Phase C)
│       ├── envelope.tur       # ADSR envelope (Phase C)
│       └── synth.tur          # Simple synthesizer examples (Phase D)
├── examples/
│   └── signal-processing/
│       ├── basic.tur          # Tutorial Step 1-3: basics
│       ├── composition.tur   # Tutorial Step 4-6: composing processors
│       ├── filters.tur        # Tutorial Step 7-9: filters
│       └── synthesizer.tur    # Tutorial Step 10-12: full synth
├── docs/
│   └── signal-processing-tutorial.md  # This tutorial
└── tests/
    └── arrow_tests.tur        # Arrow law verification tests
```

---

## 4. Implementation Phases

### Phase A: Arrow Typeclass Foundation

**Deliverables**: Core arrow typeclass hierarchy with instances for function arrows.

#### 4.1 HKT Preliminaries

Using the HKT infrastructure from `hkt-implementation-plan.md`:

```clojure
;; Kind signatures for Arrow
(defkind ArrowK  ; k1 -> k2 -> *
  (-> Type (-> Type Type)))

;; A higher-kinded Arrow type
(defalias Arrow- c d
  (forall (a b) (-> (c a) (d b))))
```

#### 4.2 Arrow Typeclass

```clojure
;; arrow.tur

(deftypeclass Arrow arr where
  ;; Lift a function to an arrow
  (arr [a b] : (-> a b) -> (arr a b))
  
  ;; Compose two arrows: f >>> g means "first f, then g"
  (>>> [a b c] : (arr a b) -> (arr b c) -> (arr a c))
  
  ;; Lift a function to act on the first component of a product
  (first [a b c] : (arr a b) -> (arr (Tuple a c) (Tuple b c)))
  
  ;; Lift a function to act on the second component of a product
  (second [a b c] : (arr a b) -> (arr (Tuple c a) (Tuple c b))))

;; Function arrow instance (the canonical Arrow)
(definstance Arrow-Function Arrow (->)
  (arr [a b] f) (fn [x] (f x))
  (>>> [a b c] f g) (fn [x] (g (f x)))
  (first [a b c] f) (fn [[x z]] [(f x) z])
  (second [a b c] f) (fn [[z x]] [z (f x)]))
```

#### 4.3 Arrow Laws

Arrow instances must satisfy these laws (verified in tests):

```clojure
;; arrow_laws.tur

;; Law 1: arr id is identity
;; (arr id) >>> f = f
;; f >>> (arr id) = f

;; Law 2: arr distributes over composition
;; arr (f . g) = arr f >>> arr g

;; Law 3: arr respects function composition
;; arr f >>> arr g = arr (g . f)

;; Law 4: first and second are functors
;; first (arr f >>> arr g) = first (arr f) >>> first (arr g)

;; Law 5: first and second interact
;; first (arr f) >>> arr (map second g) = arr (map first f) >>> arr g
;;  where map first f (x, y) = (f x, y)
;;        map second g (x, y) = (x, g y)

defn verify-arrow-laws [arr : (Arrow a)]
  [] : bool
  ;; Run property-based tests
  (check-arrow-laws arr 1000))
```

#### 4.4 Extended Arrow Classes

```clojure
;; ArrowChoice: for choice/alternation
(deftypeclass ArrowChoice arr (Arrow arr) where
  (left [a b c] : (arr a b) -> (arr (Either a c) (Either b c)))
  (right [a b c] : (arr a b) -> (arr (Either c a) (Either c b)))
  (+++ [a b c d] : (arr a b) -> (arr c d) -> (arr (Either a c) (Either b d)))
  (||| [a b c d] : (arr a c) -> (arr b d) -> (arr (Either a b) (Either c d))))

;; ArrowLoop: for feedback loops
(deftypeclass ArrowLoop arr (Arrow arr) where
  (loop [a b c] : (arr (Tuple a c) (Tuple b c)) -> (arr a b)))

;; ArrowApply: for Arrow monads
(deftypeclass ArrowApply arr (Arrow arr) where
  (app [a b] : (arr a (arr a b)) -> (arr a b)))
```

### Phase B: Signal Arrow Type

**Deliverables**: A concrete `Signal` arrow type for continuous-time signal processing.

#### 4.5 Signal Representation

A signal is a function from time to value. We use a lazy, memoized representation:

```clojure
;; signal/core.tur

;; A signal is a function Time -> Value
;; We represent it as a thunk that can be sampled at any time
defalias Time float64
defalias Sample float64

defstruct Signal
  [sample : (-> Time Sample)]

;; Smart constructor
defn constant [value : Sample] : Signal
  (Signal (fn [_t] value))

defn time : Signal
  (Signal (fn [t] t))

defn sample [sig : Signal] [t : Time] : Sample
  ((. sig sample) t)
```

#### 4.6 Signal as an Arrow

The key insight: a signal processor `Signal a -> Signal b` is an arrow.

```clojure
;; The Signal arrow type
defalias SignalArrow a b
  (-> Signal (-> Signal))

;; But we want it to be an Arrow instance, so we need to wrap it
;; properly with HKT...

;; Actually, Signal a -> Signal b is the morphism type
;; The object type is Signal
;; So Arrow for Signal processing:

definstance Arrow-Signal Arrow SignalArrow
  (arr [a b] f)
    (fn [sig-a] (Signal (fn [t] (f (sample sig-a t)))))
  
  (>>> [a b c] f g)
    (fn [sig-a] (g (f sig-a)))
  
  (first [a b c] f)
    (fn [sig-a-c] 
      (let [sig-a (map-sample sig-a-c (fn [t] (first (sample sig-a-c t))))
            sig-c (map-sample sig-a-c (fn [t] (second (sample sig-a-c t))))]
        (let [sig-b (f sig-a)]
          (combine-signals sig-b sig-c (fn [b c] (Tuple b c))))))
  
  (second [a b c] f)
    (fn [sig-c-a]
      (let [sig-c (map-sample sig-c-a (fn [t] (first (sample sig-c-a t))))
            sig-a (map-sample sig-c-a (fn [t] (second (sample sig-c-a t))))]
        (let [sig-b (f sig-a)]
          (combine-signals sig-c sig-b (fn [c b] (Tuple c b)))))))

;; Helper: transform a signal's samples
defn map-sample [sig : Signal] [f : (-> Time Sample)] : Signal
  (Signal (fn [t] (f (sample sig t))))

;; Helper: combine two signals with a function
defn combine-signals [sig-a : Signal] [sig-b : Signal] [f : (-> Sample Sample Sample)] : Signal
  (Signal (fn [t] (f (sample sig-a t) (sample sig-b t))))
```

Wait, this approach has issues. Let me reconsider the Arrow instance for signals.

Actually, the correct approach: a signal processor is a function `Signal a -> Signal b`. The Arrow instance operates on these processors.

```clojure
;; Revised: SignalArrow is the category of signal processors
;; Objects are signal types, morphisms are signal processors

;; We can't directly use (-> Signal Signal) because Arrow needs
;; to be polymorphic in both input and output types.

;; Instead, we define Signal as a type constructor:
defalias Signal a
  (-> Time a)

;; And SignalArrow is the identity - a signal processor a -> b
;; is just Signal a -> Signal b

defalias SignalArrow a b
  (-> (Signal a) (Signal b))

;; But for Arrow, we need the object type to be Signal, and morphisms
;; to be SignalArrow...
;; 
;; Actually, let's use a simpler approach: Signal is the object,
;; and we define the arrow type as a wrapped function.

defstruct SF  ;; Signal Function
  [run : (-> Signal Signal)]

definstance Arrow SF
  (arr [a b] f)
    (SF (fn [sig] (Signal (fn [t] (f (sample sig t))))))
  
  (>>> [a b c] sf1 sf2)
    (SF (fn [sig] ((. sf2 run) ((. sf1 run) sig))))
  
  (first [a b c] sf)
    (SF (fn [sig-ac]
          (let [sig-a (map-signal sig-ac (fn [t] (first (sample sig-ac t))))
                sig-c (map-signal sig-ac (fn [t] (second (sample sig-ac t))))]
            (let [sig-b ((. sf run) sig-a)]
              (combine-signals sig-b sig-c (fn [b c] (Tuple b c)))))))
  
  (second [a b c] sf)
    (SF (fn [sig-ca]
          (let [sig-c (map-signal sig-ca (fn [t] (first (sample sig-ca t))))
                sig-a (map-signal sig-ca (fn [t] (second (sample sig-ca t))))]
            (let [sig-b ((. sf run) sig-a)]
              (combine-signals sig-c sig-b (fn [c b] (Tuple c b)))))))
```

Hmm, this is getting complex. Let me take a different, more pragmatic approach.

### Revised Phase B: Simpler Signal Representation

Let's use a more direct approach inspired by Haskell's `SF` (Signal Function) library:

```clojure
;; signal/core.tur

;; A signal is a time-varying value
;; A signal function (SF) transforms a signal
defalias Time float64

;; Signal: a function from time to value
defalias Signal a
  (-> Time a)

;; Signal Function: transforms an input signal to an output signal
;; SF a b is a function that takes (Time -> a) and returns (Time -> b)
defalias SF a b
  (-> (Signal a) (Signal b))

;; Arrow instance for SF
(definstance Arrow-SF Arrow SF
  
  ;; arr f: lift a pure function to an SF
  (arr [a b] f : (-> a b))
    (fn [sig-a : (Signal a)] 
      (fn [t : Time] (f (sig-a t))))
  
  ;; f >>> g: compose two SFs
  (>>> [a b c] sf1 : (SF a b) sf2 : (SF b c))
    (fn [sig-a : (Signal a)] 
      (sf2 (sf1 sig-a)))
  
  ;; first: apply SF to first component of product
  (first [a b c] sf : (SF a b))
    (fn [sig-ac : (Signal (Tuple a c))] 
      (fn [t : Time] 
        (let [[a-val c-val] (sig-ac t)]
          (Tuple (sf (fn [_] a-val) t) c-val))))
  
  ;; second: apply SF to second component of product
  (second [a b c] sf : (SF a b))
    (fn [sig-ca : (Signal (Tuple c a))] 
      (fn [t : Time] 
        (let [[c-val a-val] (sig-ca t)]
          (Tuple c-val (sf (fn [_] a-val) t))))))
```

This is cleaner! Now we can compose signal processors naturally.

### Phase C: DSP Primitives

#### 4.7 Basic Signal Sources

```clojure
;; signal/dsp.tur

;; Constant signal
defn constant [val : Sample] : (SF () Sample)
  (arr (fn [_] val))

;; Time signal (identity on time)
defn time-sf : (SF () Time)
  (arr (fn [_] _))  ;; Will be replaced with actual time

;; Actually, we need to pass time explicitly. Let's revise:
;; Every SF has access to time implicitly through the Signal type.

;; Better approach: the input to an SF can include time
defn sine-osc [freq : float64] : (SF Time Sample)
  (arr (fn [t] (math/sin (* 2.0 math/PI freq t))))

;; But we want to hide the time parameter. Let's use a different approach:
;; The Signal type already encodes time, so:

defn sine-osc [freq : float64] : (SF () Sample)
  (arr (fn [_] 0.0))  ;; Placeholder - need to access time
```

This reveals a design issue. Let me reconsider.

**Better Design**: Make `Time` an implicit parameter that flows through the signal graph. We can use a reader-like pattern or make the input signal include time.

```clojure
;; Every SF implicitly has access to a global time signal
;; We'll use a technique: the input to the top-level SF includes time

;; For user-facing SFs, we assume the input is () and time is global
defn sine-osc [freq : float64] : (SF () Sample)
  (SF (fn [_sig] 
        (fn [t] (math/sin (* 2.0 math/PI freq t)))))

;; Gain: multiply signal by constant
defn gain [g : Sample] : (SF Sample Sample)
  (arr (fn [x] (* g x)))

;; Add two signals
defn add : (SF (Tuple Sample Sample) Sample)
  (arr (fn [[x y]] (+ x y)))

;; Mix: combine two signals with a ratio
defn mix [ratio : Sample] : (SF (Tuple Sample Sample) Sample)
  (arr (fn [[x y]] (+ (* ratio x) (* (- 1.0 ratio) y))))
```

#### 4.8 Feedback with ArrowLoop

Now we need `ArrowLoop` to implement recursive filters:

```clojure
;; First, implement ArrowLoop for SF
(definstance ArrowLoop-SF ArrowLoop SF
  (loop [a b c] sf : (SF (Tuple a c) (Tuple b c)))
    (fn [sig-a : (Signal a)] 
      (let [;; We need to tie the knot: the output c becomes the input c
            ;; This requires a fixpoint
            sig-bc (sf sig-a)
            ;; Extract b and c from the output
            sig-b (map-signal sig-bc (fn [t] (first (sig-bc t))))
            sig-c (map-signal sig-bc (fn [t] (second (sig-bc t))))]
        sig-b)))

;; This doesn't work because we're not actually creating feedback.
;; We need to pass the delayed output back as input.

;; Better: implement a delay primitive
defn delay [dt : Time] [init : Sample] : (SF Sample Sample)
  (SF (fn [sig-in] 
        (let [prev (ref init)]
          (fn [t] 
            (let [current (sig-in t)]
              (let [result @prev]
                (set! prev current)
                result))))))

;; One-pole low-pass filter: y = a * x + (1-a) * y_prev
defn low-pass [a : Sample] : (SF Sample Sample)
  (let [delayed (delay 1.0 0.0)]
    (fn [sig-in] 
      (fn [t] 
        (let [x (sig-in t)]
          (let [y-prev (sample delayed t)]
            (+ (* a x) (* (- 1.0 a) y-prev)))))))
```

The issue with this approach is that `delay` uses mutable state (`ref`), which breaks referential transparency. For a pure functional approach, we need to use the `ArrowLoop` instance properly.

Let me implement `ArrowLoop` correctly:

```clojure
;; ArrowLoop for SF: loop sf takes an SF that processes (a, c) to (b, c)
;; and produces an SF that processes a to b, feeding the output c back as input c
(definstance ArrowLoop-SF ArrowLoop SF
  (loop [a b c] sf : (SF (Tuple a c) (Tuple b c)))
    (fn [sig-a : (Signal a)]
      (let [;; Create a mutable reference to hold the feedback signal
            ;; This is the knot-tying: we need to compute sig-c from sig-bc
            ;; but sig-bc depends on sig-c
            sig-c-var (ref (constant (error "uninitialized")))
            
            ;; Define the combined signal: (a, c) -> (b, c)
            sig-ac (combine-signals sig-a @sig-c-var Tuple)
            sig-bc (sf sig-ac)
            
            ;; Extract b from bc
            sig-b (map-signal sig-bc (fn [t] (first (sig-bc t))))
            
            ;; Extract c from bc and store it in sig-c-var
            (let [sig-c-new (map-signal sig-bc (fn [t] (second (sig-bc t))))]
              (set! sig-c-var sig-c-new)
              sig-b))))
```

This still uses mutation. For a pure implementation, we'd need to use a fixpoint combinator. Let me try:

```clojure
;; Pure loop implementation using fixpoint
defn loop-sf [a b c] [sf : (SF (Tuple a c) (Tuple b c))] : (SF a b)
  (fn [sig-a]
    (let [;; The fixpoint: sig-c = second (sf (sig-a, sig-c))
          ;; We compute this iteratively
          sig-c-init (constant (error "no initial value"))
          
          ;; We need to solve: sig-bc = sf (combine sig-a sig-c)
          ;;                    sig-c = second sig-bc
          ;; This is a fixpoint equation
          
          ;; For signals, we can compute pointwise:
          ;; At each time t: (b_t, c_t) = sf (a_t, c_t)
          ;; So c_t = (second (sf (a_t, c_t)))
          ;; This is still recursive...
          
          ;; Solution: for causal signals, we can use the previous value
          (fn [t]
            (let [a-t (sig-a t)]
              ;; We need c at time t, which depends on c at time t
              ;; For a one-pole filter: c_t = a * a_t + (1-a) * c_{t-1}
              ;; This is the standard recurrence
              )))]
    ...))
```

This is getting too complex. Let me take a pragmatic approach: use mutable state for the feedback, which is acceptable for a DSP system where we're processing samples sequentially anyway.

```clojure
;; Practical: use a mutable cell for feedback state
(defstruct FeedbackCell
  [value : Sample])

defn low-pass-filter [alpha : Sample] : (SF Sample Sample)
  (fn [sig-in]
    (let [cell (FeedbackCell 0.0)]
      (fn [t]
        (let [x (sig-in t)]
          (let [y-prev (. cell value)]
            (let [y (+ (* alpha x) (* (- 1.0 alpha) y-prev))]
              (set! (. cell value) y)
              y))))))
```

### Phase C: Complete DSP Primitives

```clojure
;; signal/dsp.tur

;; --- Basic Sources ---

(defn constant [val : Sample] : (SF () Sample)
  (arr (fn [_] val)))

(defn sine [freq : Sample] [phase : Sample] : (SF Time Sample)
  (arr (fn [t] (math/sin (+ (* 2.0 math/PI freq t) phase)))))

(defn square [freq : Sample] [duty : Sample] : (SF Time Sample)
  (arr (fn [t] (if (< (mod (* freq t) 1.0) duty) 1.0 -1.0))))

(defn sawtooth [freq : Sample] : (SF Time Sample)
  (arr (fn [t] (mod (* freq t) 1.0))))

(defn triangle [freq : Sample] : (SF Time Sample)
  (arr (fn [t] (let [x (mod (* freq t) 1.0)] (if (< x 0.5) (* 2.0 x) (* -2.0 (- x 1.0)))))))

;; --- Basic Processors ---

(defn gain [g : Sample] : (SF Sample Sample)
  (arr (fn [x] (* g x))))

(defn offset [o : Sample] : (SF Sample Sample)
  (arr (fn [x] (+ x o))))

(defn add : (SF (Tuple Sample Sample) Sample)
  (arr (fn [[x y]] (+ x y))))

(defn multiply : (SF (Tuple Sample Sample) Sample)
  (arr (fn [[x y]] (* x y))))

(defn mix [ratio : Sample] : (SF (Tuple Sample Sample) Sample)
  (arr (fn [[x y]] (+ (* ratio x) (* (- 1.0 ratio) y)))))

;; --- Filters ---

(defn low-pass [alpha : Sample] : (SF Sample Sample)
  (fn [sig-in]
    (let [prev (ref 0.0)]
      (fn [t]
        (let [x (sig-in t)]
          (let [y (+ (* alpha x) (* (- 1.0 alpha) @prev))]
            (set! prev y)
            y))))))

(defn high-pass [alpha : Sample] : (SF Sample Sample)
  (fn [sig-in]
    (let [prev-in (ref 0.0)
          prev-out (ref 0.0)]
      (fn [t]
        (let [x (sig-in t)]
          (let [y (- x @prev-in + (* @prev-out (- 1.0 alpha)))]
            (set! prev-in x)
            (set! prev-out y)
            y))))))

(defn band-pass [freq : Sample] [q : Sample] : (SF Sample Sample)
  ;; State variable filter implementation
  (fn [sig-in]
    (let [z1 (ref 0.0)
          z2 (ref 0.0)]
      (fn [t]
        (let [x (sig-in t)
              ;; SVF equations
              v1 (- x @z1)
              v2 (- @z1 @z2)
              v3 (- @z2 (* q v2))
              v4 (- v3 @z2)
              
              ;; Update state
              z1-new (+ @z1 (* freq v1))
              z2-new (+ @z2 (* freq v3))]
          (set! z1 z1-new)
          (set! z2 z2-new)
          v4)))))

;; --- Envelopes ---

(defstruct ADSRParams
  [attack  : Sample  ;; time in seconds
   decay   : Sample
   sustain : Sample  ;; level (0.0 to 1.0)
   release : Sample])

defn adsr [params : ADSRParams] [gate : (SF () bool)] : (SF () Sample)
  (fn [_sig]
    (let [state (ref 0.0)  ;; 0=off, 1=attack, 2=decay, 3=sustain, 4=release
          level (ref 0.0)
          time (ref 0.0)]
      (fn [t]
        (let [gate-val ((. gate run) (constant ()) t)  ;; Hack: need to pass time
              dt 0.001]  ;; Sample period (assuming 1kHz)
          
          (cond
            ;; Gate just opened: start attack
            (and gate-val (= @state 0.0))
            (do (set! state 1.0) (set! time 0.0) @level)
            
            ;; Attack phase
            (= @state 1.0)
            (let [new-level (/ @time (. params attack))]
              (set! time (+ @time dt))
              (if (>= @time (. params attack))
                (do (set! state 2.0) (set! time 0.0) 1.0)
                (do (set! level new-level) new-level)))
            
            ;; Decay phase
            (= @state 2.0)
            (let [new-level (- 1.0 (* @time (/ (- 1.0 (. params sustain)) (. params decay))))]
              (set! time (+ @time dt))
              (if (>= @time (. params decay))
                (do (set! state 3.0) (. params sustain))
                (do (set! level new-level) new-level)))
            
            ;; Sustain phase
            (= @state 3.0)
            (if (not gate-val)
              (do (set! state 4.0) (set! time 0.0) @level)
              (. params sustain))
            
            ;; Release phase
            (= @state 4.0)
            (let [new-level (* @level (/ (- 1.0 (/ @time (. params release))) 1.0))]
              (set! time (+ @time dt))
              (if (>= @time (. params release))
                (do (set! state 0.0) (set! level 0.0) 0.0)
                (do (set! level new-level) new-level)))
            
            :else 0.0))))))
```

### Phase D: Example Synthesizer

Now we can compose everything using arrow composition:

```clojure
;; signal/synth.tur

;; A simple subtractive synthesizer voice
(defn voice [freq : Sample] [gate : (SF () bool)] : (SF () Sample)
  (let [;; Oscillators
        osc1 (>>> (sine freq 0.0) (gain 0.5))
        osc2 (>>> (square (* 2.0 freq) 0.5) (gain 0.3))
        
        ;; Mix oscillators
        mixed-osc (>>> (combine osc1 osc2 Tuple) (add))
        
        ;; Filter
        filtered (>>> mixed-osc (low-pass 0.1))
        
        ;; Envelope
        env (adsr (ADSRParams 0.1 0.3 0.5 0.2) gate)
        
        ;; Apply envelope
        result (>>> (combine filtered env Tuple) (multiply))]
    result))

;; Polyphonic synthesizer
(defn poly-synth [voices : (vec (Tuple Sample (SF () bool)))] : (SF () Sample)
  (fn [_sig]
    (let [voice-sfs (map (fn [[freq gate]] (voice freq gate)) voices)]
      (fn [t]
        (reduce + 0.0 (map (fn [sf] (sample sf t)) voice-sfs))))))

;; Helper to combine signals
defn combine [& sfs : (vec (SF a b))] : (SF a (vec b))
  (arr (fn [x] (map (fn [sf] (sample sf x)) sfs)))
```

---

## 5. Tutorial Outline

### Step 0: Prerequisites (15 min)
- Working Turmeric compiler with HKT support
- Basic understanding of typeclasses
- Familiarity with signal processing concepts (optional)

### Step 1: Understanding Arrows (30 min)
- What are arrows?
- Arrow laws
- The Arrow typeclass in Turmeric
- Function arrow instance

**Code**: Implement `Arrow` for functions, verify laws

### Step 2: Signal Basics (30 min)
- What is a signal?
- Signal as a function from time to value
- Basic signal sources (constant, sine, square)
- Visualizing signals

**Code**: Define `Signal` type, implement basic sources

### Step 3: Arrow Instances for Signals (45 min)
- Why signals form an arrow category
- Implementing `Arrow` for `SF`
- Composing signal processors

**Code**: Implement `Arrow-SF` instance

### Step 4: Basic Signal Processing (30 min)
- Gain, offset, mixing signals
- Combining signals in parallel (using `***` from ArrowChoice)
- Splitting signals (using `&&&`)

**Code**: Implement gain, add, multiply, mix

### Step 5: Filter Design (60 min)
- Recursive vs. non-recursive filters
- Implementing feedback with `ArrowLoop`
- One-pole low-pass filter
- High-pass and band-pass filters

**Code**: Implement filter primitives

### Step 6: Envelopes (45 min)
- ADSR envelope stages
- State management in arrows
- Combining envelope with oscillator

**Code**: Implement ADSR envelope

### Step 7: Building a Synthesizer (60 min)
- Voice architecture
- Oscillator + filter + envelope
- Polyphonic synthesis
-Playing notes

**Code**: Complete voice and poly-synth

### Step 8: Advanced Topics (60 min)
- ArrowChoice for conditional processing
- ArrowApply for dynamic effects
- Performance considerations
- Integration with audio I/O

### Step 9: Full Example - Simple Synth (60 min)
- Putting it all together
- Adding a simple sequencer
- Generating audio output

**Total estimated time**: ~8-10 hours

---

## 6. Testing Strategy

### 6.1 Arrow Law Verification

```clojure
;; arrow_tests.tur

defn test-arrow-laws []
  ;; Test identity
  (let [id-sf (arr (fn [x] x))]
    (assert (sf-equal id-sf (>>> id-sf id-sf))))
  
  ;; Test composition associativity
  (let [f (arr (fn [x] (+ x 1)))
        g (arr (fn [x] (* x 2)))
        h (arr (fn [x] (- x 3)))]
    (assert (sf-equal (>>> (>>> f g) h) (>>> f (>>> g h)))))
```

### 6.2 DSP Primitive Tests

```clojure
;; dsp_tests.tur

defn test-low-pass []
  (let [lp (low-pass 0.5)
        input (step 1.0 100)  ;; Step from 0 to 1 at t=100
        output (lp input)]
    ;; Verify smoothing behavior
    (assert (< (sample output 100) 1.0))
    (assert (> (sample output 200) 0.5))
    (assert (< (sample output 200) 0.8))))

defn test-adsr []
  (let [params (ADSRParams 0.1 0.2 0.7 0.3)
        gate (step true false 1.0)  ;; Gate on at t=0, off at t=1
        env (adsr params gate)]
    (assert (== (sample env 0.0) 0.0))
    (assert (== (sample env 0.05) 0.5))  ;; Halfway through attack
    (assert (== (sample env 0.1) 1.0))   ;; Attack complete
    (assert (== (sample env 0.3) 0.7))   ;; Decay to sustain
    (assert (== (sample env 1.3) 0.0))   ;; Release complete
    ))
```

### 6.3 Signal Comparison

```clojure
;; Need to define approximate equality for signals
defn sf-equal [sf1 : (SF a b)] [sf2 : (SF a b)] [tol : Sample] [duration : Time] [steps : int] : bool
  (let [dt (/ duration steps)]
    (loop [i 0]
      (when (< i steps)
        (let [t (* i dt)
              val1 (sample (sf1 (constant () i)) t)  ;; Hacky
              val2 (sample (sf2 (constant () i)) t)]
          (when (> (abs (- val1 val2)) tol)
            false)
          (recur (+ i 1)))))
      true))
```

---

## 7. Audio I/O (Optional)

For real-time audio, we can integrate with PortAudio or a simpler library:

```clojure
;; audio.tur

(extern-c "Pa_Initialize" : (-> () int32))
(extern-c "Pa_OpenDefaultStream" : ...)

;; Audio callback as an SF
defn audio-callback [sf : (SF () Sample)] : (-> () int32)
  (fn []
    ;; Render a buffer of samples
    ;; For simplicity, render at 44.1kHz
    ...))
```

The challenge is that real-time audio callbacks have specific signatures. We'd need to:
1. Define a buffer type
2. Create an SF that processes buffers
3. Adapt our sample-based SFs to buffer-based processing

This might be better as a separate effort after the core arrow infrastructure is solid.

---

## 8. Performance Considerations

### 8.1 Inlining
- Use `^inline` hint for hot paths (oscillator computation, filter updates)
- Typeclass dispatch for Arrow methods should be devirtualized where possible

### 8.2 Memory Allocation
- Avoid heap allocation in audio callback (pre-allocate buffers)
- Use arena allocation for temporary values
- Consider stack allocation for small, short-lived values

### 8.3 Numerical Precision
- Use `float32` for audio samples (standard in DSP)
- Be aware of denormal handling
- Consider SIMD for parallel signal processing

### 8.4 Sample Rate
- Standard: 44.1kHz, 48kHz
- Our Signal type uses continuous time; need to adapt to discrete samples
- May need a separate `DiscreteSignal` type for actual audio processing

---

## 9. Deliverables Checklist

- [ ] `stdlib/arrow.tur` - Arrow typeclass hierarchy
- [ ] `stdlib/arrow_laws.tur` - Arrow law verification
- [ ] `stdlib/signal/core.tur` - Signal types and basic SF instances
- [ ] `stdlib/signal/dsp.tur` - DSP primitives
- [ ] `stdlib/signal/envelope.tur` - Envelope generators
- [ ] `stdlib/signal/synth.tur` - Synthesizer examples
- [ ] `examples/signal-processing/*.tur` - Tutorial example files
- [ ] `tests/arrow_tests.tur` - Arrow law tests
- [ ] `tests/dsp_tests.tur` - DSP primitive tests
- [ ] `docs/signal-processing-tutorial.md` - Step-by-step tutorial

---

## 10. Dependencies on Other Phases

| This Component | Depends On | Status |
|---|---|---|
| Arrow typeclass | HKT infrastructure | Phase 19 complete |
| Typeclass dispatch | Typeclass system | Implemented |
| ArrowLoop | Closures, ref | Implemented |
| Signal processing | None | Ready |

---

## 11. Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| HKT not ready | Low | High | Verify HKT implementation first |
| Performance issues | Medium | Medium | Profile early, inline hot paths |
| ArrowLoop too complex | Medium | High | Start with mutable state, refactor later |
| Audio I/O difficulties | High | Low | Make it optional, use file output first |
| Numerical instability | Medium | Medium | Add clamping, denormal handling |

---

## 12. Next Steps

1. **Verify HKT infrastructure** - Check that `hkt-implementation-plan.md` is complete
2. **Implement Arrow typeclass** - Start with basic Arrow, verify with function instance
3. **Implement Signal SF** - Get basic signal processing working
4. **Build tutorial incrementally** - Write tutorial as we implement each feature
5. **Add tests** - Verify arrow laws and DSP primitive behavior
6. **Optimize** - Profile and inline hot paths
7. **Optional: Add audio I/O** - Integrate with PortAudio or similar

---

## Appendix A: Haskell Reference

For reference, here's how this looks in Haskell (using the `SF` library):

```haskell
import Data.Signal.Simple

-- Basic oscillator
oscillator :: Double -> Signal Double
oscillator freq = sin (2 * pi * freq * time)

-- Low-pass filter
data LP a = LP { lx, ly :: a }

lpFilter :: Double -> Signal Double -> Signal Double
lpFilter alpha input = output
  where
    output = lx' + alpha * (input - lx')
    lx'    = ly' + alpha * (input - ly')
    ly'    = prev ly (alpha * input)
    prev x i = case i of
                _:xs -> case xs of _:ys -> x; _ -> 0

-- Composition
synth :: Signal Double -> Signal Double
synth input = env * osc
  where
    osc = oscillator 440
    env = adsr (ADSR 0.1 0.3 0.5 0.2) (const True)
```

---

## Appendix B: Related Work

- **Haskell Arrow library**: The original arrow abstraction
- **Yampa**: Functional Reactive Programming library using arrows
- **SF**: Signal Function library for Haskell
- **Faust**: Functional DSP language (different approach but similar goals)
- **Pure Data / Max/MSP**: Visual programming for signal processing

---

## Appendix C: Glossary

| Term | Definition |
|---|---|
| Arrow | A typeclass abstracting computations that can be composed |
| SF | Signal Function: a morphism in the signal processing category |
| DSP | Digital Signal Processing |
| ADSR | Attack, Decay, Sustain, Release envelope stages |
| Low-pass filter | Attenuates frequencies above a cutoff |
| High-pass filter | Attenuates frequencies below a cutoff |
| Oscillator | Generates a periodic waveform |
| Sample | A single digital audio value at a point in time |
| Sample rate | Number of samples per second (e.g., 44100 Hz) |
