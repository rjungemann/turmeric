#ifndef TUR_CPS_PROMPT_H
#define TUR_CPS_PROMPT_H

#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * CPS5 (cps-transform-plan): multi-prompt delimited-control machine.
 *
 * The Dybvig--Peyton-Jones--Sabry model, expressed over heap continuation
 * chains (a CPS program is a chain; this is the same shape CPS4's TurKont uses,
 * specialized here with prompt/shift markers so the machine can capture
 * sub-continuations by *slicing the chain*):
 *
 *   - reset = a prompt marker in the chain (DK_PROMPT, tag t).
 *   - shift(t, body) / shift0(t, body) capture the sub-continuation from the
 *     shift point up to the nearest enclosing prompt tagged t, reify it as a
 *     callable chain, and run body with it. shift RE-INSTALLS the prompt on the
 *     captured sub (so resuming re-delimits); shift0 does NOT (CPS0.2 table).
 *   - the captured sub is multi-shot: dk_invoke runs a fresh copy each time, so
 *     call/cc* / cloneable resume is just "invoke more than once".
 *   - an implicit ROOT prompt (DK_ROOT_TAG) lets an undelimited capture reach
 *     program entry (CPS5.3): dk_run_root treats program end as the prompt.
 *
 * Standalone machine (not yet wired into codegen). Exercised by
 * tests/cps_prompt_unit.c.
 * ========================================================================= */

#define DK_ROOT_TAG 0   /* the implicit root prompt's tag */

typedef struct DK DK;

/* A plain frame: given the value flowing in, produce the value flowing out. */
typedef intptr_t (*DKFrame)(intptr_t env, intptr_t value);

/* A shift/shift0 body: receives the captured sub-continuation (invoke it with
 * dk_invoke, possibly multiple times) and returns the value delivered to the
 * prompt's outer continuation. */
typedef intptr_t (*DKBody)(intptr_t env, DK *subk);

/* ---- chain constructors (each returns a fresh heap node) -------------- */
DK *dk_done(void);
DK *dk_frame(DKFrame fn, intptr_t env, DK *next);
DK *dk_prompt(int tag, DK *next);
DK *dk_shift(int tag, DKBody body, intptr_t body_env, DK *next);
DK *dk_shift0(int tag, DKBody body, intptr_t body_env, DK *next);

/* ---- evaluation ------------------------------------------------------- */
/* Run chain `k` with seed value `v` (no implicit root prompt). */
intptr_t dk_run(DK *k, intptr_t v);
/* Run under an implicit root prompt: a shift to DK_ROOT_TAG with no explicit
 * enclosing prompt captures up to program entry. */
intptr_t dk_run_root(DK *k, intptr_t v);

/* Invoke a captured sub-continuation with `w` (multi-shot: copies internally). */
intptr_t dk_invoke(DK *sub, intptr_t w);

/* Does the chain contain a prompt marker? (used to assert re-install). */
bool dk_has_prompt(const DK *k);

/* ---- memory ----------------------------------------------------------- */
DK *dk_copy(const DK *k);   /* deep copy a chain */
void dk_free(DK *k);        /* free a chain */

#endif
