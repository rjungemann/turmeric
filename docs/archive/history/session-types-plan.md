# Session Types -- Implementation Plan (SS0-SS8)

> **Status:** Complete -- SS0--SS8 shipped (`-Xsessions`)
>
> **Target:** v3-v4 (binary sessions, SS0-SS4); v4-v5 (multi-party sessions, SS5-SS8) -- **both tracks complete**
>
> **Prerequisites:** Linear Types (LT0-LT4) complete; thread primitives (Phase T19) complete; Higher-Ranked Types (HRT1) recommended. GADTs (G0–G4) are complete and active (`-Xgadt`); Intersection & Union Types (IT0–IT4) are substantially complete (`-Xunion-types`, `-Xintersection-types`); Substructural Types (ST0–ST3) are complete (`-Xsubstructural`). See interaction notes in the [Interaction with Existing Features](#interaction-with-existing-features) table.
>
> **Related:** [../../guides/advanced-type-system-rationale.md](../../guides/advanced-type-system-rationale.md)
> (§5 Session Types, §1 Linear Types, §8 Effect Types)
> [linear-types-plan.md](linear-types-plan.md),
> [effect-rows-plan.md](effect-rows-plan.md)
>
> **Last updated:** 2026-05-19 (SS0--SS8 complete; see [session-types-guide.md](../guides/session-types-guide.md))

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
  (let [[req chan] (recv chan)
        resp       (handle-request req)
        chan       (send chan resp)]
    (close chan)))

;; Client implementation
(defn client [^linear chan : (Session ClientProtocol)] : unit
  (let [chan        (send chan (make-request))
        [resp chan] (recv chan)]
    (close chan)
    (process-response resp)))
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
      (let [[n chan] (recv chan)
             chan    (send chan n)]
        (echo-server chan))
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
      (let [[[Add x y] chan] (recv chan)
             chan            (send chan (+ x y))]
        (calculator-server chan))
    (Right chan)
      (match (offer chan)
        (Left chan)
          (let [[[Sub x y] chan] (recv chan)
                 chan            (send chan (- x y))]
            (calculator-server chan))
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
  (let [chan      (send-to chan B (make-syn))
        [sa chan] (recv-from chan B)
        chan      (send-to chan C (make-ack))
        [aa chan] (recv-from chan C)]
    (close chan)))

(defn role-b [^linear chan : (Role Handshake B)] : unit
  (let [[syn chan] (recv-from chan A)
        chan       (send-to chan A (make-syn-ack))]
    (close chan)))

(defn role-c [^linear chan : (Role Handshake C)] : unit
  (let [[ack chan] (recv-from chan A)
        chan       (send-to chan A (make-ack-ack))]
    (close chan)))

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
| GADTs (`-Xgadt`) | `offer`/`Branch` returns `(either (Session P) (Session Q))`; inside a `match` arm the elaborator refines the channel type (P or Q) alongside any GADT skolem equalities already in scope — both refinements must compose correctly | `Equal`/`coerce` must not be used to reinterpret a `Session` endpoint at a different protocol step; the elaborator should reject `coerce` whose target is a `Session` type unless the source protocol is provably equal (a legitimate use via `Equal` witnesses is sound but rare); session channels must not appear as phantom GADT type indices unless linearity is tracked through the GADT |
| Intersection & Union Types (`-Xunion-types`) | Session channels must not widen to `any` (widening loses linear tracking); `(offer chan)` returns `(either (Session P) (Session Q))` which with `-Xunion-types` could also be spelled `(Session P \| Session Q)` — in either spelling the elaborator must enforce that exactly one branch is consumed linearly | Intersection `(Session P & Session Q)` must be rejected: it would alias a single linear resource under two protocols simultaneously; the elaborator should emit a type error if a session type appears in an intersection position |
| Substructural Types (`-Xsubstructural`) | `-Xsessions` implies `-Xlinear`; the full substructural framework (`-Xsubstructural`) subsumes linear and is now complete; session channels are `CK_LINEAR` (not `CK_AFFINE` or `CK_UNIQUE`) — both weaker and stronger modes are unsound for sessions | `^affine` on a `Session` type is unsound (you could drop the channel mid-protocol); `^relevant` is unsound (you could use the channel twice); the elaborator must reject any session type annotated with a weaker-than-linear capability kind |
| Effect row polymorphism (`-Xeffect-types`) | With row-polymorphic effects, `send`/`recv` operations carry an explicit effect row; the elaborator's effect-row tracking must interleave with session-state advancement so the channel type advances after an effect is handled and resumed | Effect handlers that intercept `send`/`recv` must return the advanced channel endpoint; handlers that abort rather than resuming leave the session channel unconsumed — the elaborator must enforce that abort-resumption handlers produce a `Session[Close]` or otherwise discharge the linearity obligation |

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
src/compiler/diag.h -- Error codes TUR_E0210-TUR_E0212 (binary), TUR_E0220-TUR_E0223 (multi-party)
```

> **Note:** Error code allocation resolved by P0 (2026-05-18). Binary sessions use
> `TUR_E0210`-`TUR_E0212`; multi-party sessions use `TUR_E0220`-`TUR_E0223`.
> `TUR_E0200`-`TUR_E0202` are already allocated to uniqueness types (UT0).
> The actual implementation file is `src/compiler/diag.h` (not `src/error.h`).

---

## Pre-Phase Prerequisites

These tasks must be completed before any SS phase begins. They are cheap now
and expensive to retrofit once error codes appear in test fixtures or the
projection algorithm is wired into the compiler.

### P0 — Error Code Range Coordination

- [x] Reconcile `TUR_E0200`-`TUR_E0249` with `uniqueness-types-plan.md`, which
  proposes the same range. Lock the final allocation before any SS phase emits
  error codes or writes fixtures that reference them.

  **Resolution (2026-05-18):** There is no separate `uniqueness-types-plan.md`
  file; uniqueness types shipped as part of the substructural track and already
  occupy `TUR_E0200`-`TUR_E0202` in `src/compiler/diag.h`. The remainder of the
  `TUR_E02xx` block is unallocated. Full allocation as of this audit:

  | Range | Owner | Status |
  |---|---|---|
  | `TUR_E0100`-`TUR_E0104` | Linear types (`-Xlinear`, LT0-LT1) | Active |
  | `TUR_E0150`-`TUR_E0151` | Substructural types (`-Xsubstructural`, ST0) | Active |
  | `TUR_E0200`-`TUR_E0202` | Unique types (UT0) | Active |
  | `TUR_E0203`-`TUR_E0249` | **Unallocated** | Free |
  | `TUR_E0250`-`TUR_E0254` | Effect row types (`-Xeffect-types`) | Active |
  | `TUR_E0300`-`TUR_E0301` | Union types (`-Xunion-types`) | Active |
  | `TUR_E0350`-`TUR_E0351` | Intersection types (`-Xintersection-types`) | Active |
  | `TUR_E0400`-`TUR_E0401` | Contracts | Active |
  | `TUR_E0500`-`TUR_E0502` | Multishot continuations | Active |

  **Locked allocation for sessions:**
  - Binary sessions: `TUR_E0210`-`TUR_E0212`
  - Multi-party sessions: `TUR_E0220`-`TUR_E0223`
  - Gaps `TUR_E0203`-`TUR_E0209`, `TUR_E0213`-`TUR_E0219`, `TUR_E0224`-`TUR_E0249`
    remain unallocated as headroom.

### P1 — Linear Types Prerequisite Audit

- [x] Verify which LT phases provide the elaborator machinery session types
  depend on, and confirm they are implemented (not just marked complete in the plan):
  - Which LT phase implements `CK_LINEAR` / `CK_AFFINE` / `CK_UNIQUE`
    capability-kind tracking? (needed by SS0a and the `^affine Session` guard in SS1)
  - Which LT phase provides the elaborator linearity enforcement that SS1 re-uses
    for protocol progress checking?

  **Findings (2026-05-18):**

  - **`CK_LINEAR` -- LT0** (`src/compiler/types.h`, line 22): `CK_LINEAR` is a
    live variant in the `CopyKind` enum, gated behind `-Xlinear`. `TY_LREF` uses
    it as its default copy kind. `ty_is_linear()` predicate is available.
    **Required by SS0a -- confirmed present.**

  - **Linearity enforcement -- LT1** (`src/compiler/elab_*.c`): `is_linear` /
    `is_linear_consumed` flags live on `Binding` (`src/compiler/expr.h`).
    Consumption is tracked in `elab_form` (F_SYM case); scope-exit drop checks
    run in `elab_let` and `elab_defn`; branch-aware consumption (if/match arms
    must agree) is complete. Emits `TUR_E0100`-`TUR_E0104`.
    **Required by SS1 for protocol progress checking -- confirmed present.**

  - **`CK_AFFINE` and `CK_RELEVANT`** do NOT exist as `CopyKind` enum variants.
    Affine and relevant tracking is implemented via per-parameter boolean flags
    (`arg_affine[]` / `arg_relevant[]` on `FnType`; `is_affine` / `is_relevant`
    on `Binding`) in ST0 (`-Xsubstructural`). The `^affine Session` guard in SS1
    must therefore check the binding's `is_affine` flag rather than a `CopyKind`
    value. Since `-Xsessions` implies `-Xsubstructural`, this machinery is always
    available when session types are active.

### P2 — Test Fixture Baseline

- [x] Create session fixture baseline alongside SS0a
- [x] Add at least one happy-path and one negative-case fixture per core
  operation: `send`, `recv`, `close`, `offer`, `choose-left`, `choose-right`
- [x] Each subsequent SS phase adds its own fixtures; this baseline gives every
  phase a clear red/green criterion from day one and prevents regressions as
  later phases layer on top

  **Fixtures created (2026-05-18):** All fixtures use the `-Xsessions` flag
  and are intentionally red until SS0b-SS1 are implemented.

  Happy-path (`tests/fixtures/session-*/`):

  | Fixture | Operations | Expected output |
  |---|---|---|
  | `session-send` | `make-session`, `send`, `recv`, `close`, `spawn`, `join` | `42` |
  | `session-recv` | `make-session`, `recv`, `send`, `close`, `spawn`, `join` | `99` |
  | `session-close` | `make-session Close`, `close` (both endpoints, no thread needed) | `closed` |
  | `session-offer` | `make-session`, `offer`, `choose-left`, `send`, `recv`, `close` | `7` |
  | `session-choose-left` | `make-session`, `choose-left`, `offer`, `close` | `left` |
  | `session-choose-right` | `make-session`, `choose-right`, `offer`, `close` | `right` |

  Negative (`tests/fixtures/errors/session-*/`):

  | Fixture | Violation | Expected code |
  |---|---|---|
  | `session-send-invalid` | `send` on `Session[Recv int Close]` | `TUR-E0212` |
  | `session-recv-invalid` | `recv` on `Session[Send int Close]` | `TUR-E0212` |
  | `session-close-incomplete` | `close` on `Session[Send int Close]` (not at `Close` step) | `TUR-E0212` |
  | `session-dropped` | Session channel goes out of scope mid-protocol | `TUR-E0211` |
  | `session-offer-invalid` | `offer` on `Session[Send int Close]` (not a `Branch`) | `TUR-E0212` |
  | `session-choose-invalid` | `choose-left` on `Session[Recv int Close]` (not a `Choose`) | `TUR-E0212` |

  Note on happy-path fixtures: session channels are linear and require concurrent
  endpoints. The happy-path fixtures use a planned `spawn`/`join` API (a zero-arg
  closure that moves linear captures into the new thread). This API will be
  implemented as part of SS2/SS4. The `session-close` fixture is the exception:
  `Close` is self-dual, so both endpoints can be closed in the same thread.

### P3 — Projection Algorithm Spike (required before SS5)

- [x] Implement a standalone reference version of the projection algorithm
  (Python or pseudocode) against the Honda/Yoshida/Carbone rules before
  starting the SS5 global-type front end
- [x] Validate against at least four cases:
  - `ThreeWayHandshake` (Example 4) -- straightforward sequential interactions
  - A `choice`-containing protocol -- exercises the uninvolved-role merge
  - A recursive global protocol -- exercises `loop`/`continue` projection
  - A non-projectable protocol -- confirms the partial-function failure case
    triggers `TUR_E0220` correctly
- [x] Projection rules to validate:
  - `(-> R X T) then rest` projects for `R` to `Send[T, project(rest, R)]`
  - `(-> X R T) then rest` projects for `R` to `Recv[T, project(rest, R)]`
  - `(-> X Y T) then rest` with `R` uninvolved projects to `project(rest, R)`
  - `choice` for deciding role → `Choose`; notified roles → `Branch`; uninvolved
    roles → merge (fails if branches are not mergeable)

  **Resolution (2026-05-18):** Implemented as `tools/project.py` (11/11 checks
  pass). Key design notes carried forward to SS6:

  - `_involves(body, role)` uses a **shallow check** -- it detects top-level
    `GMsg` and the deciding role of a top-level `GChoice`, but does NOT recurse
    into nested choice branches. This is what allows the uninvolved-merge path
    to apply (and fail) for roles that only appear inside nested choices, which
    is the correct MPST semantics for the non-projectable case.
  - `GLoop` bodies ARE recursed into by `_involves` (loops are sequential, not
    branching), so roles inside a loop are correctly marked as involved.
  - Loops are projected as `Rec[label, project(body, R)]`; the `LRecVar` from
    `GContinue` nodes is detected by `_has_recvar` to decide whether to emit
    the `Rec` wrapper or skip it (role not in loop).
  - The `_merge` function performs structural equality up to recursive merge on
    sub-terms; merge of `Branch`/`Choose` types requires identical label lists
    in the same order.
  - **Limitation (acceptable for SS6):** `GLoop` nodes must be the last
    element in their enclosing sequence; interactions after a `Loop` are not
    threaded into the loop's exit points. All validated test cases are
    self-contained loops; SS6 should handle this properly.

---

## Phase SS0a — Session Type Data Model

**Goal:** Add session type constructors to the type system; define the channel
creation primitive. No parser or elaborator behaviour yet.

- [x] Add to `TypeKind` in `src/compiler/types.h` (actual path per P0 audit):

  ```c
  TY_SESSION,      /* Session[P] -- a channel carrying protocol P */
  TY_SEND,         /* Send[T, Q] -- send T then continue as Q */
  TY_RECV,         /* Recv[T, Q] -- receive T then continue as Q */
  TY_CLOSE,        /* Close -- protocol complete */
  TY_CHOOSE,       /* Choose[P, Q] -- internal choice (sender picks) */
  TY_BRANCH,       /* Branch[P, Q] -- external choice (receiver picks) */
  TY_SESSION_REC,  /* Rec[label, F] -- recursive protocol (mu-type) */
  ```

  > **Note:** `TY_REC` already exists (HKT-P2 recursive type binder). The
  > session recursive protocol uses `TY_SESSION_REC` to avoid the collision.

- [x] `make-session` is deferred to SS0b (parser/elaborator). SS0a only
  establishes the `TY_SESSION` TypeKind and its `CK_LINEAR` assignment so
  the construct exists in the data model before SS0b wires it up.
- [x] Assign `CK_LINEAR` to `TY_SESSION`; protocol descriptor types
  (`TY_SEND`, `TY_RECV`, etc.) are `CK_MOVE` (type-level only, never values)
- [x] `session_` union field added to `Type.as`; type constructors
  `type_session`, `type_send`, `type_recv`, `type_close`, `type_choose`,
  `type_branch`, `type_session_rec` added as `static inline` functions
- [x] `typekind_to_string` / `typekind_from_name` / `type_name` /
  `type_name_buf` / `type_c_name` / `type_is_guarded_recursive_helper`
  all updated with session cases in `src/compiler/types.c`
- [x] Baseline fixtures from P2 already present in
  `tests/fixtures/session-*/` and `tests/fixtures/errors/session-*/`;
  all intentionally red until SS0b-SS1 implement the operations

  **Implementation notes (2026-05-18):**
  - `TY_SESSION` gets `CK_LINEAR + SK_LINEAR`; all others get `CK_MOVE`
  - `type_c_name(TY_SESSION)` returns `"TurChannel *"` (SS2 will emit the
    actual struct definition; for now only the pointer type is needed)
  - Protocol descriptors (`TY_SEND` etc.) return `"/*session-protocol*/ void"`
    from `type_c_name` since they are never runtime values
  - Inline constructors use `= {0}` zero-initialisation (avoids a
    `<string.h>` dependency in the header)

---

## Phase SS0b — Session Type Parser and Elaborator ✓ COMPLETE

**Goal:** Parse session type syntax and implement all six channel operations in
the elaborator.

- [x] Parse session type syntax in `src/compiler/elab_types.c` (not reader.c):
  `Session`, `Send`, `Recv`, `Close`, `Choose`, `Branch` as type constructors;
  gated on `-Xsessions` / `g_sessions_enabled`.
  `Rec` stub deferred to SS3 (recursive protocols).
- [x] Add session channel operations to the elaborator (`src/compiler/elab_sessions.c`):
  - `(send chan val)` -- consumes `chan : Session[Send[T, Q]]`; returns `chan' : Session[Q]`
  - `(recv chan)` -- consumes `chan : Session[Recv[T, Q]]`; returns placeholder (SS2 for real tuple)
  - `(close chan)` -- consumes `chan : Session[Close]`; returns `unit`
  - `(offer chan)` -- consumes `chan : Session[Branch[P, Q]]`; returns placeholder (SS2 for ADT)
  - `(choose-left chan)` -- consumes `chan : Session[Choose[P, Q]]`; returns `Session[P]`
  - `(choose-right chan)` -- consumes `chan : Session[Choose[P, Q]]`; returns `Session[Q]`
  - `(make-session P)` -- returns `TY_INT` placeholder; SS2 implements the pair creation
- [x] `TUR_E0211` / `TUR_E0212` added to `diag.h` and `diag.c`
- [x] `-Xsessions` flag added to `main.c` (implies -Xsubstructural, -Xlinear)
- [x] `g_sessions_enabled` global in `globals.h` / `globals.c`
- [x] All 6 error fixtures pass: session-dropped, session-send-invalid,
  session-recv-invalid, session-close-incomplete, session-offer-invalid,
  session-choose-invalid
- [x] `elab_let` extended with basic vector destructuring support (`(let [[a b] init] ...)`)
  to allow let-binding of session op results

**Notes:**
- Type parsing lives in `elab_types.c` (not reader.c) following the pattern of
  `lref`, `forall`, etc. The reader itself doesn't need changes.
- `make-session` returns a `TY_INT` placeholder at SS0b; actual pair-creation
  codegen (returning `[Session[P], Session[Dual[P]]]`) is SS2.
- `recv` and `offer` return `TY_INT` placeholder; the tuple/Either return types
  are SS2 work.
- Vector destructuring in `let` creates placeholder-typed bindings for non-first
  elements; full type inference is deferred to the tuple-type work in SS2.

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

- [x] Implement `dual(P)` function in `src/compiler/elab_sessions.c` (`session_dual()`)
- [x] When a session channel is created (`make-session`), parse the protocol type with `type_expr_from_form`, compute `dual(P)`, and return `TY_SESSION_PAIR` carrying both `Session[P]` and `Session[dual(P)]` endpoints; vector destructuring decomposes `TY_SESSION_PAIR` into the two typed bindings
- [x] Emit error `TUR_E0210` when duality fails
- [x] **GADT `coerce` guard:** added in `elab_structs.c` (`elab_coerce`) -- rejects any `coerce` whose source expression is `TY_SESSION`

### Protocol Progress

- [x] A session channel must be fully consumed (all `send`/`recv` steps completed, ending at `Close`) before the channel binding goes out of scope
- [x] Re-use linear type machinery: `chan : Session[Close]` must be passed to `close` before scope exit
- [x] Emit `TUR_E0211` when a non-`Close` session is dropped (message now includes current protocol state)

### Error codes

| Code | Message |
|---|---|
| `TUR_E0210` | Session endpoint types are not dual: `{P}` vs. `{Q}` |
| `TUR_E0211` | Session channel `{name}` dropped before protocol completion (expected `Close`, got `{P}`) |
| `TUR_E0212` | Operation `{op}` not valid on session with protocol `{P}` |

---

## Phase SS2 — Session Codegen

**Goal:** Emit message-passing primitives and channel structs to C.

- [x] Define a typed channel struct in generated C (`TurChannel` with two
  `TurSyncCh` sub-channels -- one for data, one for branch tags -- plus a
  reference count and a mutex; synchronous rendezvous via pthread condvars)
- [x] Emit `send`, `recv`, `close`, `offer`, `choose-left`, `choose-right`
  as inline C snippets via the `EX_INLINE_C` substitution mechanism
- [x] The session type is erased at runtime: no tag or protocol descriptor
  carried at runtime in release builds
- [x] Optional: `TUR_DBGPROTO("Send[int, Close]")` macro embeds the initial
  protocol name as a `const char *dbg_proto` field on `TurChannel` in debug
  builds (NDEBUG off); expands to null pointer in release builds; useful for
  inspecting channel protocol at FFI boundaries in a debugger

---

## Phase SS3a -- Recursive Protocols

**Goal:** Support `mu X. F X` recursive protocols (equirecursive).

- [x] **Recursive protocols (`Rec`):** support `mu X. F X` for protocols that repeat
  - `EchoServer = Branch (Recv int (Send int EchoServer)) Close`
  - Equirecursive: protocol types are transparently equal to their unrolled
    forms; no `fold`/`unfold` at recursive call sites
- [x] **Co-inductive equality:** protocol equality checked co-inductively with a
  "seen" set to prevent looping. Share or reuse the co-inductive guard already
  present in `type_equiv`/`type_unify` in `src/elab.c` (used for recursive
  ADTs under `-Xgadt`) rather than implementing a separate scheme.
- [x] Add fixtures for recursive protocol happy-path and drop-before-close error
  - `tests/fixtures/session-rec/` -- echo server using `Rec self (Branch (Recv int (Send int self)) Close)`; outputs `1\n2\n3\n`
  - `tests/fixtures/errors/session-rec-dropped/` -- `close` called before protocol complete; emits TUR-E0212

---

## Phase SS3b -- Delegation and Session Subtyping

**Goal:** Allow protocol ownership transfer and covariant external-choice subtyping.

- [x] **Delegation:** a session channel can be passed to another thread or
  function, transferring protocol ownership. The receiving function must fully
  complete the delegated protocol; the delegating side loses the binding.
- [x] **Session subtyping:** a server offering `Branch[P, Q, R]` is usable
  where `Branch[P, Q]` is expected (external choice subtyping is covariant in
  offered options)
- [x] Add fixtures for delegation transfer and subtyping acceptance/rejection
  - `tests/fixtures/session-delegation/` -- `delegate-send` takes ownership of `Session[Send int Close]` from main, completes it; outputs `42`

---

## Phase SS3c -- Timeout Channels

**Goal:** Typed timeout support for `recv`.

- [x] Add `TY_TIMEOUT` to `TypeKind` alongside `TY_SEND`/`TY_RECV`
- [x] `(recv-timeout chan duration)` -- on message receipt the protocol
  continues as `Q`; on timeout it continues as `P`. Both branches return the
  channel so the session can continue.
  - Type: `(Recv T (Timeout Q P))` -- `Timeout` is self-dual (both endpoints
    see the same branching outcome)
  - `recv-timeout` returns a `Left [value continuation]` pair on success, or
    `Right continuation` on timeout. The `TurChannel` gains an `abandoned` flag
    so that when the receiver closes after a timeout, any blocked sender wakes
    up and exits cleanly rather than deadlocking.
- [x] Add fixtures for timeout-received and timeout-expired paths
  - `tests/fixtures/session-timeout-ok/` -- sender sends before deadline; receiver prints `42`
  - `tests/fixtures/session-timeout-expired/` -- sender sleeps 200 ms; receiver times out at 50 ms; prints `timeout`

---

## Phase SS4 — Integration

**Goal:** Stdlib protocols, effect integration, and STM.

- [ ] Stdlib session patterns in `stdlib/session.tur`:
  - `echo` -- trivial echo protocol
  - `rpc` -- generic request-response
  - `pubsub` -- publish-subscribe protocol template
- [ ] Integration with effects: `recv` and `send` may be declared as effects so that handlers can intercept message passing (useful for testing, logging, mock protocols). With `-Xeffect-types` active, verify that the effect handler elaboration path correctly handles the advanced session channel in the handler's return type; handlers that abort rather than resuming must discharge the linearity obligation (e.g. by closing the channel).
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
  (let [ch-a         (send ch-a work-a)
        ch-b         (send ch-b work-b)
        [res-a ch-a] (recv ch-a)
        [res-b ch-b] (recv ch-b)]
    (close ch-a)
    (close ch-b)
    (combine res-a res-b)))
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

### Binary sessions (SS0a-SS4)

| Aspect | Complexity | Notes |
|---|---|---|
| Type system changes (SS0a) | Low-Medium | Seven new `TypeKind` variants; `make-session`; `CK_LINEAR` assignment -- additive only |
| Parser + elaborator ops (SS0b) | Medium | Six channel operations; GADT/union-types interaction |
| Duality + progress checking (SS1) | High | Protocol progress checking; duality checking; GADT coerce guard |
| Codegen (SS2) | Medium | Channel struct emission; typed send/recv wrappers |
| Recursive protocols (SS3a) | High | Co-inductive equality; must integrate with existing GADT `type_equiv` guard |
| Delegation + subtyping (SS3b) | Medium | Protocol ownership transfer; covariant external-choice subtyping |
| Timeout channels (SS3c) | Low-Medium | Self-contained new type kind; typed branch on timeout |
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

Enabling `-Xsessions` implicitly enables `-Xlinear` (channels are linear). Because `-Xsubstructural` now subsumes `-Xlinear` and is complete (ST0–ST3), `-Xsessions` also implicitly enables `-Xsubstructural`; this ensures the capability-kind machinery (`CK_LINEAR` / `CK_AFFINE` / `CK_UNIQUE`) is available to reject unsound `^affine` or `^relevant` annotations on session channels.

The `-Xsessions` flag gates both binary and multi-party forms. The
`(defprotocol ...)` global-protocol form and the `Role` type are available only
under `-Xsessions`; until SS5 lands they are rejected with a "not yet
implemented" diagnostic.

> **Note:** `-Xsessions` does **not** implicitly enable `-Xgadt` or `-Xunion-types`. The GADT and union-type elaboration paths interact with session types (see interaction table) but are separate opt-in features; a program may use session types without either.

---

## Implementation Priority

**Pre-phase tasks (P0-P3):** Complete before any SS phase. P0 (error range) and P1 (LT audit) are one-time coordination tasks; P2 (fixture baseline) is done alongside SS0a; P3 (projection spike) must be done before SS5.

**Binary sessions (SS0a-SS4): Medium-High** — v3-v4, after Linear Types (LT0-LT4) and thread primitives (Phase T19).

Session types are the highest-value concurrency safety feature available. They build directly on linear types and are well-understood theoretically. The primary complexity is in protocol progress and duality checking; the codegen is relatively straightforward.

Start with the data model and test infrastructure (SS0a), then the parser and elaborator operations (SS0b), then ship simple send/receive protocols (SS1-SS2). Add recursive protocols first (SS3a), then delegation and subtyping (SS3b), then timeouts (SS3c), then the integration phase (SS4).

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
- [../../guides/advanced-type-system-rationale.md §5](../../guides/advanced-type-system-rationale.md)

---

## Tutorial: Two-Phase Commit

Two-phase commit (2PC) is a distributed coordination protocol that ensures all
participants in a transaction either commit or abort together. It is a natural
fit for multi-party session types: a single coordinator role drives the protocol
through two distinct phases (prepare then commit/abort), and the global type
enforces that the coordinator's decision is broadcast identically to every
participant -- a property that binary sessions cannot verify.

### Protocol overview

In the prepare phase the coordinator sends a `Prepare` message to each
participant and waits for a `Vote` reply. If all participants vote `Yes`, the
coordinator sends `Commit` to everyone; if any participant votes `No`, the
coordinator sends `Abort` to everyone. The session closes after the second
broadcast.

### Global protocol declaration

The example fixes two participants (`P1` and `P2`). Scaling to more participants
requires repeating the send/receive pairs for each additional role; a future
variadic-role extension would allow `(-> Coordinator [P ...] Prepare)`.

```clojure
(defprotocol TwoPhaseCommit [Coordinator P1 P2]
  ;; Phase 1: prepare
  (-> Coordinator P1 Prepare)
  (-> Coordinator P2 Prepare)
  (-> P1 Coordinator Vote)
  (-> P2 Coordinator Vote)
  ;; Phase 2: commit or abort
  (choice Coordinator
    [commit
      (-> Coordinator P1 Commit)
      (-> Coordinator P2 Commit)]
    [abort
      (-> Coordinator P1 Abort)
      (-> Coordinator P2 Abort)]))
```

### Projected local types

The compiler projects `TwoPhaseCommit` onto each role automatically (SS6). The
projected types are shown here for reference; a programmer using
`(Role TwoPhaseCommit Coordinator)` never writes them by hand.

```clojure
;; (project TwoPhaseCommit Coordinator)
(Send Prepare
  (Send Prepare
    (Recv Vote
      (Recv Vote
        (Choose
          (Send Commit (Send Commit Close))
          (Send Abort  (Send Abort  Close)))))))

;; (project TwoPhaseCommit P1)
(Recv Prepare
  (Send Vote
    (Branch
      (Recv Commit Close)
      (Recv Abort  Close))))

;; (project TwoPhaseCommit P2)  -- identical structure to P1
(Recv Prepare
  (Send Vote
    (Branch
      (Recv Commit Close)
      (Recv Abort  Close))))
```

Note that `P1` and `P2` each project to a `Branch` even though neither of them
is the deciding role. The projection algorithm propagates the coordinator's
`choice` to all involved roles: the deciding role gets `Choose` (internal
choice) and every other involved role gets `Branch` (external choice). Roles
that do not participate in a branch at all would instead receive a merged
projection of the two branches; here both branches involve `P1` and `P2`, so
the merge step is not exercised.

### Broken global type -- non-projectable example

A common protocol error is a broadcast that reaches only some participants.
The following variant forgets to send `Abort` to `P2`:

```clojure
;; BUG: abort branch omits (-> Coordinator P2 Abort)
(defprotocol BrokenCommit [Coordinator P1 P2]
  (-> Coordinator P1 Prepare)
  (-> Coordinator P2 Prepare)
  (-> P1 Coordinator Vote)
  (-> P2 Coordinator Vote)
  (choice Coordinator
    [commit
      (-> Coordinator P1 Commit)
      (-> Coordinator P2 Commit)]
    [abort
      (-> Coordinator P1 Abort)]))   ; P2 is missing here
```

`P2` is involved in the commit branch (`Recv Commit Close`) but uninvolved in
the abort branch. The projection algorithm tries to merge `Recv Commit Close`
with the abort-branch projection for `P2`, which is `Close`. Those are not
structurally equal, so the merge fails and the compiler emits `TUR_E0220`:

```
error[TUR-E0220]: global protocol `BrokenCommit` is not projectable onto
  role `P2` at interaction `choice Coordinator [commit ...] [abort ...]`:
  branches produce incompatible local types
    commit branch: (Recv Commit Close)
    abort branch:  Close
```

### Message types

```clojure
(defstruct Prepare [transaction-id :int])
(defstruct Vote    [transaction-id :int yes :bool])
(defstruct Commit  [transaction-id :int])
(defstruct Abort   [transaction-id :int])
```

### Role implementations

```clojure
;; Coordinator: sends Prepare, collects votes, decides, broadcasts outcome
(defn coordinator [^linear chan : (Role TwoPhaseCommit Coordinator)
                   txn-id : int
                   should-commit : bool] : unit
  (let [chan         (send-to chan P1 (Prepare txn-id))
        chan         (send-to chan P2 (Prepare txn-id))
        [vote1 chan] (recv-from chan P1)
        [vote2 chan] (recv-from chan P2)]
    (if (and (Vote.yes vote1) (Vote.yes vote2))
      (let [chan (choose-left chan)
            chan (send-to chan P1 (Commit txn-id))
            chan (send-to chan P2 (Commit txn-id))]
        (close chan))
      (let [chan (choose-right chan)
            chan (send-to chan P1 (Abort txn-id))
            chan (send-to chan P2 (Abort txn-id))]
        (close chan)))))

;; Participant: receives Prepare, votes, then awaits commit or abort
(defn participant [^linear chan : (Role TwoPhaseCommit P1)
                   ready : bool] : unit
  (let [[prep chan] (recv-from chan Coordinator)
        chan        (send-to chan Coordinator (Vote (Prepare.transaction-id prep) ready))]
    (match (offer chan)
      (Left chan)
        (let [[_ chan] (recv-from chan Coordinator)]   ; Commit
          (close chan))
      (Right chan)
        (let [[_ chan] (recv-from chan Coordinator)]   ; Abort
          (close chan)))))

;; Spawn all three roles from a single scoped protocol
(defn run-two-phase-commit [txn-id : int] : unit
  (let [[coord p1 p2] (make-protocol TwoPhaseCommit)]
    (spawn (fn [] (coordinator coord txn-id true)))
    (spawn (fn [] (participant p1 true)))
    (participant p2 true)))
```

### What the type checker verifies

- **Coordinator** must make a `choose-left`/`choose-right` call before sending
  `Commit`/`Abort`; skipping the choice call is a type error (`TUR_E0212`).
- **Coordinator** must send to both `P1` and `P2` in each branch; sending to
  only one is a projection mismatch caught at the `defprotocol` level
  (`TUR_E0220`), not at the call site.
- **Participant** (`P1` or `P2`) must match on `offer` before it knows whether
  it will receive `Commit` or `Abort`; reading `Commit` unconditionally is a
  type error (`TUR_E0212`).
- All three channels are linear: dropping `coord`, `p1`, or `p2` mid-protocol
  is a type error (`TUR_E0211`).

---

## Tutorial: OAuth-Style Auth Flow

OAuth 2.0's authorization code flow involves three parties: a Client that wants
a protected resource, an AuthServer that issues tokens, and a ResourceServer
that holds the resource and validates tokens. The interaction is fully
sequential with no branching -- every step is a rendezvous -- which makes it a
clean example of a projectable global protocol where the uninvolved-role
skip rule is exercised at every step.

### Protocol overview

1. Client sends an authorization request to AuthServer.
2. AuthServer issues an authorization code back to Client.
3. Client exchanges the code for an access token (sends `TokenRequest`).
4. AuthServer issues the access token.
5. Client sends an API request to ResourceServer, including the token.
6. ResourceServer asks AuthServer to validate the token.
7. AuthServer confirms the token is valid.
8. ResourceServer returns the API response to Client.

### Global protocol declaration

```clojure
(defprotocol OAuthFlow [Client AuthServer ResourceServer]
  (-> Client      AuthServer     AuthRequest)
  (-> AuthServer  Client         AuthCode)
  (-> Client      AuthServer     TokenRequest)
  (-> AuthServer  Client         AccessToken)
  (-> Client      ResourceServer ApiRequest)
  (-> ResourceServer AuthServer  ValidateToken)
  (-> AuthServer  ResourceServer TokenOk)
  (-> ResourceServer Client      ApiResponse))
```

### Projected local types

```clojure
;; (project OAuthFlow Client)
(Send AuthRequest
  (Recv AuthCode
    (Send TokenRequest
      (Recv AccessToken
        (Send ApiRequest
          (Recv ApiResponse Close))))))

;; (project OAuthFlow AuthServer)
(Recv AuthRequest
  (Send AuthCode
    (Recv TokenRequest
      (Send AccessToken
        (Recv ValidateToken
          (Send TokenOk Close))))))

;; (project OAuthFlow ResourceServer)
(Recv ApiRequest
  (Send ValidateToken
    (Recv TokenOk
      (Send ApiResponse Close))))
```

`ResourceServer` is uninvolved in the first four steps of the global protocol
(`AuthRequest`, `AuthCode`, `TokenRequest`, `AccessToken`). Its projection
skips those steps entirely, starting only at step 5 (`ApiRequest`). Likewise
`Client` is uninvolved in steps 6 and 7 (`ValidateToken`/`TokenOk`) and its
projection jumps from `Send ApiRequest` directly to `Recv ApiResponse`. The
compiler applies the uninvolved-role skip rule at each of these steps.

### Message types

```clojure
(defstruct AuthRequest   [client-id :str scope :str])
(defstruct AuthCode      [code :str expires-in :int])
(defstruct TokenRequest  [code :str client-id :str client-secret :str])
(defstruct AccessToken   [token :str token-type :str expires-in :int])
(defstruct ApiRequest    [token :str path :str])
(defstruct ValidateToken [token :str])
(defstruct TokenOk       [subject :str scope :str])
(defstruct ApiResponse   [status :int body :str])
```

### Role implementations

```clojure
;; Client: drives the auth code exchange, then fetches the resource
(defn oauth-client [^linear chan : (Role OAuthFlow Client)
                    client-id : str
                    scope : str
                    client-secret : str
                    path : str] : ApiResponse
  (let [chan         (send-to chan AuthServer (AuthRequest client-id scope))
        [code chan]  (recv-from chan AuthServer)
        chan         (send-to chan AuthServer
                       (TokenRequest (AuthCode.code code) client-id client-secret))
        [tok chan]   (recv-from chan AuthServer)
        chan         (send-to chan ResourceServer
                       (ApiRequest (AccessToken.token tok) path))
        [resp chan]  (recv-from chan ResourceServer)]
    (close chan)
    resp))

;; AuthServer: issues codes, exchanges tokens, validates on demand
(defn oauth-auth-server [^linear chan : (Role OAuthFlow AuthServer)] : unit
  (let [[req chan]  (recv-from chan Client)
        code        (issue-auth-code (AuthRequest.client-id req)
                                     (AuthRequest.scope req))
        chan        (send-to chan Client (AuthCode code 600))
        [treq chan] (recv-from chan Client)
        tok         (exchange-code-for-token
                      (TokenRequest.code treq)
                      (TokenRequest.client-id treq))
        chan        (send-to chan Client (AccessToken tok "Bearer" 3600))
        [vtok chan] (recv-from chan ResourceServer)
        subject     (validate-token (ValidateToken.token vtok))
        chan        (send-to chan ResourceServer
                      (TokenOk subject (AuthRequest.scope req)))]
    (close chan)))

;; ResourceServer: receives a request, validates the token, returns data
(defn oauth-resource-server [^linear chan : (Role OAuthFlow ResourceServer)] : unit
  (let [[req chan] (recv-from chan Client)
        chan       (send-to chan AuthServer
                     (ValidateToken (ApiRequest.token req)))
        [ok chan]  (recv-from chan AuthServer)
        body       (fetch-resource (ApiRequest.path req) (TokenOk.subject ok))
        chan       (send-to chan Client (ApiResponse 200 body))]
    (close chan)))

;; Wire all three roles together
(defn run-oauth-flow [client-id : str
                      client-secret : str
                      scope : str
                      path : str] : ApiResponse
  (let [[client auth resource] (make-protocol OAuthFlow)
        result                 (promise)]
    (spawn (fn [] (oauth-auth-server auth)))
    (spawn (fn [] (oauth-resource-server resource)))
    (let [resp (oauth-client client client-id scope client-secret path)]
      resp)))
```

### What the type checker verifies

- **Client** cannot skip the code-exchange steps and jump straight to the API
  request; the type system requires each `send-to`/`recv-from` in the exact
  order the global protocol specifies.
- **ResourceServer** cannot call `recv-from chan Client` before the first four
  steps of the protocol complete -- its projected type begins with
  `Recv ApiRequest`, so attempting to receive from `AuthServer` first would be
  a `TUR_E0212` type error.
- **AuthServer** must respond to the `ValidateToken` query from `ResourceServer`
  before closing; dropping the channel after the token exchange but before
  step 6 would be `TUR_E0211`.
- All three endpoints are linear and must be fully consumed through `close`.
</content>
</invoke>
