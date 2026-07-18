# B8 -- session-effects / session-mp-effects DK lowering plan

**Supersedes** the WONT-FIX verdict in
`docs/archive/cps-session-effects-permanently-fiber-bound.md`, which was **wrong**:
it concluded these fixtures are "permanent fiber clients" from the premise that a
body containing inline-C can never CPS-lower. That premise is false -- a
control-free inline-C EXPRESSION delegates via `CT_LETRAW` like any other
direct-emitted value op.

## What was actually blocking (measured, not assumed)

The `SessionLog`/`MpLog` effect is performed by `exchange`/`role-*` and handled by
`main` -- **all on the same thread**. The spawned pthread runs only effect-free
`recv`/`send`/`close`. Nothing threads a continuation across `pthread_create`; the
"effect crosses the thread boundary" framing was incorrect.

The real roots, from `TUR_TRACE_EVICT` + `first_unsupported`:

1. **`main` SIG-INLINE-C.** `make-session`/`send`/`recv`/`close` elaborate to
   `EX_INLINE_C` nodes (elab_sessions.c -- synchronous `tur_session_*` calls), and
   `safe_to_delegate` had no `EX_INLINE_C` case, so the whole body evicted.
2. **`exchange` SIG-REJECT.** Its `^linear ch : (Session ...)` param is a `void*`
   session-channel carrier that no signature-admission predicate accepts.
3. **`main` residual `EX_CLOSURE`.** The `(fn [] (recv r) (send r v) (close r))`
   closure passed to `spawn` CAPTURES `r`; a capturing closure is not a
   delegatable value.

## Slice 1 -- inline-C expression delegation (LANDED, commit 61242b1)

`case EX_INLINE_C: return g_opt_cps_tramp_resume;` in `safe_to_delegate`. A
control-free inline-C expression now delegates via `CT_LETRAW`. `main` moved off
SIG-INLINE-C; suite 2203/0, flag-off byte-identical. This is the keystone that
disproves "permanent."

## Slice 2 -- session-channel carrier param (`exchange` SIG-REJECT)

A `TY_SESSION` (and `TY_SESSION_REC/PAIR/RECV_PAIR/OFFER`) param is a `void*`
opaque channel handle -- a pointer carrier, exactly like `carrier_handle_ok`'s
heap-ADT handles, which already cross the DK slot by a plain cast. Extend the
param admission (`fn_sig_ok` param loop / a `fn_session_carrier_param_ok`
mirroring `fn_carrier_param_ok`) to accept a session-typed param under the flag;
`emit_params` already spells it `void*`, matching the direct emitter, so the
`__cps` ABI stays consistent.

**Soundness note (must verify):** the channel is `^linear` (used exactly once).
Crossing it into a `perform` continuation is sound iff the resume is single-shot
(the `SessionLog` handler resumes once). A multi-shot handler would double-use the
linear channel -- reject a session capture in a multi-shot continuation
(`g_cap_single_shot` gate already distinguishes these), so linearity is preserved.

## Slice 3 -- capturing closure passed to `spawn` (`main` EX_CLOSURE)

The spawn thunk captures the session channel `r`. It performs no effect, so it
only needs to be a delegatable VALUE. Two options:
- (a) admit a capturing closure as a delegatable fat-closure value in the CPS
  body (emit the `{thunk, env}` box, pass it to the `spawn` inline-C call), or
- (b) recognize the specific `(spawn <closure>)` shape and delegate the whole
  `spawn` call (inline-C + its closure arg) as one `CT_LETRAW`, since `spawn` is
  itself an inline-C sink that runs the closure on another thread and the closure
  is effect-free.

Option (b) is smaller and matches how the fixture uses it (the closure never
escapes into the DK; it is handed straight to an inline-C pthread wrapper).

## Verification

`effect-capture-k`-style: build + run both flag-on and flag-off (`sending 99` /
`received 99` / `99`), ASan-clean, `tur_effect_perform` count 0 under the flag,
suite green, flag-off byte-identical. The pthread + session/channel RUNTIME
(`tur_session_*`, `pthread_*`) stays -- it is a legitimate concurrency subsystem;
what leaves is the fiber EFFECT machine (`tur_effect_perform` /
`global_effect_handler_chain`) for these two programs.

## Endgame consequence

With slices 2-3 landed, the four B8 fixtures all emit `tur_effect_perform` = 0
(fiber-effect/p19-8 already do), so the fiber effect runtime's kill criterion --
a literal `grep tur_effect_perform == 0` across all fixtures -- is reachable. The
sole-effect-lowering plan's session/thread carve-out is removed accordingly (the
carve-out was a consequence of the wrong "permanent" verdict).
