# Fix: native CPS/DK lowering of a `while` with an interior control op (CT_LOOP)

Resolves `docs/archive/cps-while-loop-with-interior-handle-no-native-lowering.md`.

## Summary

`effect-handler-capture-loop` (a `while` whose body installs a fresh per-iteration
`handle` and mutates two `^mut` loop vars) evicted `BODY-UNSUPPORTED EX_WHILE`
under `--enable=cps-tramp-resume` and ran on the fiber.  It now lowers natively:
`run` emits `run__cps` (zero `eff=1`) and prints `100`.  All changes gated on
`g_opt_cps_tramp_resume`; the default suite is byte-identical (2197 -> 2200 with
the three new flag-on fixtures, 0 failed).

## The shape that shipped -- a recursive `__cps` helper, not a same-function join

The DEEPER SCOPE design (a `CT_LOOP` lowered to a same-function multi-arg JOIN, a
C label + `goto` back-edge) was RETRACTED after pinning it against the emitter:
`emit_handle` (`emit_cps_ir.c:5438`) lifts the handle continuation
(`t->as.handle.body`) into a SEPARATE C function (`_hk`, run as a `dk_frame`).
This loop's carried update is `total' = total + <handle result>` -- the new value
is produced INSIDE that lifted continuation, so the back-edge lives there, in a
different C function from the loop entry.  A `goto` cannot cross that boundary, so
a same-function join is impossible.  The back-edge must be a real function call ->
the loop is a synthesized tail-recursive colored `__cps` helper:

```
run_loop0__cps(total, i, __kont):        # ^mut loop vars are the params
  if (< i 5):
    cur = i
    <CT_HANDLE  perform(Ask) / case Ask k -> resume k (cur*10)>   # delim
    # continuation (lifted _hk, receives handle result __t4):
    total$next = (+ total __t4)
    i$next     = (+ i 1)
    return run_loop0__cps(total$next, i$next, __kont)   # CT_CONTINUE back-edge
  else:
    return <deliver total to KK_RET>                    # exit
# run__cps(__kont)  ==>  total=7-or-0; i=0; return run_loop0__cps(total, i, __kont)
```

The back-edge is an ordinary tail-call to a colored `__cps` fn -- already what
`emit_term` lowers -- and composes with the E7 trampoline for free.  No new
looping-join emitter construct.

## The soundness crux and how the lowering sidesteps it

The CPS IR has no mutation representation (`atom_of` maps a var straight to its
source `Binding`), and `cps_tail` lowers `EX_DO` RIGHT-TO-LEFT, so a mutable
`Binding -> CVar` rebinding table is unsound (a later-execution `set!` would be
lowered before an earlier one).  Two moves avoid it entirely:

1. **Reads resolve by NAMING, not a map.**  Each loop param CVar carries its
   source `Binding`, so every in-body read (`EX_VAR i -> CA_VAR{i}`) names the
   param via `name_for_binding` -- automatically the loop-ENTRY version.  No
   rebinding environment.
2. **`set!` writes a PRE-CREATED `$next` CVar.**  The `$next` CVars are created
   once at loop setup and stored in `CpsB` (stable identity).  `set! v e` lowers
   to `cps_bind(e, $next[v], ...)`; `CT_CONTINUE` reads `$next[v]`.  Both resolve
   by identity regardless of build order -- the right-to-left hazard cannot bite.

This is correct ONLY when every in-body read of a loop var observes the entry
version (no read sees a `set!` done earlier THIS iteration).  A conservative
guard (`loop_guard`, execution-order read-after-set walk) enforces exactly that,
plus: every carried var set exactly once, unconditionally (no `set!` under an
`if`/`match` or a handler case), and the continuation is a trivial delivery of a
single loop-carried "live-after" var.  Anything outside the subset returns NULL ->
`unsupported_form` -> the prior correct fiber fallback.  (Companion
`cps-tramp-resume-while-readset` exercises the read-after-set eviction; the escape
companion exercises an effect that leaves the loop.)

## What landed

- **IR** (`cps_ir.h`): `CT_LOOP { CVar *params; uint32_t n_params; CAtom *inits;
  CTerm *body; CKont result_kont; }` and `CT_CONTINUE { CAtom *args; uint32_t n; }`,
  plus a transform-internal `KK_LOOP` continuation kind.
- **Transform** (`cps_ir.c`): `build_loop` + the guards (`loop_collect_let_binders`,
  `loop_collect_carried`, `loop_guard`), the `EX_WHILE` case in `cps_bind` (with a
  delegation fallback preserving prior behavior for a control-free while), the
  `EX_SET` cases in `cps_bind`/`cps_tail` (loop-carried -> `$next`, else the prior
  default), the `KK_LOOP` back-edge delivery, and `cps_ir_print` for both nodes.
- **Emitter** (`emit_cps_ir.c`): `emit_loop` (forward-decls the helper, emits its
  body into `ce->helpers` after the interior handle's own lifted helpers, emits the
  entry tail-call) and `emit_continue` (the back-edge, threading `__kont` -- the
  loop helper's k, reachable inside the lifted handle continuation as `env->__k`).
  `CT_LOOP`/`CT_CONTINUE` added to every CTerm walker: `term_core_ok`,
  `has_capture_rec`, `collect_caps_rec`, `joins_closed_rec`, `is_cps_island`,
  `first_unsupported`, `emit_binder_decls`, `emit_term`.

## Verification

- `effect-handler-capture-loop` flag-on: `run` DK-lowers (no `eff=1`), prints `100`.
- New flag-on regression fixtures: `cps-tramp-resume-while-handle` (non-zero init
  `total`, distinct constants -> `1507`), `cps-tramp-resume-while-handle-escape`
  (`50`), `cps-tramp-resume-while-readset` (`10`).
- Default suite: `2200 passed, 0 failed` (flag-off byte-identical; snapshots match).
- Flag-on soundness sweep over 716 effect/mutation fixtures: every mismatch is a
  pre-existing sweep-harness artifact (all `off==on`, none emit a `CT_LOOP`) --
  no mutation miscompile.  `effect-handler-capture-loop` is the only corpus fixture
  that emits a native `CT_LOOP`.

## Follow-on: escaping interior effect (2026-07-17)

The initial landing left a `while` whose interior `perform` ESCAPES to an OUTER
handler on the fiber (`cps-tramp-resume-while-handle-escape` ->
`BODY-STRUCT-OR-TAINT eff=1`).  Diagnosed: `build_loop` DID fire (the CT_LOOP was
built cleanly, no `CT_UNSUPPORTED`), but admission rejected the whole term because
the interior `perform`'s continuation ends in `CT_CONTINUE` (the back-edge), which
`perform_body_ok`/`perform_cont_reset_ok` did not admit.  Since `perform_body_ok`
has no `CT_CONTINUE` case, adding one to `perform_cont_reset_ok` (mirroring its
`CT_TAILCALL` case, gated on the flag) makes `emit_perform` lift the continuation
as an `LH_RESUME_CONT` resume-frame -- which carries `__kont` -- and the back-edge
emits `return <loop>__cps(args, __kont)` reaching the outer handler through the
loop helper's threaded prompt chain.  One-line admission add; `run` now DK-lowers
(eff=0).  Verified: `-while-handle-escape` (50), a non-zero-init `i*perform`
variant (63), a two-escaping-effect `total*2+1` recurrence (31); default suite
`2200 passed, 0 failed`; flag-on sweep unchanged (all 39 mismatches are
pre-existing 0-`CT_LOOP` artifacts).  Residue tracker updated:
`docs/reported/cps-while-native-conservative-subset-fiber-residue.md`.

## Known follow-ups (NOT this fix)

- The subset is conservative by design (single live-after var, unconditional single
  set per var, reads-entry-only, no nested loops).  Wider shapes (multiple
  live-after vars, conditional sets, read-after-set) still evict to the fiber
  correctly.  Widening would need a real SSA-renaming pre-pass; out of scope here.
- `effect-handler-capture-loop`'s own `flags` file is still empty (it runs flag-off
  on the fiber); the flag-on lowering is covered by the new `cps-tramp-resume-while-*`
  fixtures.
