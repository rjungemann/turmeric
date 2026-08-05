# Session-typed channels under the interpreter (`tur --interpret`) -- Plan

> **Status:** COMPLETE -- Slices A + A.5 + B + C + D landed.  The full session
> surface -- `make-session` / `send` / `recv` / `close` / `offer` /
> `choose-left` / `choose-right` / `recv-timeout` and the multi-party
> `make-protocol` / `send-to` / `recv-from` / role-`close` -- now runs under
> `tur --interpret` via a cooperative fiber rendezvous (`src/turi/eval.c`,
> cooperative session + router runtime; `turi_sched_step` in `src/turi/fiber.c`).
> The carved `session-close/requires.tur-only` marker is deleted, and interpreter
> test variants (`session-send-turi`, `session-recv-turi`,
> `session-calc-rpc-turi`, `session-offer-turi`, `session-offer-right-turi`,
> `session-timeout-ok-turi`, `session-timeout-expired-turi`,
> `session-mp-ping-turi`, `session-mp-three-role-turi`,
> `session-mp-handshake-turi`, all marked `requires.interp-only`) exercise real
> send/recv, offer/choose branch selection, timed receive, and 2-/3-role
> multi-party protocols under `tests/run-turi.sh`.  No session inline-C template
> remains carved under the interpreter.
>
> **Verified 2026-07-19:** all four slices confirmed present in
> `src/turi/eval.c` -- `TuriChan` (:7102), `session_send_on`/`session_recv_on`,
> `eval_session_intercept`, and the `TuriRouter`/`TuriRole` multi-party runtime.
> `tests/fixtures/session-close/` carries no `requires.tur-only` marker, and the
> `session-*-turi` interpreter variants (send/recv/calc-rpc/offer/timeout/mp-*)
> are all present. Slices A + A.5 + B + C + D complete; nothing outstanding.
> Ready to archive.
> **Last Updated:** 2026-07-19
> **Type:** Interpreter / runtime
> **Scope:** Give the tree-walking interpreter a cooperative-fiber session-channel
> runtime so `make-session` / `close` / `send` / `recv` / `offer` / `choose` /
> `recv-timeout` (and eventually multi-party roles) run under `--interpret`,
> starting with the lone `requires.tur-only` fixture `session-close` and building
> out. This is the only remaining interpreter carve-out that is a genuine
> language/runtime feature rather than library internals.
> **Builds on:** the interpreter's existing ucontext fiber scheduler
> (`TuriFiber`, `src/turi/eval.c:7539+`), the same substrate `async`/`await`
> already use.

---

## Motivation

Session types are always-on in the compiler (the `-Xsessions` flag is legacy
text only; dispatch is unconditional at `elab_call.c:1893-1922`, no
`EXPERIMENTS[]` row). The forms elaborate fine under the interpreter -- they only
fail at the inline-C wall (`eval.c:7535`), because every session op lowers to an
inline-C template calling a `tur_session_*` runtime function.

Unlike the GC builtins (which were made to work under `--interpret` by
`memcmp`-matching three captureless one-liners and calling the *already-linked*
`src/runtime/gc.c`), the session runtime is **not a linked object file**: it is
emitted as a C-source preamble string in `emit_module.c:8651-8859`. There is
nothing for the interpreter to call. So this is not a special-case-three-strings
job; it is "write a small cooperative session runtime for the interpreter and
route the templates to it." The good news: the interpreter already has the exact
scheduling substrate needed, and the work slices cleanly, with the carved
fixture (`session-close`) sitting in the trivial first slice.

---

## How the compiled runtime works (what we are mirroring)

`emit_module.c:8651-8859` emits:

- `TurSyncCh { pthread_mutex_t mu; pthread_cond_t cv; int64_t val; int state; }`
  -- a one-slot rendezvous; `state` 0=idle, 1=data-ready, 2=data-acked.
- `TurChannel { TurSyncCh data; TurSyncCh branch; int refcount; int abandoned;
  pthread_mutex_t rc_mu; ... }` -- **both endpoints share the same
  `TurChannel*`**; make-session returns the pointer twice. Duality is
  type-level only; at runtime the two endpoints are the identical object.
- `tur_session_send`/`recv` -- a 3-phase handshake (`send` waits idle, deposits,
  waits for ack; `recv` waits data-ready, reads, acks). Blocks on condvars.
- `tur_session_send_tag`/`recv_tag` -- same handshake on the `branch` slot,
  backing `offer`/`choose-left`/`choose-right`.
- `tur_session_close` -- set `abandoned` (wakes blocked senders), refcount--,
  free at 0.
- `tur_session_recv_timeout` -- `pthread_cond_timedwait`, stashes the value in a
  `_Thread_local tur__rtv_` and returns a 0/1 tag.
- Multi-party: `TurRouter` (N x N `TurSyncCh` grid) + `TurRole {router, idx}`;
  `tur_router_send`/`recv`.

**There is no builtin `fork`/`spawn`.** Every send/recv fixture defines its own
`spawn`/`join` as user inline-C over `pthread_create(..., tur_session_thread_wrapper, f)`,
where the wrapper reads `fat[0]` as a compiled-closure function pointer
(`emit_module.c:8753`). That inline-C cannot run in the interpreter, so the
fixtures-as-written will not port unchanged (see Slice B).

The key adaptation: **the interpreter is single-threaded but cooperative.** A
blocking `recv` becomes "park this `TuriFiber`, record it as the channel's
waiter, `swapcontext` to the scheduler; the matching `send` wakes it." The
`state 0/1/2` handshake discipline carries over verbatim (it also guards the ABA
case the compiled comment at `emit_module.c:8658` calls out) -- just cooperative
instead of mutex/condvar.

---

## Where the templates live (both elaborators)

Session inline-C is built in **two** places; the interpreter must intercept
both:

- `src/compiler/elab_sessions.c` -- `send`, `close`, `offer`, `choose-left/right`,
  `recv-timeout`, role-close. Built via the `session_inline_c` helper
  (`:175-204`), which always sets `n_captures = 0` and stores the channel operand
  as `val_exprs[0]`.
- `src/compiler/elab_forms.c:290-508` -- the `[a b]` destructuring of
  `make-session` and `recv` pairs splits into per-binding inits: the *real*
  `tur_session_new(TUR_DBGPROTO("..."))` (`:376`), `tur_session_recv(__TUR_VAL_0__)`
  (`:473`), the `tur__rtv_` timeout-value read (`:469`), and bare-`__TUR_VAL_0__`
  endpoint aliases. `elab_sessions.c` only emits `"/*make-session*/"` /
  `"/*recv-pair*/"` sentinels for these.

So the eval.c interception is keyed on **code-string prefixes** (all
`n_captures == 0`, `n_val_exprs` 0-2), NOT the GC guard's `n_val_exprs == 0`:

| Match key (prefix) | n_val_exprs | Action |
| --- | --- | --- |
| `tur_session_new(` | 0 | create interpreter channel handle (refcount 2, both endpoints alias) |
| bare `__TUR_VAL_0__` (Session-typed) | 1 | endpoint alias -- eval + return the channel handle |
| `__extension__ ({ tur_session_send(` | 2 | eval chan+val; cooperative send; return chan |
| `tur_session_recv(__TUR_VAL_0__)` | 1 | eval chan; cooperative recv; return value |
| `tur__rtv_` | 0 | return stashed timeout value |
| `tur_session_close(__TUR_VAL_0__)` | 1 | eval chan; close (refcount--, wake) |
| `tur_role_close((void *)` | 1 | eval role; role close |
| `tur_session_recv_tag(__TUR_VAL_0__)` | 1 | eval chan; recv branch tag |
| `__extension__ ({ tur_session_send_tag(...0)` / `...1)` | 1 | eval chan; send tag 0/1; return chan |
| `tur_session_recv_timeout(` | 2 | eval chan+dur; timed recv; return 0/1, stash value |
| `tur_router_recv(` / `tur_router_send(` | 0-1 | multi-party (Slice D) |

Add a `session_inline_c_intercept(env, frame, ic)` helper called from the
`EX_INLINE_C` case (`eval.c:7509`) right after the GC block: eval each
`val_exprs[i]` (propagating errors/signals like the surrounding code), match the
prefix, dispatch to the interpreter session runtime, return.

---

## The interpreter session runtime (new, small)

Introduce a heap channel object (either a new `TuriValue` variant or a
tracked heap struct smuggled through the `void*`/`int64` slot the type layer
already lowers `Session`/pair/offer to -- `types.c:2754-2759`):

```c
typedef struct TuriChan {
    int64_t  data_val;   int data_state;   // 0/1/2, mirrors TurSyncCh
    int64_t  branch_val; int branch_state; // offer/choose slot
    TuriFiber *send_waiter;   // parked sender, or NULL
    TuriFiber *recv_waiter;   // parked receiver, or NULL
    int refcount;             // 2 at make-session
    int abandoned;
} TuriChan;
```

`send`/`recv` park/wake the counterpart fiber via the existing scheduler
(`swapcontext` back to the driver; the waking op marks the parked fiber
`READY`). Reuse the `EX_ASYNC` fiber-spawn path (`eval.c:7542`) for the peer.
Track the channel for reclaim in `turi_env_free` (mirror the async-fiber-stack
tracking already there). `recv-timeout` needs a scheduler timer + an env-slot
analog of `tur__rtv_`.

---

## Fixture inventory (drives the slices)

- **Slice A -- Close only (no rendezvous, no peer):** `session-close` (**the lone
  `requires.tur-only`**; skipped at `run-turi.sh:260`). Type-check-only fixtures
  with no runtime channel op (`session-global-basic`, `session-project-loop`)
  already elaborate and effectively pass; verify they run.
- **Slice B -- send/recv + peer:** `session-send`, `session-recv`,
  `session-calc-rpc`, `session-delegation`, `session-delegated-rpc`,
  `session-effects`, `session-stm`, `session-project-basic`,
  `defstruct-field-session`, `defstruct-field-session-project`.
- **Slice C -- offer/choose (branch slot):** `session-offer`,
  `session-choose-left`, `session-choose-right`, `session-rec`,
  `session-echo-rpc`, `session-project-choice`. Plus **C'** recv-timeout:
  `session-timeout-ok`, `session-timeout-expired`.
- **Slice D -- multi-party router:** `session-mp-ping`, `session-mp-calc`,
  `session-mp-effects`, `session-mp-handshake`, `session-mp-three-role`,
  `session-mp-delegated`, `defstruct-field-session-role`.

Every fixture except Slice A spawns a real `pthread` peer via user inline-C, so
Slices B-D need an interpreter-runnable peer (see below) -- the fixtures will
need an interpreter-friendly `spawn` or test variants.

---

## Phases

### Slice A -- make-session Close + close (unblocks the carved fixture) -- DONE

Intercept `tur_session_new(` (produce a `TuriChan`, refcount 2, both endpoints
alias) and `tur_session_close(` / `tur_role_close(` (refcount--, free at 0). No
scheduler, no rendezvous. **Delete `tests/fixtures/session-close/requires.tur-only`.**
Smallest step; note it still requires the heap channel object and interception of
the `elab_forms.c`-synthesized `tur_session_new(TUR_DBGPROTO(...))`, not a bare
one-liner -- so it is meaningfully more than the GC special-case, but self-contained.

### Slice A.5 -- interpreter `spawn` (peer as a fiber) -- DONE (via `async`/`await`)

The interpreter already has a closure-to-fiber spawn: `(async (fn [] ...))`
builds a `TuriFiber` (the `EX_ASYNC` path) and `(await t)` joins it. Rather than
introduce a second `spawn`/`join` surface, the shipped interpreter test variants
drive the peer with `async`/`await` -- the "ship interpreter test variants"
option. The compiled fixtures' `pthread_create` inline-C (which dereferences the
compiled fat-closure ABI) still cannot run under `--interpret`, so those keep
running compiled under `tests/run.sh`; the `-turi` variants own the interpreter
path. A dedicated interpreter-native `spawn`/`join` under the fixtures' own names
remains possible future work but was not needed to un-carve Slice B.

### Slice B -- cooperative send/recv -- DONE

Implemented as `TuriChan` (a heap struct smuggled through a `TURI_INT` pointer,
bump-allocated from the env value pool so it is leak-clean at teardown) plus the
`session_send` / `session_recv` / `session_close` park/wake protocol in
`src/turi/eval.c`, dispatched by `eval_session_intercept` from the `EX_INLINE_C`
case. Blocking uses `session_park_or_spin`: a fiber suspends via `swapcontext`
(recorded as the channel's `send_waiter`/`recv_waiter` so its counterpart can
`turi_sched_enqueue` it); the main context pumps `turi_sched_step`. The compiled
`state 0/1/2` handshake is preserved verbatim -- send returns only after the
receiver acks (state 2), and the slot's return-to-idle (state 0) wakes a peer
parked in the next step's send, so recurring RPC channels that flip
sender/receiver roles across steps rendezvous correctly.

Original Slice B design note follows:

Implement the `TuriChan` park/wake mailbox and intercept
`tur_session_send(` / `tur_session_recv(` (and the endpoint-alias
`__TUR_VAL_0__` / pair-split forms). Preserve the `state 0/1/2` "send returns
only after receiver acks" discipline so a fiber cannot race itself. This is the
bulk of the work -- it introduces the channel value type and the park/resume
protocol. Un-carve Slice B fixtures (via interpreter `spawn`).

### Slice C -- offer/choose + recv-timeout -- DONE

Implemented. The data-slot send/recv was generalized into `session_send_on` /
`session_recv_on` (parameterized by a `state`/`slot` pair), so the `branch` slot
reuses the same park/wake handshake: `session_send_tag` / `session_recv_tag`
back `choose-left` / `choose-right` (tag 0/1) and `offer`. `eval_session_intercept`
routes `tur_session_recv_tag(` / `tur_session_send_tag(...0/1)` /
`tur_session_recv_timeout(` / the `tur__rtv_` read. The `offer` / `recv-timeout`
`EX_MATCH` gets a `TY_SESSION_OFFER` branch in `eval_match_resolve` that reads
the tag, re-evaluates the scrutinee inline-C's `val_exprs[0]` for the channel
(a pure endpoint read), picks the Left (0) / Right (1) arm, and binds the channel
to the arm variable -- mirroring the compiled `emit_expr.c` TY_SESSION_OFFER
match. `recv-timeout` does a bounded main-context timed wait (pump
`turi_sched_step`, re-check the deadline) and stashes the received value in
`env->session_rtv` -- the interpreter analog of the compiled `tur__rtv_`
thread-local, read by the recv-pair split. Also fixed `session_send` to drop
silently on an abandoned channel (return the endpoint) exactly as the compiled
`tur_session_send` does, instead of erroring -- this is what lets a timed-out
receiver close its end while a late sender's deposit is discarded.

Original Slice C design note follows:

Add the `branch` slot handshake; intercept `tur_session_recv_tag(` /
`tur_session_send_tag(...0/1)`. Wire the `offer` `EX_MATCH` on the returned tag
(compiled path at `emit_expr.c:7663`; interpreter evaluates the tag and picks the
arm). `recv-timeout` adds a scheduler timer + `tur__rtv_` env-slot analog.

### Slice D -- multi-party roles -- DONE

Implemented as `TuriRouter` (an N x N grid of `TuriChan` cells; `slots[i*N+j]`
carries role i -> role j) and `TuriRole { router, role_idx }`, both pool-allocated
so they are reclaimed at env teardown.  Router send/recv reuse Slice B's data-slot
park/wake verbatim -- `router_send` / `router_recv` just address the right cell
and call `session_send_on` / `session_recv_on`.  `eval_session_intercept` routes
`tur_make_roles(` / `tur_get_role(` / `tur_router_send(` / `tur_router_recv(` /
`tur_role_close(`; the role/peer indices baked into each template are parsed out
with `session_int_after` (the same non-NUL-terminated-slice-safe scan used for
the choose tag).  `role-close` drops the router refcount.  Verified against
2-role bidirectional (ping), 3-role pipeline (three-role), and a 3-message 2-role
handshake, each with the peer role(s) as async fibers.

Original Slice D design note follows:

Interpreter `TurRouter` analog (N x N `TuriChan` grid) + `TurRole`; intercept
`tur_make_roles` / `tur_get_role` / `tur_router_send` / `tur_router_recv` /
`tur_role_close`. Largest slice, only needed for `session-mp-*`. Reuses Slice B's
park/wake.

---

## Missing language features

**None at the language level** -- the forms already elaborate. What is missing is
**interpreter runtime support**: a cooperative channel object, a park/resume
protocol on the existing fiber scheduler, and an interpreter `spawn`. All three
build on machinery the interpreter already has (`TuriFiber`, `swapcontext`,
`EX_ASYNC`, env-teardown tracking). The single conceptual constraint is
preserving the compiled 3-state handshake semantics cooperatively.

---

## Recommended stopping point

**Slice A + A.5 + B** gives real session communication under the interpreter and
un-carves the bulk of the fixtures; it is the natural v1 target. **Slice A alone**
un-carves the one `requires.tur-only` fixture for minimal effort if only the
carve-census matters. **Slices C/D** are worthwhile-but-optional follow-ups --
sequence them by whether interpreter/WASM parity for offer/choose and multi-party
sessions is actually needed downstream. Do not start D before B is solid.

---

## Validation / definition of done

- Per slice: the slice's fixtures run and pass under `tur --interpret` (with
  interpreter `spawn`/test variants where the compiled fixtures used pthreads).
- `bash tests/run-turi.sh` green with the un-carved fixtures now RUN;
  `session-close`'s `requires.tur-only` deleted at Slice A.
- `bash tests/run.sh` unchanged (interception is additive to the interpreter;
  no codegen touched).
- `check_turi_parity.py` 0-gap.
- Cooperative correctness: a send blocks until its recv acks; close wakes a
  blocked sender; no fiber deadlocks itself (the ABA guard holds).

---

## See Also

- `src/compiler/elab_sessions.c` (templates), `elab_forms.c:290-508` (pair/recv
  destructuring emit), `emit_module.c:8651-8859` (the compiled runtime being
  mirrored), `elab_call.c:1893-1922` (always-on dispatch).
- `src/turi/eval.c:7509-7537` (GC special-case + inline-C wall -- the interception
  site) and `:7539+` (the fiber scheduler to build on).
- `tests/fixtures/session-close/` (Slice A target), `session-*` /
  `session-mp-*` (later slices).
- `docs/archive/history/turi-interpret-flip-residual-plan.md` (R1 note
  reclassifying `session-close` out of the stdlib-native campaign as
  runtime-level support -- this plan is that support).
