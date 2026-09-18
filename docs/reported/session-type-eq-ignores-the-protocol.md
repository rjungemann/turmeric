# `type_eq` ignores the session protocol, so any `Session[P]` matches any `Session[Q]`

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
