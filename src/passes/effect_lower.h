#ifndef TUR_EFFECT_LOWER_H
#define TUR_EFFECT_LOWER_H

#include "arena.h"
#include "symbols.h"
#include "expr.h"
#include "effect.h"

/* Lower algebraic effect expressions to delimited continuations.
 * This pass transforms:
 * - (perform (E args...)) -> (shift k (dispatch E args k))
 * - (handle expr cases...) -> (reset (let [handlers...] expr))
 */
Expr *effect_lower(Arena *a, SymbolTable *st, Expr *program, EffectEnv *effect_env);

#endif /* TUR_EFFECT_LOWER_H */
