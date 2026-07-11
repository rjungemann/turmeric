#ifndef TUR_EMIT_CPS_H
#define TUR_EMIT_CPS_H

#include "expr.h"
#include "buf.h"
#include "emit_internal.h"

/* =========================================================================
 * emit_cps.h -- CPS-substrate codegen wiring (cps-transform-plan).
 *
 * This is the codegen integration the CPS plan deferred: it re-points the
 * base delimited-control lowerings (reset / shift / shift0) off the bespoke
 * inlined runtime and onto the heap-reified CPS substrate's multi-prompt
 * delimited-control machine (the `DK` chain evaluator from
 * src/runtime/cps_prompt.{h,c}), emitted directly into the generated program.
 *
 * A `(reset BODY)` whose body can dynamically reach a `(shift f v)` to that
 * reset is compiled to a `DK` chain run by `dk_run`: the reset is a prompt,
 * the shift is a `DK` shift node, and the value is delivered through the
 * machine. Turmeric's shift is abortive (the receiver `f` is applied to the
 * body value; the captured sub-continuation is not handed back to user code),
 * so the captured context is discarded by the machine -- which the DK model
 * expresses directly. Sub-expression evaluation reuses the direct-style
 * emitter (emit_value), so all value types/closures/builtins keep working.
 *
 * Shapes outside the supported subset fall back to the legacy lowering, so
 * the rest of the suite is byte-identical.
 * ========================================================================= */

/* True iff `program` uses base delimited control (EX_RESET / EX_SHIFT /
 * EX_SHIFT0) anywhere -- the gate for emitting the DK runtime prelude. Does
 * NOT count cloneable-/serial-/effect forms (those keep the legacy runtime). */
bool emit_cps_program_uses_delimited(const Expr *program);

/* call-cc-completion: true iff `program` uses (call/cc f) or (escape f)
 * (EX_CALLCC) -- the gate for emitting the escape-continuation runtime. */
bool emit_cps_program_uses_callcc(const Expr *program);

/* NOTE: the DK runtime prelude emitters (emit_cps_runtime_prelude,
 * emit_cps_callcc_prelude, emit_cps_cloneable_bridge_prelude,
 * emit_cps_serial_runtime_prelude) were relocated to emit_dk_runtime.h
 * (cps-backend-unification U7, step 1). The emit_cps_program_uses_* gates that
 * decide whether to emit each prelude stay here. */

/* Lower (call/cc f) / (escape f) (EX_CALLCC): establish a setjmp landing at
 * this site (the implicit-prompt capture point), hand f the landing handle,
 * and return the C expression holding the call/cc value -- either f's normal
 * return, or the value delivered by an upward (tur_escape_resume k v). */
char *emit_cps_callcc(EmitCtx *ctx, Buf *body, const Expr *e);

/* Lower (reset BODY). When BODY can reach a base shift bound to this reset,
 * emits a dk_run-driven evaluation on the substrate and returns the C
 * expression holding the result. Otherwise returns NULL, signalling the
 * caller to fall back to the legacy lowering (plain emit_value of the body). */
char *emit_cps_reset(EmitCtx *ctx, Buf *body, const Expr *e);

/* CPS9: true iff `program` has a (cloneable-reset ...) whose body is a
 * supported delimited context around a cloneable-shift -- the gate for
 * emitting the DK machine + the cloneable<->DK bridge for cloneable conts. */
bool emit_cps_program_uses_cloneable_dk(const Expr *program);

/* CPS9: lower (cloneable-reset CTX[cloneable-shift f v]) onto the DK machine,
 * reifying the delimited context CTX as DK frames and the captured
 * sub-continuation as a multi-shot cloneable cont. Returns the C expression
 * holding the result, or NULL to fall back to the legacy lowering. */
char *emit_cps_cloneable_reset(EmitCtx *ctx, Buf *body, const Expr *e);

/* True if the program contains any serial-shift/serial-reset node, lowerable or
 * not. Gates the DK machine + serial marshaling runtime preludes so the stdlib
 * save-cont!/resume-cont! references resolve even when a context cannot lower. */
bool emit_cps_program_contains_serial(const Expr *program);

/* CPS10: lower (serial-reset CTX[serial-shift f v]) onto the DK machine,
 * reifying the delimited context as tagged DK frames and the captured
 * sub-continuation as a marshalable serial cont. Returns the C expression
 * holding the result, or NULL to fall back to the legacy lowering. */
char *emit_cps_serial_reset(EmitCtx *ctx, Buf *body, const Expr *e);

#endif
