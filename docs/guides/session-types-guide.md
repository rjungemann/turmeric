---
title: Session Types Guide
category: Concurrency and Async
description: Model protocols as types, whether the protocol has two participants, or more
---

# Session Types Guide

> This guide supersedes the archived
> [`stdlib-session-typed-channels-plan`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/stdlib-session-typed-channels-plan.md).
> All phases (S1--S3) shipped; the user-facing reference for `stdlib/schan.tur`
> and the broader session-types story is here.

Turmeric supports session types -- a type discipline that statically verifies
communication protocols between concurrent processes. The feature is enabled
by default; no compiler flag is required.

## Table of Contents

- [Session Types Guide](#session-types-guide)
  - [Table of Contents](#table-of-contents)
  - [Binary Session Types (SS0-SS4)](#binary-session-types-ss0-ss4)
    - [make-session, send, recv, close](#make-session-send-recv-close)
    - [Choice: choose-left, choose-right, offer](#choice-choose-left-choose-right-offer)
    - [Recursive protocols: Rec](#recursive-protocols-rec)
    - [Timeouts](#timeouts)
    - [Duality](#duality)
    - [Effect integration](#effect-integration)
  - [Multi-Party Session Types (SS5-SS8)](#multi-party-session-types-ss5-ss8)
    - [defprotocol](#defprotocol)
    - [make-protocol, send-to, recv-from, close](#make-protocol-send-to-recv-from-close)
    - [Three or more roles](#three-or-more-roles)
    - [Projection algorithm](#projection-algorithm)
  - [Session-Typed Channel Wrappers (stdlib/schan.tur)](#session-typed-channel-wrappers-stdlibschantur)
  - [Error Codes](#error-codes)
  - [Getting More Help](#getting-more-help)

---

## Binary Session Types (SS0-SS4)

Binary session types describe a two-party communication channel. One end of the
channel runs a _protocol_ `P`; the other end runs the _dual_ protocol `dual(P)`.

### make-session, send, recv, close

```turmeric
;; Allocate a two-ended channel for the protocol Send<int, Close>.
(let [[a b] (make-session (Send int Close))]
  ...)
```

```sweet-exp
;; Allocate a two-ended channel for the protocol Send<int, Close>.
let [[a b] make-session((Send int Close))]
  ...
```

The pair `[a b]` gives both endpoints. Endpoint `a` has type
`Session[Send int Close]`; endpoint `b` has the dual type
`Session[Recv int Close]`.

**send** -- advance the sender side by one message:

```turmeric
(let [a (send a 42)]  ; a advances from Send<int,Close> to Close
  (close a))
```

```sweet-exp
let [a send(a 42)]  ; a advances from Send<int,Close> to Close
  close(a)
```

**recv** -- advance the receiver side by one message; returns `[value new-ch]`:

```turmeric
(let [[v b] (recv b)]  ; v = 42, b advances from Recv<int,Close> to Close
  (close b))
```

```sweet-exp
let [[v b] recv(b)]  ; v = 42, b advances from Recv<int,Close> to Close
  close(b)
```

**close** -- consume the channel after the protocol ends (type `Close`).

### Choice: choose-left, choose-right, offer

When the sender can choose between two branches use `choose-left` / `choose-right`;
the receiver uses `offer` and pattern-matches on `Left`/`Right`:

```turmeric
;; Sender side: Choose<Send int Close, Close>
(let [ch (choose-left ch)]  ; picks the Left branch
  (let [ch (send ch 7)]
    (close ch)))

;; Receiver side: Branch<Recv int Close, Close>
(match (offer ch)
  (Left ch)
    (let [[n ch] (recv ch)]
      (close ch))
  (Right ch)
    (close ch))
```

```sweet-exp
;; Sender side: Choose<Send int Close, Close>
let [ch choose-left(ch)]  ; picks the Left branch
  let [ch send(ch 7)]
    close(ch)

;; Receiver side: Branch<Recv int Close, Close>
match offer(ch)
  (Left ch)
  let [[n ch] recv(ch)]
    close(ch)
  (Right ch)
  close(ch)
```

### Recursive protocols: Rec

`Rec` introduces a recursive protocol variable `self` that unrolls on each
loop iteration:

```turmeric
;; Server: repeat (recv int, send int) until client closes
(defn echo-server [^linear ch :(Session (Rec self (Branch (Recv int (Send int self)) Close)))] : nil
  (match (offer ch)
    (Left ch)
      (let [[n ch] (recv ch)]
        (let [ch (send ch n)]
          (echo-server ch)))
    (Right ch)
      (close ch)))
```

```sweet-exp
;; Server: repeat (recv int, send int) until client closes
defn echo-server [^linear ch :(Session (Rec self (Branch (Recv int (Send int self)) Close)))] :nil
  match offer(ch)
    (Left ch)
    let [[n ch] recv(ch)]
      let [ch send(ch n)]
        echo-server(ch)
    (Right ch)
    close(ch)
```

See `stdlib/session.tur` for the `echo-server-loop` and `echo-client-call`
helpers that wrap this pattern.

### Timeouts

`recv-timeout` is a non-blocking receive that returns a `Choice` value:

```turmeric
(match (recv-timeout ch 500)  ; 500 ms deadline
  (Left [v ch]) (do (println v) (close ch))
  (Right ch)    (do (println "timed out") (close ch)))
```

```sweet-exp
match recv-timeout(ch 500)  ; 500 ms deadline
  (Left [v ch])
  do
    println(v)
    close(ch)
  (Right ch)
  do
    println("timed out")
    close(ch)
```

### Duality

The duality rule governs how the two ends of a channel relate:

| Protocol (one end) | Dual (other end) |
|--------------------|------------------|
| `Send T P`         | `Recv T dual(P)` |
| `Recv T P`         | `Send T dual(P)` |
| `Choose P Q`       | `Branch dual(P) dual(Q)` |
| `Branch P Q`       | `Choose dual(P) dual(Q)` |
| `Close`            | `Close`          |
| `Rec self P`       | `Rec self dual(P)` |

`make-session` accepts one protocol type and automatically produces the dual
for the second endpoint.

### Effect integration

Session channels and algebraic effects can coexist freely. A function may
perform effects while holding a linear channel, as long as the channel is
consumed exactly once along every code path:

```turmeric
(defeffect Log [msg :cstr] :nil)

(defn logged-send [^linear ch :(Session (Send int Close)) val : int] : int
  (perform (Log "before send"))
  (let [ch (send ch val)]
    (close ch)
    0))
```

```sweet-exp
defeffect Log [msg :cstr] :nil

defn logged-send [^linear ch :(Session (Send int Close)) val :int] :int
  perform(Log("before send"))
  let [ch send(ch val)]
    close(ch)
    0
```

See `tests/fixtures/session-effects/` for a complete example.

---

## Multi-Party Session Types (SS5-SS8)

Multi-party session types generalize binary sessions to N >= 2 participants.
A _global protocol_ specifies all interactions between named roles; the compiler
projects it onto each role's local view.

### defprotocol

Declare a global protocol with `defprotocol`:

```turmeric
(defprotocol Ping [A B]
  (-> A B int)    ; A sends an int to B
  (-> B A int))   ; B replies with an int to A
```

```sweet-exp
defprotocol Ping [A B]
  (-> A B int)    ; A sends an int to B
  (-> B A int)    ; B replies with an int to A
```

The role list `[A B]` names each participant. Each `(-> From To type)` line
is one message transfer.

### make-protocol, send-to, recv-from, close

After declaring a protocol, allocate role endpoints with `make-protocol`:

```turmeric
(let [[ra rb] (make-protocol Ping)]
  ...)
```

```sweet-exp
let [[ra rb] make-protocol(Ping)]
  ...
```

`ra` has type `(Role Ping A)` and `rb` has type `(Role Ping B)`. Each role
endpoint is linear -- it must be consumed exactly once.

**send-to** -- send a message to a named role peer:

```turmeric
(let [ra (send-to ra B 42)]  ; A sends 42 to B; ra advances in protocol
  ...)
```

```sweet-exp
let [ra send-to(ra B 42)]  ; A sends 42 to B; ra advances in protocol
  ...
```

**recv-from** -- receive a message from a named role peer; returns `[value new-ch]`:

```turmeric
(let [[v rb] (recv-from rb A)]  ; B receives from A; v = 42
  ...)
```

```sweet-exp
let [[v rb] recv-from(rb A)]  ; B receives from A; v = 42
  ...
```

**close** -- close the role endpoint after the protocol is complete:

```turmeric
(close ra)
```

```sweet-exp
close(ra)
```

Full two-role ping example:

```turmeric
(defprotocol Ping [A B]
  (-> A B int)
  (-> B A int))

(defn role-a [^linear ch :(Role Ping A)] : nil
  (let [ch (send-to ch B 42)]
    (let [[v ch] (recv-from ch B)]
      (println v)
      (close ch))))

(defn role-b [^linear ch :(Role Ping B)] : nil
  (let [[v ch] (recv-from ch A)]
    (let [ch (send-to ch A v)]
      (close ch))))

(defn main [] : int
  (let [[ra rb] (make-protocol Ping)]
    (let [t (spawn (fn [] (role-b rb)))]
      (role-a ra)
      (join t)))
  0)
```

```sweet-exp
defprotocol Ping [A B]
  (-> A B int)
  (-> B A int)

defn role-a [^linear ch :(Role Ping A)] :nil
  let [ch send-to(ch B 42)]
    let [[v ch] recv-from(ch B)]
      println(v)
      close(ch)

defn role-b [^linear ch :(Role Ping B)] :nil
  let [[v ch] recv-from(ch A)]
    let [ch send-to(ch A v)]
      close(ch)

defn main [] :int
  let [[ra rb] make-protocol(Ping)]
    let [t spawn(fn([] role-b(rb)))]
      role-a(ra)
      join(t)
  0
```

### Three or more roles

`defprotocol` and `make-protocol` support any number of roles (N >= 2). For
a three-role pipeline:

```turmeric
(defprotocol Pipeline [A B C]
  (-> A B int)   ; A sends to B
  (-> B C int))  ; B forwards to C

(defn main [] : int
  (let [[ra rb rc] (make-protocol Pipeline)]
    (let [ta (spawn (fn [] (role-a ra)))]
      (let [tb (spawn (fn [] (role-b rb)))]
        (role-c rc)
        (join ta)
        (join tb))))
  0)
```

```sweet-exp
defprotocol Pipeline [A B C]
  (-> A B int)   ; A sends to B
  (-> B C int)   ; B forwards to C

defn main [] :int
  let [[ra rb rc] make-protocol(Pipeline)]
    let [ta spawn(fn([] role-a(ra)))]
      let [tb spawn(fn([] role-b(rb)))]
        role-c(rc)
        join(ta)
        join(tb)
  0
```

The runtime uses a shared router so all N role endpoints communicate through
a single lock-based message router allocated on the heap.

### Projection algorithm

At compile time the elaborator _projects_ the global protocol onto each
role's local view. Projection removes all interactions that do not involve
the current role:

- A message `(-> From To T)` projects to `Send T rest` for `From`, and
  `Recv T rest` for `To`. For any other role R, it is transparent (role R
  keeps its own remaining protocol).
- Choice branches that a role does not participate in must be _uniform_
  across branches (mergeability condition). If they are not, the compiler
  emits `TUR-E0220`.

The projection check happens in `elab_global.c` when `(make-protocol P)` is
elaborated.

---

## Error Codes

| Code | Meaning |
|------|---------|
| `TUR-E0210` | Session operation on a non-session type |
| `TUR-E0211` | Session channel dropped (linearity violation) |
| `TUR-E0212` | Session operation does not match current protocol state |
| `TUR-E0213` | Protocol mismatch between session endpoints |
| `TUR-E0214` | Channel used after close |
| `TUR-E0215` | Channel used more than once (linearity violation) |
| `TUR-E0216` | Receive-timeout used on a non-timeout protocol |
| `TUR-E0217` | Offer used on a non-branch protocol |
| `TUR-E0218` | Choose used on a non-choice protocol |
| `TUR-E0219` | Type mismatch in session message payload |
| `TUR-E0220` | Global protocol is not projectable (mergeability failure) |
| `TUR-E0221` | Role not declared in the protocol |
| `TUR-E0222` | Role implementation type mismatch |
| `TUR-E0223` | Global protocol not well-formed (undeclared role used) |

---

## Session-Typed Channel Wrappers (stdlib/schan.tur)

The built-in session types above are their own typed-channel runtime. When you
instead want to put a protocol discipline over an ordinary buffered channel --
e.g. a worker-pool request/response, or an RPC pipe -- `stdlib/schan.tur`
provides a thin generic wrapper, `SChan<p>`, that carries a protocol *phantom*
`p` advanced by each operation:

```turmeric
(import schan :refer [SChan SSend SRecv SClose
                      schan-new schan-send schan-recv schan-close
                      schan-cell-new schan-cell-get schan-cell-free])
```

The phantom is built from three type-level tags (the session-type names `Send` /
`Recv` / `Close` are reserved primitive constructors, so the wrapper uses the
`S`-prefixed spellings):

| Tag | Meaning |
|-----|---------|
| `(SSend T R)` | send a `T`, then continue as `R` |
| `(SRecv T R)` | receive a `T`, then continue as `R` |
| `SClose`      | the protocol terminus |

Each operation consumes the channel at one protocol state and returns it at the
next, so the phantom is threaded through the result type:

```turmeric
schan-send  : SChan<SSend T R> -> T    -> SChan<R>
schan-recv  : SChan<SRecv T R> -> cell -> SChan<R>   ;; value written to cell
schan-close : SChan<SClose>            -> nil
```

A round trip of `SSend int (SRecv int SClose)`:

```turmeric
(let [cell (schan-cell-new)
      c0   (:: (schan-new 2) (SChan (SSend int (SRecv int SClose))))
      c1   (schan-send c0 7)        ;; c1 : SChan<SRecv int SClose>
      c2   (schan-recv c1 cell)     ;; c2 : SChan<SClose>
      v    (schan-cell-get cell)]   ;; v  : int  (= 7)
  (schan-close c2)
  (schan-cell-free cell))
```

Because the phantom advances with every step, **skipping or reordering a step is
a compile-time type error**. Calling `schan-close` while the channel is still at
`SChan<SRecv int SClose>` fails with a `TUR-E0001` phantom mismatch:

```
error [TUR-E0001]: function 'schan-close' arg 1:
  expected (type-app SChan SClose),
  got (type-app SChan (type-app (type-app SRecv int) SClose))
```

`SChan` is `:linear`, so the protocol additionally cannot be *replayed* --
each step consumes its handle exactly once.

Runnable examples: `tests/fixtures/schan-roundtrip` (single round trip),
`tests/fixtures/schan-worker-pool` (a request/response served by worker threads
reading from the wrapped channel), and `tests/fixtures/errors/schan-skip-step`
(the phantom-mismatch failure). The wrapper sits on top of the low-level
[`tur/chan`](https://github.com/rjungemann/turmeric/blob/main/stdlib/chan.tur) channels, which keep their untyped surface
for callers that do not want the protocol discipline.

> **Note on `schan-recv`.** The natural signature is
> `SChan<SRecv T R> -> Pair<T SChan<R>>`. That is currently blocked by a
> monomorphizer limitation around parametric aggregates whose element is an
> opaque/phantom type (see
> `docs/archive/history/generic-struct-opaque-element-miscompile.md`). Until it is
> fixed, `schan-recv` returns the *typed* continuation directly and delivers the
> received value through a caller-provided cell -- which keeps the continuation
> protocol fully checked.

---

## Getting More Help

Use `tur explain` to get a detailed description of any error code:

```sh
tur explain TUR-E0212
tur explain TUR-E0220
```

The test fixtures under `tests/fixtures/session-*` and `tests/fixtures/errors/session-*`
provide runnable examples for every feature and failure mode described in this guide.
