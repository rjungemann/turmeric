# Multi-Party Session Types -- Tradeoff Analysis

> **Status:** Design space reserved -- not scoped for any current release
>
> **Related:** [session-types-plan.md](session-types-plan.md) (binary sessions, SS0--SS4)
>
> **Last updated:** 2026-05-15

---

## Background

The binary session types plan (SS0--SS4) covers two-endpoint protocols: one
sender, one receiver, with a dual-type check ensuring they agree. Many practical
concurrent systems require more than two participants. This document analyses
what it would take to support N-party protocols natively, and what can be
achieved by composing binary sessions instead.

---

## Option A -- Native Multi-Party Session Types

### What it provides

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

The compiler projects this into local types for A, B, and C separately, then
checks each participant's implementation against its local type.

### Guarantees

| Guarantee | Binary sessions | Native multi-party |
|---|---|---|
| Per-channel duality | Yes | Yes (derived from global type) |
| Deadlock freedom (2-party) | Yes | Yes |
| Deadlock freedom (N-party) | **No** | Yes |
| Global protocol coherence | **No** | Yes |
| Role-based type per participant | **No** | Yes |

### Implementation complexity

- **Global type language:** A new syntactic form for global protocol
  declarations (distinct from local session types).
- **Projection algorithm:** A non-trivial algorithm that projects a global type
  onto a local type for each participant. Projection can fail (not every global
  type has a well-defined projection for every role).
- **N-way duality checking:** Instead of checking two endpoints are dual,
  check that all participants' local types are consistent projections of the
  same global type.
- **Scoped channel creation:** Creating a multi-party session requires
  allocating N endpoints simultaneously and distributing them to participants.
- **Prior art for C-targeting:** Limited. Scribble targets JVM/Go. Covington
  et al. (Session Types for C) covers binary only.

Estimated additional complexity over binary sessions: **High** (comparable
in scope to implementing binary sessions from scratch again).

---

## Option B -- Minimal Subset via Binary Composition

### What it provides

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

### What you lose vs. Option A

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

### What you keep

- Coordinator/worker (one coordinator, N workers via N binary channels)
- Fan-out/fan-in (distribute then collect)
- Pipelines (A -> B -> C as two binary channels)
- Any protocol that decomposes naturally into pairwise interactions

These patterns cover the vast majority of systems-programming concurrency
use cases.

### Stdlib support

The minimal subset can be supported with helpers in `stdlib/session.tur`:

```clojure
;; Fan-out: send the same work to N workers
(defn fan-out [channels : (list (Session (Send a (Recv b Close)))), work : a]
  : (list b)
  ...)

;; Pipeline: chain two binary sessions
(defn pipeline
  [^linear ch1 : (Session (Send a (Recv b Close)))
   ^linear ch2 : (Session (Send b (Recv c Close)))]
  : (fn [a] c)
  ...)
```

---

## Recommendation

**Ship Option B (binary composition) in v3--v4.** Reserve the design space
for Option A by:

1. Keeping `Session[P]` extensible -- avoid hardcoding assumptions that limit
   a future multi-party extension.
2. Documenting the binary-composition patterns in `stdlib/session.tur` with
   commentary on their limitations.
3. Emitting a warning (not an error) when a function holds more than two live
   session channels simultaneously -- this flags potential N-party interactions
   for human review without blocking compilation.

### Conditions for reconsidering Option A

- Strong demand from users building distributed systems or consensus protocols
  in Turmeric.
- A C-targeting reference implementation of multi-party session types to draw
  from.
- Binary sessions are stable and widely used, providing a solid foundation to
  build on.

---

## References

- Honda, Yoshida & Carbone -- [Multiparty Asynchronous Session Types](https://dl.acm.org/doi/10.1145/1328897.1328472)
- Scribble -- [Protocol Description Language](https://www.scribble.org/)
- Covington et al. -- [Session Types for C](https://dl.acm.org/doi/10.1145/1596553.1596586) (binary only)
- [session-types-plan.md](session-types-plan.md) -- Binary session implementation plan
