# Session Types -- Implementation Plan (SS0-SS8)

> **Status:** Draft -- Not Started
>
> **Target:** v3-v4 (binary sessions, SS0-SS4); v4-v5 (multi-party sessions, SS5-SS8)
>
> **Prerequisites:** Linear Types (LT0-LT4) complete; thread primitives (Phase T19) complete; Higher-Ranked Types (HRT1) recommended.
>
> **Related:** [advanced-type-system-feasibility-plan.md](advanced-type-system-feasibility-plan.md)
> (§5 Session Types, §1 Linear Types, §8 Effect Types)
> [linear-types-plan.md](linear-types-plan.md),
> [effect-rows-plan.md](effect-rows-plan.md)
>
> **Last updated:** 2026-05-18

---

## Overview

This plan covers session types in two stages:

- **Binary session types (SS0-SS4)** -- two-endpoint protocols with dual-type
  checking. This is the foundation and ships first, targeting v3-v4.
- **Multi-party session types (SS5-SS8)** -- N-participant protocols built from
  a single global protocol declaration that the compiler projects onto each
  participant role. Targets v4-v5, after binary sessions are stable.

The multi-party design adopts **Option A -- native multi-party session types**.
The full tradeoff analysis behind that choice -- including the rejected Option B
(binary composition) -- is in the [Multi-Party Session Types](#multi-party-session-types)
section below.

---

## Motivation

Session types describe **communication protocols** between concurrent processes as types. A session type specifies the exact sequence and types of messages that may be exchanged on a channel. Session types enable:

- **Compile-time protocol verification** -- violations are type errors, not runtime crashes
- **Deadlock prevention** -- duality checking ensures both endpoints agree on message order
- **Type-safe concurrency** -- message passing is statically verified end-to-end
- **Structured concurrency** -- protocols must terminate or recurse explicitly

Turmeric already has algebraic effects (Phase 19) and thread primitives (Phase T19). Session types layer on top of these to give channels a type-safe interface that is checked at compile time.

Session channels are **linear** by definition: a channel must be used to send or receive exactly according to its protocol, then closed. Attempting to send on a closed channel, receive out of order, or discard a channel mid-protocol are all type errors.

Many practical concurrent systems involve more than two participants -- handshakes, coordinator/worker pools, pipelines, consensus rings. Binary session types check each two-endpoint channel in isolation but cannot see the *global* protocol that ties N participants together. Multi-party session types (SS5-SS8) close that gap: a single global protocol declaration is projected by the compiler onto each role, so coherence and deadlock freedom hold for the whole system, not just per channel.

| Property | Today | Goal |
|---|---|---|
| Untyped message channels | **Available** (Phase T19) | Replace with typed session channels |
| Protocol duality checking | **Not supported** | SS1 |
| Linear channel discipline | **Not enforced** | SS0 + linear-types-plan.md |
| Send / receive type checking | **Not supported** | SS1 |
| Choice and branching | **Not supported** | SS3 |
| Recursive protocols | **Not supported** | SS3 |
| Session delegation | **Not supported** | SS3 |
| Multi-party global protocols | **Not supported** | SS5 |
| Global-to-local projection | **Not supported** | SS6 |
| N-party deadlock freedom | **Not supported** | SS5-SS6 |

---

## Proposed Syntax

### Binary session types

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

### Multi-party global protocols

```clojure
;; Global protocol declaration: names the roles and the message flow
(defprotocol Name [Role ...] interaction ...)

;; Interaction forms inside a global protocol:
(-> From To MsgType)                 ; From sends a MsgType to To
(choice From [label branch ...] ...) ; From selects a labelled branch
(loop label body ...)                ; recursive global protocol
(continue label)                     ; recur to an enclosing loop

;; A multi-party endpoint: the channel a single role holds
(deftype Role [G R] ...)             ; endpoint of global protocol G playing role R

;; Project a global protocol onto a role's local session type
(project G R)                        ; => a binary-style Session protocol
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

### Example 4: Multi-party three-way handshake

```clojure
;; Global protocol: a three-way handshake among roles A, B, C
(defprotocol Handshake [A B C]
  (-> A B Syn)
  (-> B A SynAck)
  (-> A C Ack)
  (-> C A AckAck))

;; Each role's local type is projected automatically by the compiler:
;;   (project Handshake A) = Send Syn (Recv SynAck (Send Ack (Recv AckAck Close)))
;;   (project Handshake B) = Recv Syn (Send SynAck Close)
;;   (project Handshake C) = Recv Ack (Send AckAck Close)

(defn role-a [^linear chan : (Role Handshake A)] : unit
  (let [chan        (send-to chan B (make-syn))]
    (let [[sa chan] (recv-from chan B)]
      (let [chan      (send-to chan C (make-ack))]
        (let [[aa chan] (recv-from chan C)]
          (close chan))))))

(defn role-b [^linear chan : (Role Handshake B)] : unit
  (let [[syn chan] (recv-from chan A)]
    (let [chan     (send-to chan A (make-syn-ack))]
      (close chan))))

(defn role-c [^linear chan : (Role Handshake C)] : unit
  (let [[ack chan] (recv-from chan A)]
    (let [chan     (send-to chan A (make-ack-ack))]
      (close chan))))

;; Spawning all three roles from a single scoped protocol
(defn run-handshake [] : unit
  (let [[a b c] (make-protocol Handshake)]
    (spawn (fn [] (role-a a)))
    (spawn (fn [] (role-b b)))
    (role-c c)))
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
| Multi-party roles | Each `Role` endpoint is `^linear`; roles run concurrently | `make-protocol` distributes one endpoint per role (SS7) |

---

## Architecture

```
src/types.h         -- TY_SESSION, TY_SEND, TY_RECV, TY_CLOSE, TY_CHOOSE,
                       TY_BRANCH, TY_REC (binary); TY_GLOBAL, TY_ROLE (multi-party)
src/reader.c        -- Parse session type constructors; parse (defprotocol ...)
src/elab.c          -- Session type checking; duality checking; linearity
                       enforcement; global-protocol well-formedness;
                       role-vs-projection checking
src/typecheck.c     -- Subtype relation for session types; protocol progress
src/project.c       -- Global-type projection algorithm (new file, SS6)
src/codegen.c       -- Message passing primitives; channel struct
src/runtime/        -- Channel runtime (typed wrappers over Phase T19
                       primitives); N-endpoint allocation and routed queues
src/error.h/.c      -- Error codes TUR_E0200-TUR_E0249 (shared with uniqueness? or own range)
```

> **Note:** Error code range `TUR_E0200`-`TUR_E0249` is proposed here; reconcile with uniqueness-types-plan.md which also proposes `TUR_E0200`. Final allocation should be coordinated. Within this range, binary sessions use `TUR_E0210`-`TUR_E0212` and multi-party sessions use `TUR_E0220`-`TUR_E0223`.

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
- [ ] Integration tests: echo server/client, calculator RPC, delegated sessions

---

## Multi-Party Session Types

### Background

The binary session types phases (SS0-SS4) cover two-endpoint protocols: one
sender, one receiver, with a dual-type check ensuring they agree. Many practical
concurrent systems require more than two participants. This section analyses
what it takes to support N-party protocols natively, what can instead be
achieved by composing binary sessions, and which approach Turmeric adopts.

### Option A -- Native Multi-Party Session Types (adopted)

#### What it provides

Native multi-party session types are based on **global types** (from Scribble /
Honda et al.). A single global protocol declaration specifies what every
participant sends and receives. Each participant's **local type** is
mechanically projected from the global type by the compiler.

Example global protocol:

```
protocol ThreeWayHandshake (A, B, C):
  A -> B : SYN
  B -> A : SYN-ACK
  A -> C : ACK
  C -> A : ACK-ACK
```

In Turmeric this is written with the `defprotocol` form (see
[Proposed Syntax](#multi-party-global-protocols) and Example 4). The compiler
projects the global protocol into local types for A, B, and C separately, then
checks each participant's implementation against its local type.

#### Guarantees

| Guarantee | Binary sessions | Native multi-party |
|---|---|---|
| Per-channel duality | Yes | Yes (derived from global type) |
| Deadlock freedom (2-party) | Yes | Yes |
| Deadlock freedom (N-party) | **No** | Yes |
| Global protocol coherence | **No** | Yes |
| Role-based type per participant | **No** | Yes |

#### Implementation complexity

- **Global type language:** A new syntactic form for global protocol
  declarations (distinct from local session types).
- **Projection algorithm:** A non-trivial algorithm that projects a global type
  onto a local type for each participant. Projection can fail (not every global
  type has a well-defined projection for every role).
- **N-way coherence checking:** Instead of checking two endpoints are dual,
  check that all participants' local types are consistent projections of the
  same global type.
- **Scoped channel creation:** Creating a multi-party session requires
  allocating N endpoints simultaneously and distributing them to participants.
- **Prior art for C-targeting:** Limited. Scribble targets JVM/Go. Covington
  et al. (Session Types for C) covers binary only.

Estimated additional complexity over binary sessions: **High** (comparable
in scope to implementing binary sessions from scratch again).

### Option B -- Minimal Subset via Binary Composition (rejected)

#### What it provides

Multi-party patterns are decomposed into binary channels by the programmer.
The compiler verifies each binary channel pair is dual; it does not verify
global protocol coherence.

Example coordinator/worker pattern:

```clojure
;; Coordinator holds two binary channels -- one to each worker
(defn coordinator
  [^linear ch-a : (Session (Send Work (Recv Result Close)))
   ^linear ch-b : (Session (Send Work (Recv Result Close)))]
  : unit
  (let [ch-a (send ch-a work-a)]
    (let [ch-b (send ch-b work-b)]
      (let [[res-a ch-a] (recv ch-a)]
        (let [[res-b ch-b] (recv ch-b)]
          (close ch-a)
          (close ch-b)
          (combine res-a res-b))))))
```

#### What it loses vs. Option A

1. **Static deadlock freedom for N-party cycles.** A circular wait involving
   three or more participants (A waits on B, B waits on C, C waits on A) is
   not caught by the type checker. The programmer must reason about receive
   ordering manually.

2. **Global protocol coherence.** If participant B's channel with A disagrees
   with B's channel with C in a way that creates an inconsistency in the
   overall protocol, the type checker will not detect it. Each binary channel
   pair is checked in isolation.

3. **Role-based reasoning.** There is no single type that describes "the role
   of participant A in this protocol." A participant with N binary channels
   holds N separate session types, which can become unwieldy past 3 parties.

4. **Mechanical local-type derivation.** With native multi-party, the
   compiler derives each participant's type from the global specification
   automatically. With binary composition, the programmer writes all N*(N-1)/2
   channel types by hand and is responsible for their consistency.

#### What it keeps

- Coordinator/worker (one coordinator, N workers via N binary channels)
- Fan-out/fan-in (distribute then collect)
- Pipelines (A -> B -> C as two binary channels)
- Any protocol that decomposes naturally into pairwise interactions

These patterns cover many systems-programming concurrency use cases, but the
type checker provides no global guarantee about how the pieces fit together.

### Decision

**Turmeric adopts Option A -- native multi-party session types.** Multi-party
support is scoped as Phases SS5-SS8.

Rationale: the point of session types is to move protocol errors from runtime
to compile time. Option B delivers that guarantee for two-party protocols but
silently drops it the moment a third participant is involved -- precisely the
case where protocol bugs are hardest to find by hand (circular waits,
projection inconsistencies). Option A keeps the guarantee uniform: every
protocol Turmeric admits, of any arity, is checked for coherence and deadlock
freedom. The cost is a high-complexity projection algorithm and a global-type
front end, but that work is bounded and well-understood (Honda/Yoshida/Carbone;
Scribble), and the multi-party runtime reuses the binary channel primitives.

Binary composition is not removed -- raw binary channels remain available and
the coordinator/worker, fan-out/fan-in, and pipeline patterns above are still
expressible. They are simply no longer the *recommended* way to express
N-party protocols; a `defprotocol` declaration is.

#### Preconditions for starting SS5

- Binary sessions (SS0-SS4) are stable and in real use. The projection
  algorithm is validated by checking that projected local types match the
  hand-written binary types programmers already trust, so a solid binary
  foundation must come first.
- The projection algorithm (SS6) is the highest-risk component and has no
  C-targeting reference implementation to draw from; budget for that risk
  when scheduling the multi-party track.

---

## Phase SS5 — Global Protocol Types

**Goal:** Add the global protocol declaration form and the multi-party type kinds.

- [ ] Add to `TypeKind` in `src/types.h`:

  ```c
  TY_GLOBAL,    /* Global[...] -- a multi-party global protocol */
  TY_ROLE,      /* Role[G, R]  -- an endpoint of protocol G playing role R */
  ```

- [ ] Parse `(defprotocol Name [Role ...] interaction ...)` in `src/reader.c`
- [ ] Global-protocol interaction forms:
  - `(-> From To MsgType)` -- `From` sends a `MsgType` to `To`
  - `(choice From [label branch ...] ...)` -- `From` selects a labelled branch; the selection is communicated to every other involved role
  - `(loop label body ...)` / `(continue label)` -- recursive global protocols
- [ ] Global protocols are compile-time entities only -- no runtime representation
- [ ] Add multi-party channel operations to the elaborator:
  - `(make-protocol G)` -- returns one `Role[G, R]` endpoint per declared role, as a tuple in role-declaration order
  - `(send-to chan R val)` -- consumes a `Role` endpoint whose next step sends to `R`; returns the advanced endpoint
  - `(recv-from chan R)` -- consumes a `Role` endpoint whose next step receives from `R`; returns `[val chan']`
  - `(close chan)` -- consumes a `Role` endpoint at `Close`; returns `unit`
- [ ] Well-formedness checks (emit `TUR_E0223`):
  - every role used in an interaction is declared in the role list
  - no interaction sends from a role to itself
  - recursion is guarded (every `loop` makes progress before `continue`)
  - every branch of a `choice` is consistently structured

---

## Phase SS6 — Projection and Coherence

**Goal:** Implement the projection algorithm and check each role implementation against its projected local type.

### Projection

- [ ] New file `src/project.c`: implement `project(G, R)` -- compute the local (binary-style) `Session` protocol for role `R` from global protocol `G`
- [ ] Projection rules (per Honda/Yoshida/Carbone):
  - `(-> R X T)` then `rest` projects for `R` to `Send[T, project(rest, R)]`
  - `(-> X R T)` then `rest` projects for `R` to `Recv[T, project(rest, R)]`
  - `(-> X Y T)` then `rest` with `R` uninvolved projects to `project(rest, R)`
  - `choice` projects to `Choose` for the deciding role, `Branch` for the
    directly notified roles, and a *merge* of branch projections for roles
    uninvolved in the choice (merge fails if the branches are not mergeable)
- [ ] Projection is **partial**: a global type may have no well-defined
  projection for a role (e.g. a role must behave differently per branch but is
  never told which branch was chosen). Emit `TUR_E0220` naming the offending
  role and interaction.

### Coherence

- [ ] `(project G R)` is usable as a type annotation; the elaborator resolves it to a concrete `Session` protocol
- [ ] Each role implementation is checked against its projected local type using the existing binary local-type machinery (SS1). No separate N-way duality check is needed -- coherence follows from every role being a projection of the same `G`.
- [ ] Emit `TUR_E0221` when `(project G R)` names a role not declared in `G`
- [ ] Emit `TUR_E0222` when a role implementation does not match its projected local type

### Error codes

| Code | Message |
|---|---|
| `TUR_E0220` | Global protocol `{G}` is not projectable onto role `{R}` at interaction `{step}` |
| `TUR_E0221` | Role `{R}` is not declared in global protocol `{G}` |
| `TUR_E0222` | Role `{R}` implementation does not match its projected local type: expected `{P}`, got `{Q}` |
| `TUR_E0223` | Global protocol `{G}` is not well-formed: `{reason}` |

---

## Phase SS7 — Multi-Party Codegen and Runtime

**Goal:** Scoped multi-party channel creation and routed message passing.

- [ ] `(make-protocol G)` allocates one `Role[G, R]` endpoint per declared role and returns them as a tuple `[r1 r2 ... rN]`, in role-declaration order
- [ ] Each `Role` endpoint carries a runtime role tag so `send-to`/`recv-from` can route messages
- [ ] Runtime backing: an N-party session is a small router holding one directed message queue per ordered role pair (`N*(N-1)` queues), each queue a typed wrapper over the Phase T19 channel primitives
- [ ] `(send-to chan R val)` and `(recv-from chan R)` lower to enqueue/dequeue on the directed queue for the relevant role pair
- [ ] The global-protocol and `Role` types are erased at runtime, exactly as binary session types are (SS2); only the role tag survives, to index the router
- [ ] Optional debug builds: tag each message with its global-protocol step index and assert on dequeue

---

## Phase SS8 — Multi-Party Integration

**Goal:** Stdlib protocols, effect integration, and diagnostics.

- [ ] Stdlib multi-party templates in `stdlib/session.tur`:
  - `three-way-handshake` -- the SYN/ACK example
  - `coordinator` -- one coordinator, N workers, fan-out then fan-in
  - `ring` -- a token passed around a ring of roles
- [ ] Effect integration: `send-to`/`recv-from` may be declared as effects (carried over from SS4) so handlers can mock or log multi-party message passing
- [ ] `tur explain TUR_E0220`, `TUR_E0221`, `TUR_E0222`, `TUR_E0223` entries
- [ ] Integration tests: three-way handshake, coordinator/worker, ring; plus negative tests for non-projectable protocols and role/projection mismatches
- [ ] Documentation: a multi-party section in the session-types guide covering the `defprotocol` syntax and the projection model

---

## Complexity Assessment

### Binary sessions (SS0-SS4)

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes | High | Seven new `TypeKind` variants; duality relation |
| Elaborator changes | High | Protocol progress checking; duality checking |
| Codegen changes | Medium | Channel struct emission; typed send/recv wrappers |
| C emission | Medium | Thin wrappers over Phase T19 primitives |
| Error messages | High | Protocol mismatch errors require showing expected vs. actual protocol steps |

### Multi-party additions (SS5-SS8)

| Aspect | Complexity | Notes |
|---|---|---|
| Global type front end (SS5) | Medium | New `defprotocol` form; well-formedness checks |
| Projection algorithm (SS6) | High | Non-trivial; projection is partial (can fail); highest-risk component |
| Role-vs-projection checking (SS6) | Medium | Reuses binary local-type checking from SS1 |
| Multi-party runtime (SS7) | Medium | N-endpoint allocation; routed directed queues |
| Error messages (SS5-SS6) | High | Projection-failure diagnostics must explain which role and interaction is at fault |

---

## Feature Flag

```sh
turc -Xsessions myfile.tur
```

Enabling `-Xsessions` implicitly enables `-Xlinear` (channels are linear).

The `-Xsessions` flag gates both binary and multi-party forms. The
`(defprotocol ...)` global-protocol form and the `Role` type are available only
under `-Xsessions`; until SS5 lands they are rejected with a "not yet
implemented" diagnostic.

---

## Implementation Priority

**Binary sessions (SS0-SS4): Medium-High** — v3-v4, after Linear Types (LT0-LT4) and thread primitives (Phase T19).

Session types are the highest-value concurrency safety feature available. They build directly on linear types and are well-understood theoretically. The primary complexity is in protocol progress and duality checking; the codegen is relatively straightforward.

Start with simple send/receive protocols (SS0-SS2), ship them, then add choice, recursion, and delegation (SS3-SS4).

**Multi-party sessions (SS5-SS8): Medium** — v4-v5, a separate track that must
not begin until binary sessions are stable and in real use. The projection
algorithm (SS6) is validated against the hand-written binary types programmers
already trust, so a solid binary foundation is a hard precondition. Within the
multi-party track, ship the global-type front end and projection (SS5-SS6)
before the runtime (SS7-SS8).

---

## Open Questions

1. **Isorecursive vs. equirecursive protocols:** ~~Isorecursive types require explicit `fold`/`unfold`; equirecursive types are transparent but harder to type-check. Equirecursive is more ergonomic for end users.~~
   **Decision:** Equirecursive. Protocol types are transparently equal to their unrolled forms; no `fold`/`unfold` at recursive call sites. Type equality is checked co-inductively (with a "seen" set to prevent looping). Builds on the existing `Fix`/`Free` recursive type infrastructure.
2. **Multi-party sessions:** ~~Binary sessions (two endpoints) are covered here. Multi-party session types (N participants) are significantly more complex and should be deferred.~~
   **Decision:** Adopt **Option A -- native multi-party session types**, based on global protocol declarations and compiler projection. Scoped as Phases SS5-SS8, targeting v4-v5, after binary sessions (SS0-SS4) are stable. The full tradeoff analysis -- including the rejected Option B (binary composition) -- is in the [Multi-Party Session Types](#multi-party-session-types) section above.
3. **Timeout support:** ~~`recv-timeout` is useful for production systems but complicates the protocol type (the protocol may or may not advance). Should timeouts be typed or untyped?~~
   **Decision:** Typed (protocol-aware). A new `Timeout` protocol constructor encodes both branches in the type:
   `(Recv T (Timeout Q P))` -- on message receipt the protocol continues as `Q`; on timeout it continues as `P`. The channel is returned in both branches so the session can continue. `Timeout` is self-dual (both endpoints see the same branching outcome). A new `TY_TIMEOUT` variant is added to `TypeKind` alongside `TY_SEND`/`TY_RECV`. Added in SS3.
4. **STM interaction:** ~~Can a session `send` be rolled back if the enclosing STM transaction retries? This requires careful design to preserve the linear discipline.~~
   **Decision:** Allow `choose`/`offer` inside `atomic` blocks; forbid `send`/`recv`. The atomically-committed decision is which protocol branch to take; actual message passing happens outside the transaction. This preserves linearity (no partially-advanced channel is left stranded on retry) and avoids a per-transaction message buffer. The elaborator emits a type error if `send` or `recv` appears directly inside an `atomic` expression.
5. **Asynchronous subtyping for multi-party:** Should projected local types permit message reordering (issuing a send ahead of a receive it does not depend on) for performance? Asynchronous subtyping is sound but its decidability is delicate.
   **Decision:** Deferred. SS5-SS8 use synchronous projection -- each interaction is a rendezvous. Asynchronous optimisation is a possible post-SS8 extension and is explicitly out of scope for the multi-party track.

---

## Prior Art

- **Haskell (`session-types` library):** Type-safe binary session types
- **Rust (`sesh` crate):** Research session types for Rust
- **Scribble:** Protocol specification language with global types and projection -- direct prior art for the global-type / projection model adopted in SS5-SS6
- **Covington et al.:** Session Types for C (direct prior art for the C-targeting binary implementation; binary only)
- **Linear Logic (Girard):** Theoretical foundation for session types

---

## References

- [Honda -- Types for Dyadic Interaction](https://dl.acm.org/doi/10.1145/248206.248214)
- [Honda, Yoshida & Carbone -- Multiparty Asynchronous Session Types](https://dl.acm.org/doi/10.1145/1328897.1328472)
- [Covington et al. -- Session Types for C](https://dl.acm.org/doi/10.1145/1596553.1596586)
- [Scribble -- Protocol Description Language](https://www.scribble.org/)
- [advanced-type-system-feasibility-plan.md §5](advanced-type-system-feasibility-plan.md)
</content>
</invoke>
