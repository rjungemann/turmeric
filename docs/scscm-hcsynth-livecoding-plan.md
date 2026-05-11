# SCSCM Library for Turmeric — hcsynth Live-Coding Integration Plan

> **Status**: Draft / Proposal  
> **Target**: Phase 20+ (Post-algebraic effects)  
> **Dependencies**: Phase 2 FFI (`extern-c`, inline-C), Phase 19 algebraic effects (for async), hcsynth OSC API  
> **Related**: [signal-processing-arrows-plan.md](./signal-processing-arrows-plan.md), [effects-plan.md](./effects-plan.md)

---

## 1. Overview

### 1.1 Goals

| Goal | Priority | Success Criterion |
|---|---|---|
| Turmeric ↔ hcsynth communication | High | OSC message send/receive working via hcsynth API |
| Live-coding workflow | High | REPL-like evaluation of Turmeric code that updates running hcsynth graph |
| Type-safe SCSCM bindings | High | Turmeric types map cleanly to SuperCollider server concepts |
| Minimal runtime overhead | Medium | No per-message allocation in hot path |
| Error handling | Medium | Graceful degradation when hcsynth connection drops |

### 1.2 Architecture Summary

```
┌─────────────────────────────────────────────────────────────────┐
│                         Turmeric Compiler                           │
├─────────────────────────────────────────────────────────────────┤
│  scscm.tur library                      │  User live-coding code   │
│  ┌─────────────────┐                    ┌───────────────────────┐ │
│  │ OSC type aliases │                    │ SynthDef definitions  │ │
│  │ Message builders │                    │ Pattern generation    │ │
│  │ hcsynth bindings │                    │ Parameter automation   │ │
│  └────────┬────────┘                    └───────────┬───────────┘ │
└───────────┼──────────────────────────────────────────┼────────────┘
            │                                              │
            ▼                                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Generated C + hcsynth C API                   │
├─────────────────────────────────────────────────────────────────┤
│  FFI thunks  ◄──────►  hcsynth OSC shim (HC_Wasm_OscShim)         │
│  Memory mgmt ◄──────►  hcsynth server process                      │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                        SuperCollider Server                        │
│  scsynth - real-time audio synthesis engine                       │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 Communication Model

- **Transport**: OSC (Open Sound Control) over UDP
- **hcsynth API**: Provides C API for sending OSC messages to scsynth
- **Direction**: 
  - Turmeric → hcsynth: Synth definitions, parameter changes, triggers
  - hcsynth → Turmeric: Synth status, trigger responses (future)

---

## 2. Phase Breakdown

### 2.1 Phase A — FFI Foundation

**Goal**: Establish basic FFI bindings to hcsynth OSC API.

**Prerequisites**: Phase 2 (`extern-c`, inline-C) complete.

**Tasks**:

- [ ] Survey hcsynth C API (`HC_Wasm_OscShim.h`, `HC_Wasm_OscShim.cpp`)
- [ ] Identify core OSC functions needed:
  - `hc_osc_send_message()` - send OSC message to scsynth
  - `hc_osc_bundle_begin()` / `hc_osc_bundle_end()` - batch messages
  - `hc_osc_alloc_message()` / `hc_osc_free_message()` - memory management
  - `hc_osc_add_int32()` / `hc_osc_add_float()` / `hc_osc_add_string()` / `hc_osc_add_blob()`
- [ ] Create `stdlib/scscm/ffi.tur` with type aliases and extern declarations:

```turmeric
;; Types
(extern-type osc_message_t)
(extern-type osc_bundle_t)

;; Core OSC functions
(extern-c hc_osc_alloc_message [^cstr address_pattern] :osc_message_t)
(extern-c hc_osc_free_message [^osc_message_t msg] :void)
(extern-c hc_osc_message_add_int32 [^osc_message_t ^int32] :int)
(extern-c hc_osc_message_add_float [^osc_message_t ^float] :int)
(extern-c hc_osc_message_add_string [^osc_message_t ^cstr] :int)
(extern-c hc_osc_message_add_blob [^osc_message_t ^ptr ^int] :int)
(extern-c hc_osc_send [^osc_message_t] :int)
```

- [ ] Create memory management helpers in Turmeric:
  - Safe wrappers that use `defer` for cleanup
  - `with-osc-message` macro for scoped message building

**Fixtures**:
- `scscm-ffi-basic.tur` — allocate message, add args, send, free
- `scscm-ffi-bundle.tur` — batch multiple messages

**Exit criterion**: Can send a simple `/s_new` message to scsynth from Turmeric.

---

### 2.2 Phase B — OSC Message Builders

**Goal**: Ergonomic OSC message construction in Turmeric.

**Tasks**:

- [ ] Define OSC type aliases in `stdlib/scscm/types.tur`:

```turmeric
;; OSC argument types
(def-type OscInt int32)
(def-type OscFloat float)
(def-type OscString cstr)
(def-type OscBlob slice<uint8>)
(def-type OscTime int64)  ;; NTP timestamp

;; OSC address patterns
(def-type OscAddress cstr)
```

- [ ] Create message builder API in `stdlib/scscm/msg.tur`:

```turmeric
;; Message constructor - returns a message that must be sent or freed
(defn osc-message [address & args]
  ```c
  osc_message_t* msg = hc_osc_alloc_message(address);
  if (!msg) return NULL;
  return (void*)msg;
  ```)

;; Add typed arguments
(defn osc-add! [msg arg]
  (cond
    (int? arg)  (hc_osc_message_add_int32 msg arg)
    (float? arg) (hc_osc_message_add_float msg arg)
    (cstr? arg)  (hc_osc_message_add_string msg arg)
    :else (panic "Unsupported OSC type")))

;; Send and free
(defn osc-send! [msg]
  (let [result (hc_osc_send msg)]
    (hc_osc_free_message msg)
    result))

;; Convenience: build, send, free in one
(defn osc-send [address & args]
  (let [msg (osc-message address)]
    (defer (hc_osc_free_message msg))
    (doseq [arg args] (osc-add! msg arg))
    (hc_osc_send msg)))
```

- [ ] Create `with-osc-message` macro for exception-safe message building:

```turmeric
(defmacro with-osc-message [[msg-var address] & body]
  `(let [~msg-var (osc-message ~address)]
     (defer (hc_osc_free_message ~msg-var))
     ~@body))
```

**Fixtures**:
- `scscm-msg-simple.tur` — send `/ping` message
- `scscm-msg-complex.tur` — send `/s_new` with multiple args
- `scscm-msg-batch.tur` — send bundle of messages

**Exit criterion**: Can construct and send any standard scsynth OSC message.

---

### 2.3 Phase C — SynthDef & Node API

**Goal**: High-level API for SuperCollider synth graph control.

**Tasks**:

- [ ] Define SynthDef and Node types in `stdlib/scscm/synth.tur`:

```turmeric
;; Node ID - corresponds to scsynth node IDs (int, 0 means allocate new)
(def-type NodeID int32)

;; Synth definition name
(def-type SynthDefName cstr)

;; Group and Synth node types
(def-type Group NodeID)
(def-type Synth NodeID)

;; Action types for /n_set, /n_free, etc.
(def-type NodeAction :enum [ :add-to-head :add-to-tail :add-before :add-after :replace ])
```

- [ ] Implement Synth creation and control:

```turmeric
;; Create a new synth instance
;; /s_new [synth-def-name node-id add-action ...args...]
(defn synth-new [def-name & args]
  (osc-send "/s_new" def-name 0 :add-to-tail args...))

;; Set synth parameters
;; /n_set [node-id ...param-value-pairs...]
(defn synth-set [node-id & param-values]
  (osc-send "/n_set" node-id param-values...))

;; Free a synth
;; /n_free [node-id]
(defn synth-free [node-id]
  (osc-send "/n_free" node-id))

;; Group control
(defn group-new [] (osc-send "/g_new" 0 :add-to-tail))
(defn group-add [group-id synth-def & args]
  (osc-send "/g_new" synth-def 0 :add-to-head group-id args...))
```

- [ ] Implement batch operations with bundles:

```turmeric
(defn bundle! [& messages]
  ```c
  osc_bundle_t* bundle = hc_osc_bundle_alloc();
  if (!bundle) return -1;
  // TODO: add messages to bundle
  int result = hc_osc_bundle_send(bundle);
  hc_osc_bundle_free(bundle);
  return result;
  ```)
```

**Fixtures**:
- `scscm-synth-basic.tur` — create and free a synth
- `scscm-synth-params.tur` — create synth, modify parameters
- `scscm-synth-batch.tur` — batch multiple synth creations

**Exit criterion**: Can create, control, and free synths on scsynth.

---

### 2.4 Phase D — Type-Safe Parameter System

**Goal**: Strongly-typed parameter control with units and ranges.

**Tasks**:

- [ ] Define parameter specification types in `stdlib/scscm/params.tur`:

```turmeric
(defstruct ParamSpec
  [name :cstr
   default :float
   min :float
   max :float
   unit :cstr    ;; "", "Hz", "dB", "sec", etc.
   warp :WarpType])  ;; :linear, :exp, :log, :db, etc.

(def-type WarpType :enum [ :linear :exp :log :db ])

;; Map of param name to ParamSpec for a SynthDef
(def-type SynthDefSpec (vec<ParamSpec>))
```

- [ ] Create parameter conversion utilities:

```turmeric
;; Convert value to OSC range with warping
(defn warp-value [spec value]
  (case (:warp spec)
    :linear value
    :exp (math/pow 2.0 (/ value 12.0))  ;; octave to ratio
    :log (* 12.0 (math/log2 value))     ;; ratio to octave
    :db (math/pow 10.0 (/ value 20.0))   ;; dB to amplitude
    :db2 (* 20.0 (math/log10 value))))   ;; amplitude to dB

;; Clamp and warp a value
(defn param->osc [spec value]
  (let [clamped (math/clamp value (:min spec) (:max spec))
        warped (warp-value spec clamped)]
    warped))
```

- [ ] Create typed synth controllers:

```turmeric
(defstruct SynthController
  [node-id :NodeID
   spec :SynthDefSpec])

(defn synth-controller [def-name]
  {:node-id (synth-new def-name)
   :spec (get-synthdef-spec def-name)})

(defn ctrl-set [ctrl param-name value]
  (let [spec (find-param (:spec ctrl) param-name)
        osc-value (param->osc spec value)]
    (synth-set (:node-id ctrl) param-name osc-value)))
```

**Fixtures**:
- `scscm-params-warp.tur` — test warping functions
- `scscm-params-controller.tur` — create controller, set parameters

**Exit criterion**: Parameter setting with automatic warping and clamping works.

---

### 2.5 Phase E — Live-Coding Integration

**Goal**: Enable live-coding workflow with hcsynth.

**Tasks**:

- [ ] Create REPL-friendly evaluation system:

```turmeric
;; Global state for live-coding session
(defstruct ScscmSession
  [server-addr :cstr
   server-port :int
   next-node-id :int32
   synth-defs :(map<cstr SynthDefSpec>)
   active-synths :(map<NodeID SynthController>)])

;; Initialize connection
(defn scscm-connect [addr port]
  {:server-addr addr
   :server-port port
   :next-node-id 1000
   :synth-defs {}
   :active-synths {}})

;; Evaluate code in live-coding context
(defn scscm-eval [session code]
  ;; Parse and execute code with access to session
  ;; Return updated session or error
  )
```

- [ ] Implement synth replacement strategy:

```turmeric
;; Replace a running synth with a new one, preserving parameters
(defn synth-replace [session old-node new-def & args]
  (let [;; Get current parameter values from old synth
        old-params (synth-get old-node)
        ;; Create new synth with same parameters
        new-node (synth-new new-def old-params...)
        ;; Free old synth
        _ (synth-free old-node)]
    (update session :active-synths dissoc old-node assoc new-node new-def)))
```

- [ ] Create live-coding utilities:

```turmeric
;; Play a note with automatic cleanup
(defn play-note [session def-name note vel duration]
  (let [synth (synth-new def-name :freq (midi->hz note) :amp (vel->amp vel))
        _ (defer-after (sec->samples duration) (synth-free synth))]
    synth))

;; Schedule a function to run after delay
(defn defer-after [samples thunk]
  ```c
  // TODO: integrate with hcsynth timing or use a separate timer
  // For now, use a simple approach with a global delay queue
  ```)
```

**Fixtures**:
- `scscm-live-basic.tur` — connect, play note, disconnect
- `scscm-live-replace.tur` — replace running synth
- `scscm-live-pattern.tur` — simple pattern generation

**Exit criterion**: Can live-code synth patches with hot-reloading.

---

### 2.6 Phase F — Pattern Library (Optional)

**Goal**: Mini-Klang / TidalCycles-inspired pattern library.

**Tasks**:

- [ ] Define time representation:

```turmeric
(def-type Beats float)  ;; Time in beats
(def-type Seconds float)
(def-type Samples int64)

(def bpm->beats-per-sec [bpm] (/ bpm 60.0))
```

- [ ] Create pattern types:

```turmeric
(def-type Pattern<T> (fn [Beats] :T))

(defn const [value] (fn [_] value))
(defn cycle [& values]
  (fn [time] (get values (mod (floor (* time (bpm->beats-per-sec 120))) (len values)))))

defn every [n pat]
  (fn [time] (if (== 0 (mod (floor time) n)) (pat time) nil)))
```

- [ ] Create pattern combinators:

```turmeric
(defn stack [& patterns]
  (fn [time] (map #(% time) patterns)))

defn seq [& patterns]
  (fn [time] (let [index (mod (floor (* time (bpm->beats-per-sec 120))) (len patterns))]
             ((get patterns index) time))))

defn slow [factor pat]
  (fn [time] (pat (/ time factor))))

defn fast [factor pat]
  (fn [time] (pat (* time factor))))
```

- [ ] Create pattern players:

```turmeric
(defn play-pattern [session synth-def pattern bpm]
  (let [beats-per-sec (bpm->beats-per-sec bpm)
        start-time (now)]
    (fn []
      (let [elapsed (- (now) start-time)
            beats (* elapsed beats-per-sec)
            params (pattern beats)]
        (apply synth-new synth-def params)))))
```

**Fixtures**:
- `scscm-pattern-basic.tur` — constant and cycle patterns
- `scscm-pattern-combinators.tur` — stack, seq, every
- `scscm-pattern-synth.tur` — play patterns through synths

**Exit criterion**: Can create and play rhythmic patterns.

---

### 2.7 Phase G — Error Handling & Robustness

**Goal**: Production-ready error handling and recovery.

**Tasks**:

- [ ] Define error types:

```turmeric
(def-type ScscmError :enum
  [ :connection-failed
    :send-failed
    :invalid-args
    :synth-not-found
    :out-of-nodes ])

(defstruct ScscmResult<T>
  [ok :bool
   value :T
   error :ScscmError])
```

- [ ] Add error handling to all FFI calls:

```turmeric
(defn safe-osc-send [msg]
  (let [result (hc_osc_send msg)]
    (if (== result 0)
      {:ok true :value result}
      {:ok false :error :send-failed})))
```

- [ ] Create connection monitoring:

```turmeric
(defn check-connection [session]
  ;; Send a ping and check response
  (let [start (now)]
    (osc-send "/ping")
    ;; TODO: wait for /pong response with timeout
    ))

(defn reconnect [session]
  ;; Attempt to reconnect to server
  )
```

- [ ] Add node ID management:

```turmeric
(defn alloc-node-id [session]
  (let [id (:next-node-id session)]
    (update session :next-node-id inc)
    id))

(defn free-node-id [session id]
  ;; Mark ID as available for reuse
  )
```

**Fixtures**:
- `scscm-errors-basic.tur` — error handling for failed sends
- `scscm-errors-reconnect.tur` — automatic reconnection

**Exit criterion**: Library gracefully handles connection failures and errors.

---

### 2.8 Phase H — Performance Optimization

**Goal**: Minimize overhead for high-frequency OSC messages.

**Tasks**:

- [ ] Message pooling to avoid allocations:

```turmeric
;; Pre-allocate messages for common patterns
(defn osc-message-pooled [address]
  ;; Use a pool of pre-allocated messages
  )
```

- [ ] Batch message sending:

```turmeric
(defn osc-send-batch [& messages]
  ;; Collect messages and send as bundle
  )
```

- [ ] Inline critical paths:

```turmeric
;; Use inline-C for hot paths
(defn synth-set-fast [node-id param value]
  ```c
  // Directly call hcsynth functions without going through Turmeric thunks
  osc_message_t* msg = hc_osc_alloc_message("/n_set");
  hc_osc_message_add_int32(msg, node-id);
  hc_osc_message_add_string(msg, param);
  hc_osc_message_add_float(msg, value);
  hc_osc_send(msg);
  hc_osc_free_message(msg);
  ```)
```

- [ ] Benchmark against hand-written C:
  - Measure latency for single message
  - Measure throughput for batch messages

**Fixtures**:
- `scscm-perf-benchmark.tur` — performance comparison

**Exit criterion**: Overhead < 10% compared to hand-written C.

---

## 3. Project Structure

```
fith/
├── stdlib/
│   └── scscm/
│       ├── ffi.tur              # Phase A: FFI bindings
│       ├── types.tur            # Phase B: OSC and SC types
│       ├── msg.tur              # Phase B: Message builders
│       ├── synth.tur            # Phase C: Synth/Node API
│       ├── params.tur           # Phase D: Parameter system
│       ├── live.tur             # Phase E: Live-coding integration
│       ├── pattern.tur          # Phase F: Pattern library
│       ├── errors.tur           # Phase G: Error handling
│       └── perf.tur             # Phase H: Performance
├── examples/
│   └── scscm/
│       ├── basic.tur           # Simple synth example
│       ├── live-coding.tur      # Live-coding demo
│       └── pattern-demo.tur     # Pattern library demo
├── tests/
│   └── scscm/
│       ├── ffi_test.tur        # Phase A tests
│       ├── msg_test.tur        # Phase B tests
│       └── ...
└── docs/
    └── scscm-hcsynth-livecoding-plan.md  # This document
```

---

## 4. Integration with hcsynth

### 4.1 Required hcsynth Features

- OSC message sending API (`hc_osc_*` functions)
- Server connection management
- Optional: OSC message receiving for feedback

### 4.2 Build Integration

```cmake
# In Turmeric build system, link against hcsynth
find_package(hcsynth REQUIRED)

target_link_libraries(turmeric_runtime PRIVATE hcsynth)
```

Or include hcsynth as a submodule:
```cmake
add_subdirectory(../hypercollider/external/hcsynth)
```

### 4.3 Runtime Initialization

```c
// In generated _main.c or runtime initialization
#include <HC_Wasm_OscShim.h>

void tur_scscm_init() {
    hc_osc_init();
    // Optional: set default server
    hc_osc_connect("127.0.0.1", 57110);
}
```

---

## 5. Design Decisions

### 5.1 Memory Management Strategy

| Approach | Pros | Cons | Decision |
|---|---|---|---|
| Manual free | Full control, no overhead | Error-prone, easy to leak | Use `defer` for scoped cleanup |
| GC (rc<T>) | Automatic cleanup | Overhead for reference counting | Use for session state |
| Pooling | High performance for hot paths | Complex to implement | Use for message pooling |
| **Chosen** | **Use `defer` for message cleanup, rc<T> for session state, pooling for performance** | | |

### 5.2 Error Handling Strategy

| Approach | Pros | Cons | Decision |
|---|---|---|---|
| Exceptions (Phase 17) | Clean syntax, familiar | Requires exception support | Use for library errors |
| Result<T,E> | Explicit, no hidden control flow | Verbose | Use for FFI boundaries |
| Panic | Simple | No recovery | Use for unrecoverable errors |
| **Chosen** | **Use Result<T,E> at FFI boundaries, exceptions for library code, panic for unrecoverable** | | |

### 5.3 Threading Model

| Approach | Pros | Cons | Decision |
|---|---|---|---|
| Single-threaded | Simple, no synchronization | Blocks during OSC sends | Default for v1 |
| Async (effects) | Non-blocking, better performance | Complex, requires Phase 19 | Future enhancement |
| Background thread | Dedicated OSC thread | Complex synchronization | Future enhancement |
| **Chosen** | **Single-threaded for v1, async with effects for v2** | | |

---

## 6. Testing Strategy

### 6.1 Unit Tests
- Test each FFI binding individually
- Test message construction and sending
- Test parameter warping and clamping

### 6.2 Integration Tests
- Require running scsynth instance
- Test end-to-end synth creation and control
- Test error conditions (disconnected server, invalid messages)

### 6.3 Performance Tests
- Benchmark message sending throughput
- Measure latency for time-critical operations

### 6.4 Test Fixtures Location
```
tests/scscm/
├── ffi/
├── msg/
├── synth/
├── params/
├── live/
└── integration/
```

---

## 7. Milestones

| Milestone | Phases | Target Date | Deliverables |
|---|---|---|---|
| M1: Basic Connectivity | A, B | TBD | Can send OSC messages to scsynth |
| M2: Synth Control | C | TBD | Can create and control synths |
| M3: Live-Coding | E | TBD | Basic live-coding workflow |
| M4: Production Ready | G, H | TBD | Error handling, performance optimized |
| M5: Pattern Library | F | TBD | Pattern combinators and players |

---

## 8. Open Questions

1. **How to handle OSC message receiving from scsynth?**
   - Options: Polling, callback registration, effect-based async
   - Recommendation: Start without receiving, add later via effects

2. **Should we support SynthDef loading from .scsyndef files?**
   - Options: Pre-load in C, send from Turmeric, external tool
   - Recommendation: Use hcsynth's SynthDef loading, reference by name

3. **How to handle sample-accurate timing?**
   - Options: hcsynth timing, external clock, simple delay
   - Recommendation: Use hcsynth's timing system for accuracy

4. **Should we support multi-server connections?**
   - Options: Single global, per-session, connection pooling
   - Recommendation: Per-session connections for flexibility

---

## 9. Related Work

- [SuperCollider](https://supercollider.github.io/) - Original scsynth server
- [hcsynth](https://github.com/hypercollider/hypercollider/tree/main/engine) - C implementation of scsynth
- [sc3-plugins](https://github.com/supercollider/sc3-plugins) - UGen plugins
- [TidalCycles](https://tidalcycles.org/) - Pattern library inspiration
- [Klang](https://github.com/nonnonstop/klang) - Live-coding environment for SuperCollider

---

## 10. Appendix

### 10.1 OSC Message Examples

| SuperCollider Message | OSC Address | Arguments |
|---|---|---|
| Synth creation | `/s_new` | synth-def-name, node-id, add-action, arg1, arg2, ... |
| Parameter set | `/n_set` | node-id, param-name, value, ... |
| Node free | `/n_free` | node-id |
| SynthDef load | `/d_load` | synth-def-name |
| SynthDef free | `/d_free` | synth-def-name |
| Group creation | `/g_new` | node-id, add-action |
| Status request | `/status` | |
| Ping | `/ping` | |

### 10.2 hcsynth API Reference

See `../hypercollider/engine/HC_Wasm_OscShim.h` for the canonical API.

### 10.3 Turmeric Type Mapping

| Turmeric Type | C Type | OSC Type |
|---|---|---|
| `int32` | `int32_t` | OSC_INT32 |
| `float` | `float` | OSC_FLOAT |
| `cstr` | `const char*` | OSC_STRING |
| `slice<uint8>` | `uint8_t* + len` | OSC_BLOB |
| `NodeID` | `int32_t` | OSC_INT32 |
