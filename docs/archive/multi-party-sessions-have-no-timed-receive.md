# Multi-party sessions have no timed receive

> **RESOLVED 2026-09-28.** All four steps of the fix direction landed:
>
> 1. **Syntax.** `(timeout (-> From To T) [ok ...] [expired ...])` in a
>    `defprotocol`. The deadline stays at the op, as in the binary
>    `recv-timeout`; the branches are labelled (either order, either may be
>    empty) and the forms after the `timeout` follow both. The parser appends
>    that rest to each branch body, so a role cursor walks into a branch and
>    straight on with no continuation stack (`GI_TIMEOUT`,
>    `src/compiler/elab_internal.h`).
> 2. **Projection** (`src/compiler/project.c`) -- the design risk, settled as
>    the report predicted: only the receiver observes the outcome, so every
>    OTHER role, the sender included, must project the two branches
>    identically, else `TUR-E0220`. The receiver gets `Recv T (Timeout ok
>    expired)`; the sender `Send T (Timeout P P)` -- kept wrapped so two-role
>    projections are the binary duals `make-session` builds (pinned by
>    `session-project-timeout`, and swapping the roles is a `TUR-E0001`); a
>    bystander gets `P`. `proto_equal` had no `TY_TIMEOUT` arm and called any
>    two Timeouts equal; it recurses now. A protocol containing a `timeout` is
>    projected onto every role at `defprotocol`, since the role endpoints --
>    which walk the global tree rather than a projection -- lean on the rule:
>    a bystander or the sender steps into `ok` knowing `expired` is the same.
> 3. **`recv-timeout-from`** (`elab_global.c`) returns the same
>    `TY_SESSION_OFFER` shape as `recv-timeout` and stashes the value in
>    `tur__rtv_`, so the `Left` arm's `[v ch]` destructuring and the offer
>    match are the binary code unchanged. `send-to` into, and a plain
>    `recv-from` at, a timed step are accepted (the latter is the `ok`
>    outcome). `close`'s bystander skip now shares the ops' helper.
> 4. **Runtime.** `tur_router_recv_timeout` is the binary timed wait on a
>    router slot. The part the report did not anticipate is the SENDER: once
>    the receiver has given up, the message it gave up on must neither block
>    the sender forever (the router has no `abandoned` flag) nor be delivered
>    to the receiver's NEXT receive from that peer -- which is what a
>    non-trivial `expired` branch would otherwise get. An expiry leaves the
>    slot a `skip`, taken under the slot lock, and the next send on the slot
>    consumes it and returns. The interpreter's router cell does the same,
>    over the already-fixed fiber-aware `session_recv_timeout`.
>
> Pinned by `tests/fixtures/session-mp-timeout` (relay expired and ok, and a
> retry that must print the SECOND value) and its `--interpret` twin
> `session-mp-timeout-turi`, plus four negative fixtures (bystander and
> sender non-uniform `TUR-E0220`, `recv-timeout-from` on an untimed step
> `TUR-E0212`, a bad branch label `TUR-E0223`). The guide's Timeouts section
> points at the new "Timed receives" section instead of saying binary-only.

**Severity: low-medium.** Feature asymmetry, not a defect -- nothing gives a
wrong answer. Binary sessions have `recv-timeout`; multi-party role endpoints
have no equivalent, so a role blocked in `recv-from` has no bounded-wait option
and no way to recover from a peer that never sends.

## What exists

| Binary session | Multi-party role |
| --- | --- |
| `send` | `send-to` |
| `recv` | `recv-from` |
| `close` | `close` |
| `offer` / `choose-left` / `choose-right` | `choice` in the global type, projected |
| **`recv-timeout`** | **nothing** |

```
$ grep -n 'sym_recv_timeout\|sym_recv_from' src/compiler/elab_core.c
2315:    e->sym_recv_timeout = intern_cstr(st, "recv-timeout");
2322:    e->sym_recv_from     = intern_cstr(st, "recv-from");
```

There is one timeout op and it is Session-only: `elab_session_recv_timeout`
(`src/compiler/elab_sessions.c:612`) resolves its operand through
`session_protocol_of`, which requires a `TY_SESSION`. A `Role` endpoint cannot
reach it, and no `recv-timeout-from` (or any spelling of it) is interned.

The type-level machinery is not the blocker: `Timeout` is a real protocol
constructor with a dual rule (`type_timeout`, `elab_sessions.c:156`), and the
multi-party projection already lowers a global `choice` into per-role
`Choose`/`Branch`. What is missing is the global-type syntax to express a timed
interaction and the projection rule for it.

## Why it matters

A timeout is the only recovery an N-party protocol has against a participant
that stalls. Binary sessions get one; multi-party -- where there are more
participants to stall, and the router makes any one of them able to block a
peer -- gets none. The asymmetry is also invisible: nothing in
[session-types-guide.md](../guides/session-types-guide.md) says multi-party has
no timeout, and a reader who has just read the Timeouts section reasonably
assumes it carries over.

The interpreter dimension is worth stating too. Under `--interpret` a stalled
role is *detected* (`eval: session recv deadlocked`, exit 1) rather than hanging
-- see [turi-session-expansion-plan.md](turi-session-expansion-plan.md)
phase S6 -- so the missing timeout hurts most exactly where it cannot be worked
around: a compiled multi-party program, which hangs until killed.

## Fix direction

Not a small change, and worth scoping before starting:

1. Global-type syntax for a timed interaction, e.g.
   `(-> A B int :timeout 500)` or a `(timeout ms ...)` wrapper.
2. A projection rule: for the receiving role it becomes the existing
   `Timeout` local type; for every other role it must be transparent, and the
   mergeability condition (`TUR-E0220`) has to account for the branch the
   timeout introduces -- a timed receive is a two-outcome branch, so the roles
   not party to it must be uniform across both outcomes, exactly as with
   `choice`.
3. A `recv-timeout-from` surface op reusing the existing `Timeout` elaboration.
4. Runtime: `tur_router_recv` needs the `pthread_cond_timedwait` treatment
   `tur_session_recv_timeout` already has; the interpreter's `router_recv` needs
   the timer work described in
   [turi-fiber-recv-timeout-ignores-its-deadline](turi-fiber-recv-timeout-ignores-its-deadline.md)
   (fixed and archived; fix that one first -- a fiber-context timed receive is broken for binary
   sessions today, and multi-party would inherit the same hole).

Step 2 is where the design risk is; 1, 3 and 4 are mechanical once it is settled.

**A reasonable interim:** document the asymmetry in the guide's Timeouts section
rather than leaving a reader to infer it. That is a one-paragraph change and
removes the surprise even if the feature waits.

## See also

- `src/compiler/elab_sessions.c:601-676` -- `elab_session_recv_timeout`, the
  binary implementation to generalize.
- `src/compiler/elab_global.c` -- projection, where the new rule lands.
- `src/compiler/elab_core.c:2315,2322` -- the interned op names.
