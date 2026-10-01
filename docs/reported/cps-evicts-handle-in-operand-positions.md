# The CPS backend still evicts some `handle`s over effectful fn fields

**Severity: medium.** A legal program is refused at build time with "this
effect operation has no lowering here". This is an expressiveness gap, not a
miscompile. It is what remains of the type fuzzer's `GEN_REJECT`s after
`handle-over-effectful-fn-field-in-arg-let-evicted` (archived) was fixed:
fuzz seeds 3333 and 4444 produce 4 in 600 cases, all through the
`fn_field_eff` crossing.

## Shapes still refused (2026-10-01)

`TUR_TRACE_EVICT=1` names the evicting form in each case:

1. **A rank-2 poly call with a `handle` operand.** `l0ru7 BODY-UNSUPPORTED
   indirect call (non-atomic args)`: a poly-fn parameter call `(l v)` whose
   argument contains the handle. The method-call twin was fixed by hoisting
   control operands in `elab_method_call`. A rank-2 call goes through a
   different elaboration path.
2. **`unsupported form: EX_REINTERPRET`.**
   `(handle (.run (make-struct FE fe) (g2 "s")) ...)`, where `g2` is a
   generic whose result is reinterpreted to `cstr`. Since the registry paths
   atomize non-atomic arguments, the reinterpret reaches the CPS translation.
   Neither `cps_bind` nor `cps_tail` in `src/passes/cps_ir.c` lowers a
   reinterpret that is not the tyvar-result wrapper they already delegate. An
   attempted fix is described below.
3. **A signature-rejected generic HOF** (`l2gbapp11 SIG-REJECT`) on the path
   from the handler to the performer.
4. **A join continuation that would capture the field-load callee.**
   `(handle (.run (make-struct FE fe) (let [c false] (t (fn [] c)))) ...)`
   is `BODY-STRUCT-JOIN` (`needs_heap_join`). Before the E2c capture fix
   (`CC_ATOM` / `COL_ATOM` of `tailcall.fn_atom`, 2026-10-01) the join read the
   callee atom uncaptured, which was invalid C (`use of undeclared identifier
   '__t2'`). It is a clean refusal now, not admitted.

## What was tried for (2)

Binding a word-to-word reinterpret's operand directly to the typed binder in
`cps_bind`/`cps_tail`, and delegating a reinterpret with a delegatable operand
whole. Neither changed the trace: the rejected node is reached somewhere
else, and a gdb breakpoint on `unsupported_form` was not hit (compilation
runs on a worker thread, and LeakSanitizer aborts under ptrace). Reverted.
Next step: build `tur` without sanitizers (`-DTUR_DEBUG_SANITIZE=OFF`) to
debug, or add a `TUR_TRACE_EVICT` field naming the parent node of the
unsupported form.

## Regression check

The fuzzer's `GEN_REJECT` count on a fixed seed: `python3
tests/type-fuzz-src.py --n 300 --seed 3333`, which is 3 today.
