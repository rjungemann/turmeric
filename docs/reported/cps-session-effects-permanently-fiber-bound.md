# session-effects / session-mp-effects: permanently fiber-bound (pthread + inline-C session runtime)

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

```turmeric
(defn spawn [f : ptr<void>] : ptr<void>   ```c pthread_create(...); ``` )   ; inline-C
(defn join  [t : ptr<void>] : int         ```c pthread_join(...); ``` )     ; inline-C

(defn exchange [^linear ch :(Session (Send int (Recv int Close))) val : int] : int
  (perform (SessionLog "sending 99"))
  (let [ch (send ch val)]
    (let [[result ch] (recv ch)]
      (perform (SessionLog "received 99"))
      (close ch)
      result)))

(defn main [] : int
  (let [[s r] (make-session (Send int (Recv int Close)))]
    (let [t (spawn (fn [] ...recv/send/close on r...))]
      (let [result (handle (exchange s 99)
                     (SessionLog [msg] k) (do (println msg) (resume k nil)))]
        (println result) (join t))))
  0)
```

## Exact eviction reasons (pinned)

`TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume ...` reports two eff=1 evictions per
fixture (the rest are unrelated eff=0 stdlib map/set-eq machinery):

1. **`exchange` / `role-a`: SIG-REJECT** on the session-typed PARAMETER.
   Instrumenting `fn_sig_ok`: param[0] is `(Session P)` = **TY_SESSION** (TypeKind 43) /
   `(Role G R)` = **TY_ROLE** (TypeKind 55). Neither `is_poly_fn` nor `^borrow`; `sig_slot_ok`
   = 0 (a session/role TypeKind is not a scalar), and it is not a by-value aggregate
   (`slot_box_ty`) nor a carrier handle. So the param fails every admission branch of
   `fn_sig_ok` and the function sig-rejects. (Returns are fine -- `int`.)

2. **`main`: SIG-INLINE-C** (`first_unsupported` = `EX_INLINE_C`) -- PERMANENT. `main`'s
   term carries an inline-C node (the `pthread` `spawn`/`join` and/or the session-channel
   primitives), so the CPS backend can never thread a DK continuation through it. `main`
   HANDLES the effect (`SessionLog` / `MpLog`).

## Why this is COMPOUND (fixing the param alone does nothing)

The two are not independent. `main` is a PERMANENT sig_perm sink (SIG-INLINE-C), and it
handles `SessionLog`/`MpLog`. So that effect is permanently tainted (in `g_perm_lo/hi`).
Effects are dynamically scoped -- a `perform` and its `handle` must run on the SAME machine.
Therefore even if the TY_SESSION/TY_ROLE param were admitted (a bounded signature widening --
these lower to an int64 channel handle, so a carrier-scalar admission like the reverted
opaque-carrier work would pass `fn_sig_ok`), `exchange`/`role-a` would immediately become
**SIG-TAINT**: they perform an effect whose only handler (`main`) is permanently on the
fiber. The fixture would not move.

Root of the permanence: these programs are built on **OS threads + inline-C session
channels**. `spawn` runs the peer role on a second `pthread`; the DK/CPS model is a
single-threaded per-continuation machine. Session channels are inline-C handle ops. This is
a different concurrency substrate from the delimited-continuation DK, and `main` is anchored
to it by inline-C.

## Recommendation

**Do NOT treat these as CPS-backend admission targets.** A session-param signature widening
(TY_SESSION/TY_ROLE carrier admission) is not worth landing on its own -- it moves zero
fixtures (the SIG-INLINE-C main still taints the effect) and adds gated signature surface for
no gain, exactly like the reverted opaque-carrier slice.

They matter for the DELETION step, not the migration step. Options for whoever executes the
fiber-runtime deletion (docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md Sec 4):

- **Scope them out explicitly.** If the pthread+inline-C session runtime is retained as its
  own subsystem (it does not use the delimited-continuation effect machine the deletion
  targets -- it is a separate thread/channel runtime), the `SessionLog`/`MpLog` effect on it
  can keep a minimal fiber-style effect path, or these fixtures move to a `requires.*` bucket
  that documents they exercise the thread/session runtime, not the DK.
- **Rewrite the fixtures** to handle the log effect in a helper that is NOT inline-C-anchored
  (so E3'-style handler-in-helper could apply), leaving only the genuinely-threaded session
  I/O on the fiber. Only worthwhile if the goal is to shrink the fiber's effect surface to
  exactly the thread/channel primitives.
- **Accept them as permanent fiber clients** and record that "delete the fiber effect
  runtime" really means "delete the delimited-continuation effect machine; the pthread/session
  channel runtime is a separate subsystem." This is the most honest framing: these fixtures
  prove the fiber effect runtime cannot be deleted to ZERO without also addressing the
  thread-based session concurrency model, which is out of scope for the CPS lowering.

## Verification notes for anyone who revisits

- Repro the eviction: `TUR_TRACE_EVICT=1 tur emit-c --enable=cps-tramp-resume
  tests/fixtures/session-effects/input.tur 2>&1 >/dev/null | grep eff=1` -> `SIG-REJECT
  exchange`, `SIG-INLINE-C main`.
- Confirm the param TypeKind: instrument `fn_sig_ok` to print `p->type.kind` (43 = TY_SESSION,
  55 = TY_ROLE; see src/compiler/types.h).
- Confirm the compound taint: if you experimentally admit the session param, `exchange`
  reclassifies SIG-REJECT -> SIG-TAINT (not in_s), proving `main`'s permanent handling blocks
  it. Do not land such an admission.
