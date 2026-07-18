---
title: "v2 -- CPS/DK endgame: the remaining fiber-live surface (measured)"
status: open -- tactical companion to cps-dk-sole-effect-lowering-plan.md
severity: existential (this is the concrete work-list for deleting the fiber effect runtime)
measured: 2026-07-18
---

# CPS/DK endgame -- the remaining fiber-live surface

Tactical companion to `cps-dk-sole-effect-lowering-plan.md` (the strategic
master plan). That document says *why* the fiber effect runtime must be deleted
and states the kill criteria. This document is the concrete, **measured**
work-list: exactly which fixtures still ride the fiber under
`--enable=cps-tramp-resume`, why (root cause pinned to a repro), and the fix
direction for each -- so the flag can graduate to always-on and the fiber effect
machinery can be deleted.

## 0. Measure this correctly -- do NOT trust the `eff=1` column

The `TUR_TRACE_EVICT=1` `[EVICT] ... eff=N` trace is the WRONG signal for the
deletion goal, in two directions:

- **It over-counts.** `eff` is `(se->eff_lo || se->eff_hi)` -- the *entire*
  effect row, which includes compile-time-only markers (`#fx{Unsafe}`, `IO`).
  So `unsafe-*`, `variadic-*`, `sized-*`, `lint-*`, `free-*` fixtures light up
  `eff=1` while never touching the fiber effect runtime at all.
- **It under-counts.** The trace only fires for a `cps_colored` *named* `defn`
  that falls back to the direct emitter. A top-level handle folded into a
  synthesized main, a handle inside `(defmodule ...)`, and a first-class
  `with-handler` value emit fiber `tur_effect_perform` with **no eviction trace
  line at all**.

**The correct measure** is the emitted C itself: count `tur_effect_perform("`
call sites (string-literal effect name = a real call; the bare
`tur_effect_perform(const char*...)` definition in the preamble is not a call
site) plus `__handle_body_N(void)` fiber handle thunks.

```sh
TUR=./build/tur
for dir in tests/fixtures/*/; do
  input="$dir/input.tur"; [ -f "$input" ] || continue
  n=$("$TUR" --enable=cps-tramp-resume emit-c "$input" 2>/dev/null \
        | grep -cE 'tur_effect_perform\("')
  [ "$n" -gt 0 ] && echo "$(basename "$dir") $n"
done
```

Under this measure, on 2026-07-18: the 22 opted-in `cps-tramp-resume-*` fixtures
are **100% fiber-clean**, and exactly **24 non-opted-in fixtures** stay
fiber-bound when the flag is forced on. Those 24 are the entire remaining
surface. The goal is to drive them to zero (minus the documented permanent
carve-outs), then flip the flag on by default and delete the fiber effect
runtime.

## 1. The remaining surface -- 24 fixtures, 8 buckets

Fixable: **B1-B7 (20 fixtures)**. Permanent-candidate carve-outs: **B8 (4)**.

Recommended execution order is by leverage: **B3 (7) -> B1+B2 (7) -> B4/B5/B6
-> B7 -> decide B8**.

---

### B1 -- macro-expanded top-level handle (3 fixtures) -- FIXABLE

**Fixtures:** `effect-with-write`, `effect-with-fail`, `effect-with-getenv`.

**Measured root cause.** A *literal* top-level handle already DK-lowers:

```turmeric
(defeffect Ask [] :int)
(handle (do (let [x (perform (Ask))] (println x)))
  (Ask [] k) (resume k 42))         ; => 0 perform sites under the flag
```

A **macro-expanded** one does not:

```turmeric
(defeffect Write [s :cstr] :nil)
(defmacro with-write [body]
  (handle body (Write [s] k) (do (println s) (resume k nil))))
(with-write (do (perform (Write "hello")) (perform (Write "world"))))
                                    ; => 2 perform sites (fiber) under the flag
```

The synthesized-main fold (`elab_toplevel.c`, `fold_stmt_is_risky` /
the fold gate) is intentionally macro-conservative: it aborts the fold when a
top-level form has a macro head, so a macro that expands to a `handle` never
reaches the d2b-main DK path.

**Fix direction.** Run the fold *after* top-level macro expansion (fold on the
expanded form), or teach the gate that a macro whose expansion is a single
`handle`/`reset`/`do`-of-effectful-statements is fold-safe. The DK lowering of
the resulting handle already works (the literal case proves it) -- this is
purely about letting the expanded form reach the fold. Guard: flag-off must stay
byte-identical; only the flag-on synthesized main changes.

---

### B2 -- handle inside `(defmodule ...)` (4 fixtures) -- FIXABLE

**Fixtures:** `module-effect-private`, `module-cross-module-effect`,
`effect-export-explicit`, `effect-row-cross-private`.

**Measured root cause.** The blocker is the `defmodule` wrapper itself, isolated
by bisection:

| Variant | perform sites (flag on) |
|---|---|
| `(defn run [] (handle ...))` called from `main`, no module | **0** (DK) |
| `^export run`, no module | **0** (DK) |
| same code inside `(defmodule mymod (export run) ...)` | **1** (fiber) |
| inside `(defmodule ...)` with NO export | **1** (fiber) |
| inside `(defmodule ...)` with `^private` effect | **1** (fiber) |

So it is neither the export nor `^private` -- wrapping an otherwise-DK-lowerable
`handle` in `(defmodule ...)` reintroduces the fiber lowering. The
module-qualified function path (`mymod__run`) is not being routed through the
DK/d2b machinery the bare top-level path uses.

**Fix direction.** Trace where module member elaboration diverges from
top-level `defn` elaboration (module name-mangling / the module-member emit
path in `elab_toplevel.c` + `emit_cps_ir.c`). The colored classifier and the
d2b/DK lowering must see module members identically to top-level defns. Likely a
missing `cps_colored`/d2b classification on the module-qualified binding, or the
module emit path bypassing `emit_cps_ir`.

---

### B3 -- first-class handler VALUES (7 fixtures) -- FIXABLE, highest leverage

**Fixtures:** `fh-handler-value`, `with-handler-value`, `fh-multishot-value`,
`fh-multi-effect-type`, `fh-compose-handlers`, `defstruct-field-handler`,
`defstruct-field-handler-multi`.

**Measured root cause.** These use the first-class handler form -- a runtime
handler object built with `(handler (E [x] k) ...)` and installed with
`with-handler` -- rather than the `handle` special form:

```turmeric
(with-handler (handler (Ask [] k) (resume k 42))
  (do (println (perform (Ask))) 0))
```

plus its combinators: `compose-handlers` (two handler values merged) and
handler values stored in a `defstruct`/ADT field (`(handler Ask int int)` typed
field, read back with `.h` and installed). The DK backend lowers the static
`handle` special form but not the runtime-handler-value path -- that path still
lowers to `__handle_body_N` / `tur_effect_perform`.

**Fix direction.** This is the single biggest win (7 fixtures) and the deepest
of the fixable buckets. `with-handler` applied to a handler *value* needs a DK
lowering analogous to `handle`: install the handler object's cases as a DK
handler frame (`dk_hgroup` / the `DKK_HANDLER` path) at runtime. The handler
value already lowers to `TY_HANDLER` on the int64 carrier; the work is emitting
a DK handler-install from a runtime handler object instead of from static
handle-case syntax. `compose-handlers` = install both case-sets in one group;
struct-field handlers = the same install where the handler object comes from a
field read. Do `fh-handler-value`/`with-handler-value` first (simplest), then
multishot/multi-effect, then compose/struct-field.

---

### B4 -- cross-function effect propagation (2 fixtures) -- FIXABLE

**Fixtures:** `cps-backend-effect-under-match`, `handle-effectful-fn-param-same-fn`.

- `cps-backend-effect-under-match`: a `perform` inside a `match` arm of a callee
  (`pick`) reached transitively through another callee (`route`) from the
  handler's `run`. The colored `__cps` threading is not propagating through the
  match-arm-in-a-transitively-called-function shape.
- `handle-effectful-fn-param-same-fn`: an effectful fn-**value** parameter --
  `(defn run-with [f : (fn [] #fx{E} int)] (handle (f) (E [] k) (resume k 5)))`
  called with both a lambda and a named effectful fn. This is the long-standing
  "E2 effectful fn-value" root; see the (still-open) reports
  `docs/reported/cps-effectful-fnvalue-call-under-handle-installing-hof.md` and
  `docs/reported/effectful-fnvalue-param-miscompile.md`.

**Fix direction.** Extend the E2 effectful-fn-value threading (a `DK*`-carrying
fn-value calling convention) so a colored fn-value parameter called under a
handle threads the DK; and ensure colored `__cps` propagation reaches a
`perform` under `match` in a transitively-called callee.

---

### B5 -- async x effect interaction (2 fixtures) -- FIXABLE (scope: async lowering)

**Fixtures:** `effects-async`, `async-with-handler`.

**Measured root cause.** A `handle` / `with-handler` **inside an `async`
closure**:

```turmeric
(async (fn [] (handle (perform (AddTen 5)) (AddTen [x] k) (resume k (+ x 10)))))
```

The effect handle inside the async-spawned closure stays on the fiber. Related
(and correctly by-design) is `cps-async-recursive-await-eviction` (archived) --
recursive `await` deliberately evicts. This bucket is specifically the
handle-inside-async body.

**Fix direction.** The async closure body must DK-lower its interior handle like
any other function body. Coordinate with the `cps-async` experiment
(`g_opt_cps_async`) -- if async is itself CPS-lowered, the interior handle
should compose with it. May be gated behind `cps-async` graduation.

---

### B6 -- effectful typeclass instance method (1 fixture) -- FIXABLE

**Fixture:** `typeclass-effect-row-caller`.

**Measured root cause.** An instance method that performs an effect
(`(io-show [x] :nil (perform (Write "42")))` for `IOShow int`), reached through
a dict slot. The instance method is `SIG-EXPORT` (`c_export_name` pinned,
`__inst_IOShow_io_hyshow_int`) and effectful, so it routes to the fiber.

**Fix direction.** An effectful `__inst_*` method needs a `__cps` variant
reachable through the dict slot (a DK-threaded instance-method calling
convention), or the caller's handle must DK-lower across the dict-dispatched
effectful call. This is the "effectful exported instance" case the master plan's
`SIG-EXPORT` row flagged as "only the effectful instances."

---

### B7 -- escaping / multishot continuation via `set!` (1 fixture) -- FIXABLE, hardest

**Fixture:** `effect-capture-k`.

**Measured root cause.** The genuinely hard one -- the handler captures its
continuation into an outer mutable and resumes it **after** the handle exits:

```turmeric
(let [^mut k-store 0]
  (let [first-result (handle (compute)
                       (Ask [] k) (do (set! k-store k) 0))]
    (println first-result)
    (let [second-result (resume k-store 5)]   ; resume AFTER handle returned
      (println second-result))))
```

The continuation escapes its handler dynamic extent and is invoked later. This
needs a heap-cell (by-reference) mutable capture plus copy-on-store so the stored
continuation survives `dk_perform`'s free of the captured chain
(`tur_cloneable_cont_alloc` is the existing reified-continuation substrate).

**Fix direction.** By-reference (heap-cell) mutable capture for a stored
continuation + copy-on-store into the `^mut` so the DK chain outlives the
handler. See `docs/upcoming/cps-backend-multishot-continuations-owning-capture-plan.md`
and `docs/upcoming/cps-dk-multishot-user-effects-plan.md`.

---

### B8 -- fiber-substrate-entangled (4 fixtures) -- PERMANENT CANDIDATES

**Fixtures:** `fiber-effect`, `p19-8-fiber-effect-chain`, `session-effects`,
`session-mp-effects`.

**Measured root cause.**

- `fiber-effect`, `p19-8-fiber-effect-chain`: **deliberately** exercise the
  cooperative-fiber coroutine primitive via inline-C (`tur_fiber_block_new/
  resume/yield/free`), with an effect `handle` running *on* a fiber. The
  coroutine-fiber feature is a legitimate separate language feature; the effect
  handle inside a fiber-run function cannot DK-lower across the opaque inline-C
  fiber boundary.
- `session-effects`, `session-mp-effects`: `spawn` is inline-C
  `pthread_create(..., tur_session_thread_wrapper, ...)`; the effectful
  `exchange`/`role-*` perform `SessionLog` across a pthread + inline-C session
  runtime. `SIG-INLINE-C` main -- the CPS backend cannot thread a DK through an
  opaque inline-C body.

**Decision needed.** These are the master plan's honest carve-outs. Two options:
(a) accept them as permanent `SIG-INLINE-C` carve-outs and delete the fiber
*effect* runtime anyway *iff* the coroutine-fiber primitive (`tur_fiber_block_*`,
distinct from `tur_effect_perform`) is retained as its own feature; or (b)
separate the cooperative-fiber substrate from the effect runtime so the effect
machinery can be deleted while `tur_fiber_block_*` stays. **The key question for
the master plan's kill criteria: is `tur_effect_perform` implementable purely on
`tur_fiber_block_*`, or are they independent?** If independent, B8 does not block
deleting the effect runtime -- these fixtures keep using the coroutine fibers and
never touch effect perform once their interior handles (where possible) DK-lower.

---

## 2. Verification protocol (every slice)

1. **Flag-off byte-identical.** For every fixture, `emit-c` with no flag must be
   byte-identical before/after the change (the experiment is gated; the shipping
   path must not move). Snapshot-diff the corpus.
2. **Flag-on fiber-clean.** The target fixture's `emit-c --enable=cps-tramp-resume`
   drops to **0** `tur_effect_perform("` call sites.
3. **Output equivalence.** The fixture's program output is identical on the fiber
   path and the DK path (direct-vs-DK equivalence), e.g. `effect-with-write`
   still prints `hello`/`world`.
4. **ASan clean.** The emitted program is ASan/UBSan-clean (no UAF / double-free /
   leak) -- the borrowed-`__kont` class of bug (see
   `docs/archive/cps-effect-nested-value-position-borrowed-kont-uaf.md`) is the
   cautionary precedent.
5. **Suite.** `bash tests/run.sh` (12-min timeout) -- read the result; regen
   `expected.c` snapshots in the same change if codegen moved.

## 3. Definition of done (flag graduation)

The `cps-tramp-resume` experiment graduates (flag deleted, feature always-on) and
the fiber **effect** runtime is deleted when:

- B1-B7 are closed: the fiber-live sweep (Sec 0) returns only the B8 carve-outs.
- The B8 decision (Sec 1) is made and, if any remain, they are documented
  permanent `SIG-INLINE-C` carve-outs that provably do not require the fiber
  *effect* machinery (only the coroutine-fiber primitive, retained separately).
- `tur_effect_perform`, `EffectHandlerFrame`, `global_effect_handler_chain`,
  `tur_handler_dispatch`, and the `emit_effects.c` direct perform/handle/resume
  emitters are removed; the suite stays green flag-off (now the only path).

## 4. Status of the reports this plan supersedes

Resolved and archived 2026-07-18 (verified by the Sec 0 measure): the
effect-subtype cluster (`cps-e2-pure-lambda-into-effectful-fnvalue-param`), the
while-loop interior-handle residue
(`cps-while-native-conservative-subset-fiber-residue`,
`cps-while-loop-with-interior-handle-no-native-lowering`), the effect
pass-through soundness bug (`handle-body-passthrough-effect-unhandled`, now
prints `12`), the async recursive-await by-design note
(`cps-async-recursive-await-eviction`), and the permanently-fiber-bound session
note (`cps-session-effects-permanently-fiber-bound`, now B8 here).

Still open in `docs/reported/`: `cps-toplevel-synthesized-main-bypasses-dk`
(B1 + B2), `cps-effectful-fnvalue-call-under-handle-installing-hof` +
`effectful-fnvalue-param-miscompile` (B4), and three non-fiber-liveness residuals
that are correctness/hygiene, not blockers for this deletion:
`cps-reopen-perform-onode-leak` (O(N) DK-node leak, flag-on only),
`cps-drop-elided-under-delimited-control` (heap leak), and
`cps-named-receiver-uniform-fnptr-cast` (low-severity fn-ptr-cast UB). The
`uncons-hyfmap-cps-direct-emits-unmangled-tcons` warning is a live but separate
codegen defect.
