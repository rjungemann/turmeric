# CPS `CT_MATCH` does not drop its heap-ADT scrutinee (flag-on leak)

> **Archived 2026-07-19 -- SUBSUMED by `cps-owning-adt-value-not-dropped-under-match`.**
> This report's premise -- "Flag-off is leak-clean: the direct emitter's ownership
> pass drops the scrutinee" -- does NOT hold. Verified on the exact fixture
> (`cps-backend-effect-under-match`) under `cc -fsanitize=address,undefined`: the
> shipping **flag-off** build leaks the identical 16-byte scrutinee box
> (`ctor_Full <- route__cps <- run__cps`), and the flag-on build leaks the same 16
> bytes (plus the universal DK-node baseline). So the direct emitter does not drop
> a matched non-`rc` boxed ADT either -- the scrutinee non-drop is Turmeric's
> documented "non-`rc` heap value is never auto-freed" memory model
> (`docs/guides/gc-guide.md`), not a CPS-specific defect. Teaching `emit_match`
> (CPS) to drop the scrutinee would DIVERGE from the shipping direct emitter and
> from the memory model, so the requested fix is intentionally not implemented.
> If bare boxed ADTs ever become RC-managed, that language-level change lands in
> the direct emitter's drop pass and both paths inherit it together.

**Severity:** low (experimental `--enable=cps-tramp-resume` path only; shipping
flag-off path unaffected).

## Summary

The B4 `CT_MATCH` lowering (`emit_match` in `src/compiler/emit_cps_ir.c`) reads
the scrutinee's `->tag` / `->as.<Ctor>._N` fields but never emits a drop of the
owned scrutinee node. A boxed tagged-sum ADT passed by-move into a colored
`match` therefore leaks its carrier node on the DK path.

## Repro

`tests/fixtures/cps-backend-effect-under-match/input.tur` (a colored `pick`
matching a `Box` and performing `Choose` under the `Full` arm):

```sh
CC="cc -fsanitize=address,undefined -g" \
  ./build/tur --enable=cps-tramp-resume build \
  tests/fixtures/cps-backend-effect-under-match/input.tur -o /tmp/eum
ASAN_OPTIONS=detect_leaks=1 /tmp/eum
# => Direct leak of 16 byte(s) ... ctor_Full ... route__cps
#    (plus the universal 104-byte dk_new baseline every flag-on effect fixture carries)
```

Flag-off (`./build/tur build ...`) is leak-clean: the direct emitter's ownership
pass drops the scrutinee. The fixture runs flag-off under `tests/run.sh`, so the
suite stays green.

## Root cause

`emit_match` (`src/compiler/emit_cps_ir.c`) binds the scrutinee as a typed
carrier pointer and extracts fields, but emits no `drop_glue_tur_adt_<Name>` for
the scrutinee value the way the direct match emitter (`emit_expr.c`, the
`EX_MATCH` path) does.

## Fix directions

Non-trivial because the drop cannot sit before a `perform` in an arm: a
multi-shot resume would re-run the arm and double-free / use-after-free. The
principled placement is either (a) drop in each arm's straight-line tail after
the fields are consumed and before the delivery, guarded so a suspending arm
defers the drop into its resume continuation, or (b) follow the existing DK
discipline and reap the scrutinee node at the entry boundary (`__dk_reap_ptr`)
when the scrutinee is a by-move owned value with no owning fields still aliased
by a binding. Option (b) matches how the flag-on path already handles its
structural DK-node leaks ("leaked with the DK nodes, reaped at the outermost
entry boundary").

Until then the flag-on path leaks the scrutinee node, consistent with the
existing flag-on DK-node leak baseline; the shipping (flag-off) path is correct.
