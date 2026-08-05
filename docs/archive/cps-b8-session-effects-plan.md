# B8 -- session-effects / session-mp-effects DK lowering plan

**STATUS: COMPLETE (DONE 2026-07-18; confirmed 2026-07-19).** All four B8 fixtures
emit `tur_effect_perform` = 0 under `--enable=cps-tramp-resume`; both session
fixtures run correctly on both paths, ASan shows only the 104-byte DK-node
baseline leak that cps-backend-option-effect/struct-effect already ship with,
suite 2203/0. Slices: 1 (inline-C expr delegation, 61242b1), 2 (session void*
carrier param, 9f19f35), 3 (nil inline-C side effect + capturing spawn-closure
delegation, 2ed823e; TY_ROLE carrier, 686abfc).  A full sweep of 1792 fixtures
finds ZERO fiber-live `tur_effect_perform` call sites.

> **CONFIRMED DONE (2026-07-19).** `session-effects`, `session-mp-effects`,
> `fiber-effect`, `p19-8-fiber-effect-chain` all still present and DK-lower. The
> `cps-tramp-resume` experiment has GRADUATED (always-on) and the fiber effect
> runtime has been deleted (Stage G). The "permanent carve-out" this plan
> superseded never materialized. Nothing remains open -- ready to archive.

---


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

## Slice 2 status (LANDED, commit 9f19f35)

`type_is_session` + `carrier_handle_ok`/`fn_carrier_param_ok` admit the void*
session param. `exchange` clears SIG-REJECT; suite 2203/0, flag-off byte-identical.

## Slice 3 -- capturing closure passed to `spawn` (`main` EX_CLOSURE)

The spawn thunk captures the session channel `r`, performs no effect, and is
handed straight to the inline-C pthread wrapper. It only needs to be a
delegatable VALUE.

**Empirically validated (probe, NOT landed).** Admitting a general capturing
closure as a delegatable value under the flag makes `session-effects` DK-lower
END-TO-END: `tur_effect_perform` = 0, runs correctly (`sending 99` / `received
99` / `99`), suite 2203/0, and ASan shows **no use-after-free** -- only a leaked
fat-closure env (the documented Phase-1-keystone / Phase-3 gap: the CPS
delegation path does not apply the direct emitter's scoped-env free). So the
lowering is CORRECT; the only work is the env free.

**Attempted (2026-07-18) -- the leak is NOT the closure env; it is the `^linear`
session channel's scope-exit drop, which the CPS delegation path does not apply.**
Wiring an `EX_CLOSURE` + `reap_env` delegation into `cps_bind` (freed at the
outermost DK entry boundary) makes `session-effects` DK-lower and run CORRECTLY
(`sending 99` / `received 99` / `99`, perform=0, plain run exit 0), but ASan still
shows a **264-byte leak from `tur_session_new` in `main__cps`** -- the session
CHANNEL itself. Flag-off, the direct emitter applies the `^linear` value's
scope-exit drop (`close`/free); on the CPS path the delegated `make-session`
inline-C runs but the linear drop is never inserted, so the channel leaks. This is
the Phase-3 owning/linear-value teardown gap (E3/E4 in
`cps-backend-multishot-continuations-owning-capture-plan.md`, explicitly OPEN),
NOT a closure-env issue -- `reap_env` cannot touch it. `session-mp-effects`
additionally does not reach perform=0 with the closure change alone (its
role-a/role-b structure needs more). The probe was reverted (a leaky, partial
change against the leak discipline is not a landing).

**The real slice 3** is therefore the Phase-3 work: apply the `^linear`
session-channel (and general owning-value) scope-exit drop on the CPS delegation
path, so a DK-lowered `make-session`/`recv` value is freed exactly as the direct
emitter frees it. That closes the 264-byte channel leak; the closure-env
`reap_env` piece rides along. Until then, the session fixtures are FIXABLE (the
lowering is proven correct) but not leak-clean, so they should not be flipped on
in the suite. This is the last genuine piece before `grep tur_effect_perform == 0`.

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
