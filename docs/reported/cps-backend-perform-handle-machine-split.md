# CPS backend can split a perform/handle pair across the two effect machines (crash)

**Severity:** high (miscompile -> abort), but **gated**: only reachable under
`--enable=cps-backend` (off by default), so the default suite is unaffected. It
is a graduation blocker -- the `cps-backend` graduation gate requires no
miscompiles with the fallback removed.

## Summary

The CPS-IR-to-C backend classifies each colored function for CPS emission
**independently** (`ensure_S` in `src/compiler/emit_cps_ir.c`: per-function
`fn_sig_ok` + `term_core_ok`). Algebraic effects, however, couple two functions
at runtime: a `perform` must be caught by a `handle` **on the same machine**.

- A CPS-emitted performer lowers `perform` to `dk_perform(tag, ..., k)`, which
  searches the **DK continuation chain** for a `dk_handler`.
- A CPS-emitted handler installs that `dk_handler`.
- A fallback (fiber) performer/handler uses `tur_effect_perform` + the fiber
  `__eff_frame` handler stack instead.

Nothing forces a perform/handle pair to be classified the same way. When the
performer is admitted to the CPS set but its handler falls back (or vice versa),
the two machines do not interoperate: the `dk_perform` finds no DK handler in
the chain (the fiber handler is invisible to it), and the program aborts with
`tur: unhandled effect (tag N)`.

## Minimal repro

Handler forced onto the fallback path by a capture in its case body, performer
stays CPS-emittable:

```turmeric
(defeffect E [] :int)
(defn f [] : int (perform (E)))            ; simple -> CPS-emitted (dk_perform)
(defn run [x : int] : int
  (handle (f)
    (E [] k) (resume k x)))                ; case captures `x` -> run falls back
(defn main [] : int (println (run 5)) 0)
```

```
$ tur run repro.tur                       # direct-style
5
$ tur run repro.tur --enable=cps-backend  # CPS backend
tur: unhandled effect (tag 2)
Aborted
```

`tur emit-c ... --enable=cps-backend` shows `f__cps` emitted but no `run__cps`
(`run` fell back). `f__cps` performs via `dk_perform(2, ...)`; `run`'s fallback
installs a fiber `__eff_frame` handler, which `dk_perform` cannot see.

Any reason a handler falls back while its performer does not (or the reverse)
triggers this: a captured var in the case, `>1` handler case, a non-Tier-value
in the handler's own body, etc. The `(:: N :uint64)` reinterpret gap was one
such trigger (now fixed -- see below), but the hole is general.

## Root cause

`ensure_S` (`src/compiler/emit_cps_ir.c`) admits a function to the CPS set on a
purely **intra-function** predicate. There is no whole-program **effect
coherence** constraint tying a `perform E` site to the `handle E` that
dynamically encloses it. Because effect handling is dynamically scoped, the
enclosing handler is not syntactically visible at the perform site, so a naive
per-function fixpoint cannot see the coupling.

## Fix directions

The safe invariant: **for a given effect, every colored function that performs
or handles it must be classified the same way** (all CPS, or all fallback).
Options, roughly in increasing precision:

1. **Conservative co-classification.** Build an effect -> {colored functions
   that perform or handle it} map during `ensure_S`. In the fixpoint, if any
   member is not CPS-admissible, evict *all* members of that effect's group from
   the CPS set (they fall back together, coherently). Simple, sound, slightly
   over-broad (one un-analyzable handler taints the whole effect's fast path).
2. **Handler-rooted admission.** Only admit a performer of `E` to the CPS set
   once at least one CPS-admissible handler of `E` exists and the fixpoint has
   proven every reachable handler of `E` is CPS. Requires call-graph reachability
   from performer to handler.
3. **Runtime bridge.** Make `dk_perform` fall through to the fiber handler stack
   when no DK handler is found (and the fiber `perform` consult the DK chain).
   Removes the crash without co-classification, but couples the two runtimes and
   muddies the "one machine per colored region" model; likely undesirable.

Option 1 is the smallest sound step and fits the existing whole-program fixpoint
in `ensure_S`. It should land before the fallback is removed at graduation (the
gate in `docs/upcoming/v1/cps-backend-non-scalar-values-plan.md`).

## Status

- The `(:: N :uint64)` / `(:: N :int64)` reinterpret trigger is **fixed**:
  `src/passes/cps_ir.c` now peels a same-size Tier A `EX_REINTERPRET` (see
  `is_tierA_reinterp` / `tierA_scalar_kind`), so a 64-bit ascribed literal no
  longer forces the handler onto the fallback path. That closes the specific
  crash above for the uint64/int64-literal shape.
- The **general** coherence hole (handler falls back for any other reason while
  the performer stays CPS) remains open and is the subject of this report.

## Related

- `docs/reported/cps-coloring-ascription-hides-control-op.md` -- a different
  coloring-vs-backend gap (coverage, not a crash).
- Parent plans: `docs/upcoming/v1/cps-ir-to-c-backend-plan.md`,
  `docs/upcoming/v1/cps-backend-non-scalar-values-plan.md`.
