# A `while`-loop in a handle continuation evicts (capture-collection + loop-invariant threading gaps)

**STATUS: RESOLVED.** A `while`-loop sitting in a `handle`'s CONTINUATION now
DK-lowers under `--enable=cps-tramp-resume`. Two gaps were fixed in
`src/compiler/emit_cps_ir.c`:

1. **Admission** -- `has_capture_rec` / `collect_caps_rec` had no `CT_LOOP` case, so
   a loop in a lifted continuation bailed (`collect_caps` -> `cs->ok = false`),
   failing the enclosing `CT_HANDLE`'s continuation-capture check and evicting
   BODY-STRUCT-OR-TAINT. Added `CT_LOOP` cases (collect the loop's init atoms +
   the outer vars its body reads; bind the carried params), mirroring the existing
   `CT_CONTINUE` cases.
2. **Emission** -- `emit_loop`'s synthesized helper `<fn>_loop<id>__cps(<carried>,
   DK*)` only took the loop-carried params, so a loop body that reads a
   loop-INVARIANT free var (a fn param `base`, the handle result `n`) emitted an
   undeclared reference (`'n_*'/'base' undeclared`). `emit_loop` now collects the
   body's non-carried free vars and threads them as extra helper params, passed
   unchanged at the entry call and every `CT_CONTINUE` back-edge (via the new
   `CE.cur_loop_inv`).

Moved onto the DK: `cps-backend-composite-in-continuation` and
`cps-oracle-shift-under-handle` (both `(let [n (handle (g) ...)] (... while over
^mut acc/i reading base/n ...))`, output 40). Regression fixture:
`cps-tramp-resume-loop-in-handle-continuation`.

## Verification

- `f`/`g` in the two fixtures emit `f__cps`, zero `eff=1`, output `40` (flag-on and
  flag-off identical).
- Existing while-native fixtures unchanged (`cps-tramp-resume-while-handle` 1507,
  `-while-handle-escape` 50, `-while-readset` 10, `effect-handler-capture-loop` 100).
- Default suite `bash tests/run.sh` 2202/0 (flag-off byte-identical -- the
  admission only opens under the flag-on candidate path); flag-on build sweep clean.

## Context

One of the 4 non-permanent CPS roots in
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md. Related: the native
`while`-loop lowering (`CT_LOOP`/`CT_CONTINUE`) and its conservative-subset residue
(cps-while-native-conservative-subset-fiber-residue.md). This slice extends that
lowering to the loop-in-continuation position (a loop reading loop-invariant
outer vars), leaving the mutation-width residue (read-after-set, multi-live-after)
as the remaining while-loop follow-on.
