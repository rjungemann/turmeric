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

/* Emit the self-contained DK multi-prompt machine (a faithful C port of
 * src/runtime/cps_prompt.c) into the generated program's preamble. Call once,
 * gated on emit_cps_program_uses_delimited(). */
void emit_cps_runtime_prelude(Buf *out);

/* call-cc-completion: true iff `program` uses (call/cc f) or (escape f)
 * (EX_CALLCC) -- the gate for emitting the escape-continuation runtime. */
bool emit_cps_program_uses_callcc(const Expr *program);

/* Emit the undelimited escape-continuation runtime (tur_escape_cont +
 * tur_escape_resume) into the generated preamble. Call once, gated on
 * emit_cps_program_uses_callcc(). */
void emit_cps_callcc_prelude(Buf *out);

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

#endif
