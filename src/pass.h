/* pass.h — Compiler pass scheduling infrastructure (Phase P19-1).
 *
 * Defines the ordered pass pipeline and the PassContext struct that threads
 * shared state between passes.  Each new pass is added here by:
 *   1. Adding a PassKind constant.
 *   2. Adding a case in the run_core_passes() switch in main.c.
 *
 * Current pipeline order:
 *   PASS_ELABORATE → PASS_KIND_CHECK → PASS_EFFECT_LOWER → PASS_EFFECT_ROW_INFER →
 *   PASS_CPS → PASS_BORROW_CHECK
 *
 * The emit step is NOT a PassKind because the three emit variants
 * (compile, header, implementation) take different output Buf* arguments
 * and are dispatched directly by each compile_to_* caller after
 * run_core_passes() succeeds.
 */

#ifndef TUR_PASS_H
#define TUR_PASS_H

#include <stdint.h>

#include "arena.h"
#include "effect.h"
#include "expr.h"
#include "forms.h"
#include "symbols.h"

/* Compiler pass identifiers.  Ordered to match the pipeline sequence used in
 * run_core_passes().  New passes are inserted here and in the switch. */
typedef enum PassKind {
    PASS_ELABORATE,        /* Form* → typed Expr (elaboration + type checking)      */
    PASS_KIND_CHECK,       /* kind inference and validation (Phase HKT H0)          */
    PASS_EFFECT_LOWER,     /* perform/handle → shift/reset (effect lowering)         */
    PASS_EFFECT_ROW_INFER, /* infer and validate effect rows per function (P19-2)    */
    PASS_CPS,              /* continuation-passing-style transform for shift/reset   */
    PASS_BORROW_CHECK,     /* ownership, move, and borrow analysis (Phase 14)        */
} PassKind;

/* PassContext carries all shared state for one compilation unit through the
 * compiler pass pipeline.  Callers initialise this before the first pass;
 * each pass reads and/or updates prog.
 *
 * Fields:
 *   arena       — bump allocator whose lifetime spans the full pipeline.
 *   st          — symbol table; populated by the reader, read by later passes.
 *   effect_env  — effect registry; populated by PASS_EFFECT_LOWER, carried
 *                 forward so PASS_EFFECT_ROW_INFER and codegen can query it.
 *                 NULL until PASS_EFFECT_LOWER runs.
 *   prog        — current program AST; set by PASS_ELABORATE and updated in
 *                 place by each subsequent transform pass.
 *   forms       — raw input forms fed to PASS_ELABORATE; unused afterwards.
 *   nforms      — number of entries in forms[].
 */
typedef struct PassContext {
    Arena       *arena;
    SymbolTable *st;
    EffectEnv   *effect_env;  /* NULL until PASS_EFFECT_LOWER completes */
    Expr        *prog;        /* NULL until PASS_ELABORATE completes     */
    Form       **forms;       /* input to PASS_ELABORATE                 */
    uint32_t     nforms;
    uint32_t     stdlib_prefix; /* number of leading forms from stdlib   */
} PassContext;

#endif /* TUR_PASS_H */
