# session-effects / session-mp-effects: ~~permanently fiber-bound~~ SUPERSEDED (WRONG VERDICT)

**CORRECTION (2026-07-18): this WONT-FIX verdict was wrong and is superseded by**
`docs/upcoming/v2/cps-b8-session-effects-plan.md`.  The conclusion rested on the
premise that a body containing inline-C can never CPS-lower.  That is false: the
session ops (make-session/send/recv/close) are control-free inline-C EXPRESSIONS
and delegate via CT_LETRAW like any other value op.  Adding  to
safe_to_delegate (commit 61242b1) moves  off SIG-INLINE-C; the residual
blockers (session  carrier param, capturing spawn-closure) are ordinary
B4/B6-class admission work, not a permanent wall.  The effect is performed AND
handled on the SAME thread -- nothing crosses .  Original analysis
retained below for the record; its DECISION is rescinded.

---

# session-effects / session-mp-effects: permanently fiber-bound (pthread + inline-C session runtime)

**RESOLVED -- decision recorded (WONT-FIX as a CPS target)**, see
`docs/archive/history/cps-session-effects-permanently-fiber-bound.md`.

**Decision:** accept `session-effects` and `session-mp-effects` as **permanent fiber
clients** and scope them OUT of the CPS/DK admission work.  The report's own recommendation
("Do NOT treat these as CPS-backend admission targets ... Do not land such an admission") is
adopted: no compiler change was made.  The analysis was reproduced and confirmed verbatim on
this branch; a durable "permanent fiber client -- NOT a CPS/DK migration target" note was
added to each fixture's header (so the eviction analysis is not re-derived and nobody
re-attempts the net-zero admission); and the deletion plan
`docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md` (part of this same
cps-runtime-finish-plan line) was corrected -- Sec 4/W5 no longer claims these mains are
"covered by E3", and the decisive gate (2c), the build-time assertion (Sec 6 step 1), and
the "done" definition (Sec 7) now carry the session/thread carve-out.  Measured and recorded
there: the two fixtures' emitted C actually uses `tur_effect_perform` /
`global_effect_handler_chain` / `EffectHandlerFrame` / `tur_handler_dispatch`, so Sec 6
step 3 ("delete the fiber effect runtime C") and the Sec 7 "grep = 0" cannot be reached until
these two are rewritten or bucketed out.  The pthread + inline-C session/channel runtime is a
SEPARATE concurrency subsystem from the delimited-continuation DK effect machine; "delete the
fiber effect runtime" means delete the DK effect machine's fiber path, not this thread/channel
runtime.

Reproduced on this branch (`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume`):
`SIG-REJECT exchange` + `SIG-INLINE-C main` (session-effects); `SIG-REJECT role-a` +
`SIG-INLINE-C main` (session-mp-effects).  Both run correctly on the fiber
(`sending 99` / `received 99` / `99`).

---

**Severity:** low-as-a-CPS-target / medium-for-the-deletion-goal. These two fixtures cannot
move to the CPS/DK backend, and unlike the `Unsafe`-marker non-blockers they perform AND
handle a REAL algebraic effect (`SessionLog` / `MpLog`) entirely on the fiber. So they are a
genuine obstacle to *deleting* the fiber effect runtime, not just an un-migrated fixture.
Correctness today is fine (they run on the fiber).

## The fixtures

`tests/fixtures/session-effects` (single-channel) and `tests/fixtures/session-mp-effects`
(multi-party). Both:

- declare a real effect (`SessionLog` / `MpLog`) and PERFORM it inside a session function,
- run a second party on ANOTHER OS THREAD via `spawn` (raw `pthread_create` inline-C) +
  `join` (`pthread_join` inline-C),
- exchange over a linear session channel (`send`/`recv`/`close` / `send-to`/`recv-from`),
- HANDLE the effect in `main`, around the session exchange.

## Exact eviction reasons (pinned)

`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume ...` reports two eff=1 evictions per
fixture (the rest are unrelated eff=0 stdlib map/set-eq machinery):

1. **`exchange` / `role-a`: SIG-REJECT** on the session-typed PARAMETER.
   param[0] is `(Session P)` = **TY_SESSION** (TypeKind 43) / `(Role G R)` = **TY_ROLE**
   (TypeKind 55). Neither `is_poly_fn` nor `^borrow`; `sig_slot_ok` = 0 (a session/role
   TypeKind is not a scalar), and it is not a by-value aggregate (`slot_box_ty`) nor a
   carrier handle. So the param fails every admission branch of `fn_sig_ok`. (Returns fine.)

2. **`main`: SIG-INLINE-C** (`first_unsupported` = `EX_INLINE_C`) -- PERMANENT. `main`'s
   term carries an inline-C node (the `pthread` `spawn`/`join` and/or the session-channel
   primitives), so the CPS backend can never thread a DK continuation through it. `main`
   HANDLES the effect (`SessionLog` / `MpLog`).

## Why this is COMPOUND (fixing the param alone does nothing)

`main` is a PERMANENT sig_perm sink (SIG-INLINE-C) and handles `SessionLog`/`MpLog`, so that
effect is permanently tainted (`g_perm_lo/hi`). Effects are dynamically scoped -- a `perform`
and its `handle` must run on the SAME machine. So even if the TY_SESSION/TY_ROLE param were
admitted, `exchange`/`role-a` would immediately become **SIG-TAINT** (their only handler,
`main`, is permanently on the fiber). The fixture would not move.

Root of the permanence: these programs are built on **OS threads + inline-C session
channels**. `spawn` runs the peer role on a second `pthread`; the DK/CPS model is a
single-threaded per-continuation machine. This is a different concurrency substrate from the
delimited-continuation DK, and `main` is anchored to it by inline-C.

## Recommendation (adopted)

**Do NOT treat these as CPS-backend admission targets.** A session-param signature widening
moves zero fixtures (the SIG-INLINE-C main still taints the effect) and adds gated signature
surface for no gain. These matter for the DELETION step, not the migration step:

- **Accept them as permanent fiber clients** and record that "delete the fiber effect
  runtime" really means "delete the delimited-continuation effect machine; the
  pthread/session channel runtime is a separate subsystem." (This is the option taken.)

## Verification notes for anyone who revisits

- Repro: `TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume
  tests/fixtures/session-effects/input.tur 2>&1 >/dev/null | grep eff=1` -> `SIG-REJECT
  exchange`, `SIG-INLINE-C main`.
- Param TypeKind: instrument `fn_sig_ok` to print `p->type.kind` (43 = TY_SESSION,
  55 = TY_ROLE).
- Compound taint: if you experimentally admit the session param, `exchange` reclassifies
  SIG-REJECT -> SIG-TAINT (not in_s). Do not land such an admission.
