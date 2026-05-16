# Session Types — Implementation Plan (SS0–SS4)

> **Status:** Draft — Not Started
>
> **Target:** v3–v4
>
> **Prerequisites:** Linear Types (LT0–LT4) complete; thread primitives (Phase T19) complete; Higher-Ranked Types (HRT1) recommended.
>
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§5 Session Types, §1 Linear Types, §8 Effect Types)
> [linear-types-plan.md](linear-types-plan.md),
> [effect-rows-plan.md](effect-rows-plan.md)
>
> **Last updated:** 2026-05-15

---

## Motivation

Session types describe **communication protocols** between concurrent processes as types. A session type specifies the exact sequence and types of messages that may be exchanged on a channel. Session types enable:

- **Compile-time protocol verification** -- violations are type errors, not runtime crashes
- **Deadlock prevention** -- duality checking ensures both endpoints agree on message order
- **Type-safe concurrency** -- message passing is statically verified end-to-end
- **Structured concurrency** -- protocols must terminate or recurse explicitly

Turmeric already has algebraic effects (Phase 19) and thread primitives (Phase T19). Session types layer on top of these to give channels a type-safe interface that is checked at compile time.

Session channels are **linear** by definition: a channel must be used to send or receive exactly according to its protocol, then closed. Attempting to send on a closed channel, receive out of order, or discard a channel mid-protocol are all type errors.

| Property | Today | Goal |
|---|---|---|
| Untyped message channels | **Available** (Phase T19) | Replace with typed session channels |
| Protocol duality checking | **Not supported** | SS1 |
| Linear channel discipline | **Not enforced** | SS0 + linear-types-plan.md |
| Send / receive type checking | **Not supported** | SS1 |
| Choice and branching | **Not supported** | SS3 |
| Recursive protocols | **Not supported** | SS3 |
| Session delegation | **Not supported** | SS3 |

---

## Proposed Syntax

```clojure
;; Send type: channel will send a value of type T, then continue as Q
(deftype Send [T Q] ...)

;; Receive type: channel will receive a value of type T, then continue as Q
(deftype Recv [T Q] ...)

;; Close: protocol is finished
(deftype Close [])

;; A session channel carrying protocol P
(deftype Session [P] ...)

;; Choice (internal): sender chooses which branch to take
(deftype Choose [P Q] ...)    ; &{P, Q}

;; Branch (external): receiver selects the offered branch
(deftype Branch [P Q] ...)    ; |{P, Q}

;; Recursive protocol (mu-type)
(deftype Rec [F] ...)         ; mu X. F X
```

---

## Motivating Examples

### Example 1: Simple request-response protocol

```clojure
;; Server side: receive a request, send a response, then close
(deftype ServerProtocol []
  (Recv Request (Send Response Close)))

;; Client side: dual -- send a request, receive a response, then close
(deftype ClientProtocol []
  (Send Request (Recv Response Close)))

;; Server implementation
(defn server [^linear chan : (Session ServerProtocol)] : unit
  (let [[req chan] (recv chan)]
    (let [resp      (handle-request req)]
      (let [chan    (send chan resp)]
        (close chan)))))

;; Client implementation
(defn client [^linear chan : (Session ClientProtocol)] : unit
  (let [chan         (send chan (make-request))]
    (let [[resp chan] (recv chan)]
      (close chan)
      (process-response resp))))
```

### Example 2: Repeating protocol with recursion

```clojure
;; Echo server: receive ints, send them back, repeat until quit
(deftype EchoServer []
  (Branch
    (Recv int (Send int EchoServer))   ; loop arm
    Close))                             ; quit arm

(defn echo-server [^linear chan : (Session EchoServer)] : unit
  (match (offer chan)
    (Left chan)
      (let [[n chan] (recv chan)]
        (let [chan (send chan n)]
          (echo-server chan)))
    (Right chan)
      (close chan)))
```

### Example 3: Type-safe RPC calculator

```clojure
(deftype Calculator []
  (Branch
    (Recv [Add int int] (Send int Calculator))
    (Branch
      (Recv [Sub int int] (Send int Calculator))
      Close)))

(defn calculator-server [^linear chan : (Session Calculator)] : unit
  (match (offer chan)
    (Left chan)
      (let [[[Add x y] chan] (recv chan)]
        (let [chan (send chan (+ x y))]
          (calculator-server chan)))
    (Right chan)
      (match (offer chan)
        (Left chan)
          (let [[[Sub x y] chan] (recv chan)]
            (let [chan (send chan (- x y))]
              (calculator-server chan)))
        (Right chan)
          (close chan))))
```

---

## Interaction with Existing Features

| Feature | Interaction | Notes |
|---|---|---|
| Linear types | Session channels are `^linear` | Channel must be fully consumed according to protocol |
| Algebraic effects | Handlers can use session operations | `recv`/`send` can be effects |
| STM | Atomic sessions within transactions | Careful design needed; avoid deadlock in STM |
| Threads | Sessions between threads are the primary use case | Channels are thread-safe by construction |
| `rc<T>` | Session channels may not be `rc`-shared | Linearity prevents this |
| Borrow checker | Sessions are owned, not borrowed | `recv`/`send` consume and return the channel |
| HRT | Recursive protocols require recursive types | HRT1 (Rank-2) recommended |

---

## Architecture

```
src/types.h         -- TY_SESSION, TY_SEND, TY_RECV, TY_CLOSE, TY_CHOOSE, TY_BRANCH
src/reader.c        -- Parse session type constructors
src/elab.c          -- Session type checking; duality checking; linearity enforcement
src/typecheck.c     -- Subtype relation for session types; protocol progress
src/codegen.c       -- Message passing primitives; channel struct
src/runtime/        -- Channel runtime (typed wrappers over Phase T19 primitives)
src/error.h/.c      -- Error codes TUR_E0200-TUR_E0249 (shared with uniqueness? or own range)
```

> **Note:** Error code range `TUR_E0200`–`TUR_E0249` is proposed here; reconcile with uniqueness-types-plan.md which also proposes `TUR_E0200`. Final allocation should be coordinated.

---

## Phase SS0 — Session Type Foundations

**Goal:** Add session type constructors to the type system and parser.

- [ ] Add to `TypeKind` in `src/types.h`:

  ```c
  TY_SESSION,   /* Session[P] -- a channel carrying protocol P */
  TY_SEND,      /* Send[T, Q] -- send T then continue as Q */
  TY_RECV,      /* Recv[T, Q] -- receive T then continue as Q */
  TY_CLOSE,     /* Close -- protocol complete */
  TY_CHOOSE,    /* Choose[P, Q] -- internal choice (sender picks) */
  TY_BRANCH,    /* Branch[P, Q] -- external choice (receiver picks) */
  TY_REC,       /* Rec[F] -- recursive protocol (mu-type) */
  ```

- [ ] Parse session type syntax in `src/reader.c`
- [ ] Session channels are `CK_LINEAR` by construction (from linear-types-plan.md)
- [ ] Add session channel operations to the elaborator:
  - `(send chan val)` -- consumes `chan : Session[Send[T, Q]]`; returns `chan' : Session[Q]`
  - `(recv chan)` -- consumes `chan : Session[Recv[T, Q]]`; returns `[val chan'] : [T, Session[Q]]`
  - `(close chan)` -- consumes `chan : Session[Close]`; returns `unit`
  - `(offer chan)` -- consumes `chan : Session[Branch[P, Q]]`; returns `(either (Session P) (Session Q))`
  - `(choose-left chan)` -- consumes `chan : Session[Choose[P, Q]]`; returns `Session[P]`
  - `(choose-right chan)` -- consumes `chan : Session[Choose[P, Q]]`; returns `Session[Q]`

---

## Phase SS1 — Session Type Checking

**Goal:** Implement duality checking and protocol progress checking.

### Duality

Every session channel has two endpoints with **dual** protocols. If one endpoint is `Send[T, Q]`, the other must be `Recv[T, Dual[Q]]`.

The duality relation:

| Protocol | Dual |
|---|---|
| `Send[T, Q]` | `Recv[T, Dual[Q]]` |
| `Recv[T, Q]` | `Send[T, Dual[Q]]` |
| `Close` | `Close` |
| `Choose[P, Q]` | `Branch[Dual[P], Dual[Q]]` |
| `Branch[P, Q]` | `Choose[Dual[P], Dual[Q]]` |
| `Rec[F]` | `Rec[x => Dual[F x]]` |

- [ ] Implement `dual(P)` function in `src/typecheck.c`
- [ ] When a session channel is created (`make-session`), check that the two endpoints have dual types
- [ ] Emit error `TUR_E0210` when duality fails

### Protocol Progress

- [ ] A session channel must be fully consumed (all `send`/`recv` steps completed, ending at `Close`) before the channel binding goes out of scope
- [ ] Re-use linear type machinery: `chan : Session[Close]` must be passed to `close` before scope exit
- [ ] Emit `TUR_E0211` when a non-`Close` session is dropped

### Error codes

| Code | Message |
|---|---|
| `TUR_E0210` | Session endpoint types are not dual: `{P}` vs. `{Q}` |
| `TUR_E0211` | Session channel `{name}` dropped before protocol completion (expected `Close`, got `{P}`) |
| `TUR_E0212` | Operation `{op}` not valid on session with protocol `{P}` |

---

## Phase SS2 — Session Codegen

**Goal:** Emit message-passing primitives and channel structs to C.

- [ ] Define a typed channel struct in generated C:

  ```c
  typedef struct TurChannel {
      pthread_mutex_t lock;
      pthread_cond_t  ready;
      void*           data;
      int             closed;
  } TurChannel;
  ```

- [ ] Emit `send`, `recv`, `close` as thin wrappers over the Phase T19 channel primitives
- [ ] The session type is erased at runtime: no tag or protocol descriptor needed
- [ ] Optional: in debug builds, emit a runtime protocol tag and check it on each operation (useful for FFI boundaries)

---

## Phase SS3 — Session Combinators

**Goal:** Add recursive protocols, delegation, and session subtyping.

- [ ] **Recursive protocols (`Rec`):** support `mu X. F X` for protocols that repeat
  - `EchoServer = Branch (Recv int (Send int EchoServer)) Close`
  - Implement as isorecursive types (explicit fold/unfold) or equirecursive (transparent)
- [ ] **Delegation:** a session channel can be passed to another thread/function, transferring protocol ownership
  - The receiving function must fully complete the delegated protocol
- [ ] **Session subtyping:** a server offering `Branch[P, Q, R]` can be used where `Branch[P, Q]` is expected (external choice subtyping is covariant in offered options)
- [ ] **Timeout channels:** `(recv-timeout chan duration)` returns `(option T)` and advances or resets the protocol

---

## Phase SS4 — Integration

**Goal:** Stdlib protocols, effect integration, and STM.

- [ ] Stdlib session patterns in `stdlib/session.tur`:
  - `echo` -- trivial echo protocol
  - `rpc` -- generic request-response
  - `pubsub` -- publish-subscribe protocol template
- [ ] Integration with effects: `recv` and `send` may be declared as effects so that handlers can intercept message passing (useful for testing, logging, mock protocols)
- [ ] Integration with STM: session operations within `atomic` blocks are serialised; deadlock detection is the programmer's responsibility (document limitations)
- [ ] `tur explain TUR_E0210`, `TUR_E0211`, `TUR_E0212` entries
- [ ] Integration tests: echo server/client, calculator RPC, multi-party sessions

---

## Complexity Assessment

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | High | Six new `TypeKind` variants; duality relation |
| Elaborator changes | High | Protocol progress checking; duality checking |
| Codegen changes | Medium | Channel struct emission; typed send/recv wrappers |
| C emission | Medium | Thin wrappers over Phase T19 primitives |
| Error messages | High | Protocol mismatch errors require showing expected vs. actual protocol steps |

---

## Feature Flag

```sh
turc -Xsessions myfile.tur
```

Enabling `-Xsessions` implicitly enables `-Xlinear` (channels are linear).

---

## Implementation Priority

**Medium-High** — v3–v4, after Linear Types (LT0–LT4) and thread primitives (Phase T19).

Session types are the highest-value concurrency safety feature available. They build directly on linear types and are well-understood theoretically. The primary complexity is in protocol progress and duality checking; the codegen is relatively straightforward.

Start with simple send/receive protocols (SS0–SS2), ship them, then add choice, recursion, and delegation (SS3–SS4).

---

## Open Questions

1. **Isorecursive vs. equirecursive protocols:** ~~Isorecursive types require explicit `fold`/`unfold`; equirecursive types are transparent but harder to type-check. Equirecursive is more ergonomic for end users.~~
   **Decision:** Equirecursive. Protocol types are transparently equal to their unrolled forms; no `fold`/`unfold` at recursive call sites. Type equality is checked co-inductively (with a "seen" set to prevent looping). Builds on the existing `Fix`/`Free` recursive type infrastructure.
2. **Multi-party sessions:** ~~Binary sessions (two endpoints) are covered here. Multi-party session types (N participants) are significantly more complex and should be deferred.~~
   **Decision:** Reserve the design space. Binary sessions ship in v3--v4. Multi-party support is not scoped for any current release. The `Session[P]` type is designed to be extensible. See [multiparty-sessions-tradeoffs.md](multiparty-sessions-tradeoffs.md) for the full tradeoff analysis between a native implementation and a minimal binary-composition subset.
3. **Timeout support:** ~~`recv-timeout` is useful for production systems but complicates the protocol type (the protocol may or may not advance). Should timeouts be typed or untyped?~~
   **Decision:** Typed (protocol-aware). A new `Timeout` protocol constructor encodes both branches in the type:
   `(Recv T (Timeout Q P))` -- on message receipt the protocol continues as `Q`; on timeout it continues as `P`. The channel is returned in both branches so the session can continue. `Timeout` is self-dual (both endpoints see the same branching outcome). A new `TY_TIMEOUT` variant is added to `TypeKind` alongside `TY_SEND`/`TY_RECV`. Added in SS3.
4. **STM interaction:** ~~Can a session `send` be rolled back if the enclosing STM transaction retries? This requires careful design to preserve the linear discipline.~~
   **Decision:** Allow `choose`/`offer` inside `atomic` blocks; forbid `send`/`recv`. The atomically-committed decision is which protocol branch to take; actual message passing happens outside the transaction. This preserves linearity (no partially-advanced channel is left stranded on retry) and avoids a per-transaction message buffer. The elaborator emits a type error if `send` or `recv` appears directly inside an `atomic` expression.

---

## Prior Art

- **Haskell (`session-types` library):** Type-safe binary session types
- **Rust (`sesh` crate):** Research session types for Rust
- **Scribble:** Protocol specification language (multi-party sessions)
- **Covington et al.:** Session Types for C (direct prior art for C-targeting implementation)
- **Linear Logic (Girard):** Theoretical foundation for session types

---

## References

- [Honda -- Types for Dyadic Interaction](https://dl.acm.org/doi/10.1145/248206.248214)
- [Covington et al. -- Session Types for C](https://dl.acm.org/doi/10.1145/1596553.1596586)
- [advanced-type-system-feasibility-plan.md §5](advanced-type-system-feasibility-plan.md)
