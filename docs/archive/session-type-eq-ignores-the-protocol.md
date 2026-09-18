# `type_eq` ignores the session protocol, so any `Session[P]` matches any `Session[Q]`

**RESOLVED 2026-09-18.** Fixed, and **the filed root cause was only half of
it** -- worth recording, because fixing only the half named below would have
changed nothing observable.

`type_eq` really did lack a `TY_SESSION` case and really did fall through to
`return 1`, and that is now fixed (`src/compiler/types.c`, `sess_proto_eq`).
But adding it alone left both repros compiling. The *operative* gate is the
saturated positional argument check at `src/compiler/elab_call.c:6487`, which
consulted `type_eq` **only for `TY_STRUCT`/`TY_ADT` parameters** and matched
everything else on `TypeKind` alone -- and every session endpoint has the same
kind, `TY_SESSION`. So the protocol was never compared no matter what
`type_eq` did. Both changes are needed together: the gate has to ask, and
`type_eq` has to be able to answer.

Two further notes on the fix:

- **Equirecursion was the real difficulty, not termination.** The report
  guessed a coinductive comparison would be needed "because `TY_SESSION_REC`
  is recursive". Termination is actually free: a back-reference is a sentinel
  (`TY_SESSION_REC` with `fst == NULL`) and a binder's body holds that
  sentinel rather than a pointer to itself, so the graph is a finite DAG. The
  assumption set is needed for a different reason -- `session_protocol_of`
  unfolds `Rec` at every use site, so a declared parameter can hold the folded
  `Rec[X, P]` while the argument holds its unfolding, and those must still
  compare equal. The failure mode to avoid was a false NEGATIVE that rejects
  working code, not non-termination.
- **The fix caught two genuinely mis-wired fixtures.**
  `tests/fixtures/session-rec` and `tests/fixtures/session-echo-rpc` both bound
  `[[s r] (make-session P)]` and then passed `r` to the function declaring `P`
  and `s` to the one declaring `dual(P)` -- the two endpoints swapped. They
  compiled only because of this bug, and they *ran* correctly (the runtime
  pairs two queues, and the two operation sequences were complementary), so
  nothing else was ever going to catch it. Both are a one-token fix (`[[r s]`),
  and both still produce their expected output.

Pinned by `tests/fixtures/errors/session-protocol-mismatch-at-call` and
`tests/fixtures/errors/session-endpoints-swapped`. `session_dual`
(`elab_sessions.c:88`) was checked for the same gap and does not have it -- it
handles every protocol kind including the back-reference sentinel.

Still permissive, deliberately: the three internal pair kinds
(`TY_SESSION_PAIR`, `TY_SESSION_RECV_PAIR`, `TY_SESSION_OFFER`) are transient
elaborator products always destructured on the spot, never a type a user writes
on a parameter, so they are not part of the boundary this fixes. The
partial-application path (`elab_call.c:4862`) has the same STRUCT/ADT-only
gate; it was left alone rather than changed without a repro.

---

**Summary:** `type_eq` has no case for `TY_SESSION` (or for any session
protocol kind), so it falls through to its trailing `return 1` and **two
session types with completely different protocols compare equal**. Handing a
function an endpoint whose protocol is not the one it declares is accepted
silently, which defeats the guarantee session types exist to provide.

**Severity:** Medium-high. A wrong program is accepted with no diagnostic. The
in-body checks still work (performing `send` where the protocol says `recv` is
a TUR-E0212), so the feature is not useless -- but the *boundary* between two
functions, which is exactly where a protocol mismatch would otherwise show up
as a hung coroutine at runtime, is unchecked.

## Minimal repro

```turmeric
;; Expects an endpoint whose protocol is ALREADY complete.
(defn takes-close [^linear ch : (Session Close)] : nil
  (close ch))

(defn main [] : int
  ;; `a` is Session[Send int Close] -- NOT Session[Close]. Passing it to
  ;; takes-close is the only questionable thing in this program: `b` is
  ;; driven correctly (recv, then close).
  (let [[a b] (make-session (Send int Close))]
    (takes-close a)
    (let [[n b1] (recv b)]
      (close b1)
      n)))
```

```sh
$ tur check repro.tur
$ echo $?
0
```

No diagnostic. `takes-close` will `close` a channel that still owes a `send`.

A second shape, closer to how the guides present sessions -- swap the two ends
of one `make-session` between a client and a server that declare dual
protocols, and it is still accepted:

```turmeric
(defn client [^linear ch : (Session (Send int (Recv int Close)))] : int ...)
(defn server [^linear ch : (Session (Recv int (Send int Close)))] : nil ...)

(let [[a b] (make-session (Send int (Recv int Close)))]
  (server a)     ; WRONG END -- a runs the client's protocol
  (client b))    ; also wrong; accepted
```

Control: `tur check` does report real errors on these files (a
`(defn main [] : int "hello")` in the same position is a TUR-E0709), so the
silence is specific to the session comparison, not a file that was skipped.

## Root cause

`src/compiler/types.c:106` -- `type_eq` spans lines 106-346 and mentions
**none** of `TY_SESSION`, `TY_SEND`, `TY_RECV`, `TY_CLOSE`, `TY_CHOOSE`,
`TY_BRANCH`, `TY_SESSION_REC` or `TY_TIMEOUT`. After the `a.kind != b.kind`
guard, two `TY_SESSION` values reach the function's trailing `return 1` with
their `as.session_.fst` protocol payloads never compared.

This is the same defect class the neighbouring `TY_TYPEROW` case was added to
prevent, and its comment already names the hazard
(`src/compiler/types.c:316`):

> Without this explicit case a row would fall through to `return 1` and any
> two rows would compare equal -- a silent miscompile.

Session types are the remaining kind in that position.

## Fix directions

Add an explicit `TY_SESSION` case that compares the protocol structurally, and
cases for the protocol kinds themselves (`TY_SEND`/`TY_RECV` compare payload
type *and* continuation; `TY_CLOSE` is nullary; `TY_CHOOSE`/`TY_BRANCH`
compare both arms; `TY_TIMEOUT` compares both outcomes).

**The one trap:** `TY_SESSION_REC` is recursive, so naive structural recursion
will not terminate on `(Rec self (Recv int (Send int self)))`. The existing
printer already unrolls to a bounded depth and prints a `?` at the cut
(`Rec[self, Recv[int, Send[int, Rec[self, ?]]]]` appears in TUR-E0212 text), so
there is precedent for a depth bound -- but equality wants a proper
coinductive comparison (an assumed-equal set of `(a, b)` pairs, standard for
recursive types) rather than a depth cut, or two protocols that differ only
past the cut would compare equal and reintroduce this bug in miniature.

Worth checking whether `dual()` has the same gap, since it walks the same
kinds.

`tests/fixtures/errors/` wants both repro shapes pinned -- the mismatched
declared protocol and the swapped `make-session` ends.

## How it was found

Cleaning up the session example in
`docs/guides/advanced-type-system-rationale.md`, which used lowercase
`recv`/`send` and so type-checked only vacuously. Probing what *is* checked
turned up this hole: the guide's claim in the same section that duality
checking "catches protocol mismatches at compile time" holds for the operation
order inside a body, but not across a call boundary.

Guide upkeep: if this is fixed, the "Why these features fit" paragraph in
`docs/guides/advanced-type-system-rationale.md` becomes accurate as written
and needs no change.
