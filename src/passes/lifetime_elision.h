#ifndef TUR_LIFETIME_ELISION_H
#define TUR_LIFETIME_ELISION_H

#include "lifetimes.h"
#include "types.h"

/* Lifetime elision rules (Rust-style) */

/* Rule 1: Each lifetime in an input type becomes a distinct lifetime parameter.
 * Rule 2: If there's exactly one input lifetime, assign it to all output lifetimes.
 * Rule 3: For method calls with &self or &mut self, use &self's lifetime. */

/* Collect lifetime parameters from function signature and apply elision rules.
 * Returns: number of lifetime parameters assigned */
uint8_t lifetime_elision_apply(LifetimeContext *ctx,
                              Type *param_types, uint8_t n_params,
                              Type *return_type);

/* Check if a type contains lifetime annotations */
bool type_has_any_lifetime(const Type *t);

/* Collect all unique lifetimes from a type */
void type_collect_lifetimes(const Type *t, LifetimeId *out, uint8_t *n_out, uint8_t max_out);

#endif
