/* refine_fuzz.c -- differential fuzzing of the in-house refinement solver
 * against Z3.
 *
 * This is the harness the plan's Z3 retirement criteria call for.  It exists
 * because fixture coverage is not differential coverage: compiling the entire
 * fixture corpus with `refined` forced on exercises only a couple of dozen
 * obligations, because almost no fixture contains a `#refine`.  Randomly
 * generated VCs are how the solver actually gets hammered.
 *
 * The property under test is the SOUNDNESS INVARIANT, and it is one-directional
 * in each solver's own terms:
 *
 *   - in-house VALID + Z3 INVALID  -> a soundness bug in the in-house chain.
 *     This is the failure that matters; a proof that is not a proof is the one
 *     thing the whole design forbids.
 *   - in-house INVALID + Z3 VALID  -> a soundness bug in the counterexample
 *     search: it claims to have evaluated a real witness.
 *   - either UNKNOWN                -> fine.  Incompleteness is expected and
 *     costs only a runtime check.
 *
 * Builds only under TUR_REFINE_Z3_ORACLE (a dev-only option that Release and
 * WASM builds refuse); without it this compiles to a stub that reports skip.
 *
 * Determinism: a fixed default seed so a failure reproduces.  Override with
 * TUR_FUZZ_SEED / TUR_FUZZ_ITERS.
 *
 * Throughput is about 13 VCs/second in a Debug (ASan) oracle build, dominated
 * by building a fresh Z3 context per query.  That freshness is not negotiable
 * -- sharing one context is the bug that made the oracle answer VALID to
 * everything (see refine_libz3.c) -- so the default iteration count is sized
 * for a few minutes rather than for exhaustiveness.  Turn it up for a soak. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lsp/lsp_sym.h"

int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}

#ifndef TUR_REFINE_Z3_ORACLE

int main(void) {
    printf("refine_fuzz: skipped (built without -DTUR_REFINE_Z3_ORACLE)\n");
    return 0;
}

#else

#include "compiler/refine_solver.h"
#include "compiler/refine_smtlib.h"
#include "runtime/arena.h"

/* --------------------------------------------------------------------- */
/* Deterministic PRNG (xorshift64*) -- reproducing a failure matters more
 * than statistical quality. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}
static uint32_t rnd_below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

/* --------------------------------------------------------------------- */

#define FUZZ_MAX_VARS 3

typedef struct {
    RefineVC *vc;
    uint32_t  n_vars;
    bool      use_ufunc;
    uint32_t  uf;
} Gen;

/* A random arithmetic term: variables, small literals, +, -, and
 * multiplication by a literal (the linear fragment), plus -- when this VC is
 * an abstracted one -- an uninterpreted application. */
static VCTerm *gen_term(Gen *g, uint32_t depth) {
    uint32_t pick = rnd_below(depth ? 10 : 6);
    switch (pick) {
        case 0: case 1:
            return vc_int(g->vc, (int64_t)rnd_below(11) - 5);
        case 2: case 3: case 4:
            return vc_var_ref(g->vc, rnd_below(g->n_vars));
        case 5:
            if (g->use_ufunc) {
                VCTerm *args[1] = { vc_var_ref(g->vc, rnd_below(g->n_vars)) };
                return vc_app(g->vc, g->uf, args, 1);
            }
            return vc_var_ref(g->vc, rnd_below(g->n_vars));
        case 6:
            return vc_mk2(g->vc, VC_ADD, gen_term(g, depth - 1), gen_term(g, depth - 1));
        case 7:
            return vc_mk2(g->vc, VC_SUB, gen_term(g, depth - 1), gen_term(g, depth - 1));
        case 8:
            return vc_mk2(g->vc, VC_MUL, gen_term(g, depth - 1),
                          vc_int(g->vc, (int64_t)rnd_below(5) - 2));
        default:
            return vc_mk1(g->vc, VC_NEG, gen_term(g, depth - 1));
    }
}

static VCTerm *gen_atom(Gen *g) {
    VCOp ops[3] = { VC_LT, VC_LE, VC_EQ };
    VCTerm *a = gen_term(g, 2), *b = gen_term(g, 2);
    VCTerm *at = vc_mk2(g->vc, ops[rnd_below(3)], a, b);
    if (rnd_below(4) == 0) at = vc_not(g->vc, at);
    return at;
}

static VCTerm *gen_formula(Gen *g) {
    uint32_t pick = rnd_below(8);
    if (pick < 5) return gen_atom(g);
    if (pick < 7) return vc_mk2(g->vc, VC_AND, gen_atom(g), gen_atom(g));
    return vc_mk2(g->vc, VC_OR, gen_atom(g), gen_atom(g));
}

/* --------------------------------------------------------------------- */

static RefineVerdict inhouse(RefineVC *vc, Arena *a) {
    RefineBackend chain[] = { refine_s0_decide, refine_s1_decide,
                              refine_s2_decide, refine_s3_decide };
    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++) {
        RefineDecision d = chain[i](vc, a);
        if (d.verdict != RT_UNKNOWN) return d.verdict;
    }
    /* Mirror the discharge pass: an unproven obligation is then given to the
     * bounded counterexample search, which is the only thing that may answer
     * INVALID. */
    if (refine_model_search(vc, a)) return RT_INVALID;
    return RT_UNKNOWN;
}

static void dump_vc(const RefineVC *vc) {
    Buf b; buf_init(&b);
    refine_smtlib_emit(vc, &b);
    buf_putc(&b, '\0');
    fprintf(stderr, "%s\n", b.data ? b.data : "<empty>");
    buf_free(&b);
}

int main(void) {
    const char *seed_s  = getenv("TUR_FUZZ_SEED");
    const char *iters_s = getenv("TUR_FUZZ_ITERS");
    if (seed_s && *seed_s) rng_state = strtoull(seed_s, NULL, 10) | 1ull;
    uint32_t iters = iters_s && *iters_s ? (uint32_t)strtoul(iters_s, NULL, 10) : 4000;

    uint32_t agree_valid = 0, inhouse_only_unknown = 0, both_unknown = 0;
    uint32_t agree_invalid = 0, soundness_bugs = 0, refute_bugs = 0;

    for (uint32_t it = 0; it < iters; it++) {
        uint64_t seed_here = rng_state;
        Arena a; arena_init(&a, 1 << 20);

        Gen g;
        g.vc = vc_new(&a);
        g.n_vars = 1 + rnd_below(FUZZ_MAX_VARS);
        /* Every fourth VC carries an uninterpreted symbol, so the EUF stage
         * and the abstraction rules get exercised too. */
        g.use_ufunc = (rnd_below(4) == 0);
        static const char *const NAMES[FUZZ_MAX_VARS] = { "x", "y", "z" };
        for (uint32_t v = 0; v < g.n_vars; v++)
            vc_declare_var(g.vc, NAMES[v], VS_INT);
        g.uf = g.use_ufunc ? vc_declare_ufunc(g.vc, "f", 1, VS_INT, NULL, false) : 0;

        uint32_t n_hyps = rnd_below(3);
        for (uint32_t h = 0; h < n_hyps; h++) vc_add_hyp(g.vc, gen_formula(&g));
        vc_set_goal(g.vc, gen_formula(&g));

        RefineVerdict mine = inhouse(g.vc, &a);
        RefineVerdict z3v  = refine_z3_decide(g.vc, &a).verdict;

        if (mine == RT_VALID && z3v == RT_INVALID) {
            soundness_bugs++;
            fprintf(stderr,
                    "\nSOUNDNESS BUG (seed %llu): in-house says VALID, Z3 says INVALID\n",
                    (unsigned long long)seed_here);
            dump_vc(g.vc);
        } else if (mine == RT_INVALID && z3v == RT_VALID) {
            refute_bugs++;
            fprintf(stderr,
                    "\nREFUTATION BUG (seed %llu): in-house says INVALID, Z3 says VALID\n",
                    (unsigned long long)seed_here);
            dump_vc(g.vc);
        } else if (mine == RT_VALID && z3v == RT_VALID) {
            agree_valid++;
        } else if (mine == RT_INVALID && z3v == RT_INVALID) {
            agree_invalid++;
        } else if (mine == RT_UNKNOWN && z3v != RT_UNKNOWN) {
            inhouse_only_unknown++;
        } else {
            both_unknown++;
        }

        arena_free(&a);
    }

    printf("refine_fuzz: %u VCs\n", iters);
    printf("  agree valid            : %u\n", agree_valid);
    printf("  agree invalid          : %u\n", agree_invalid);
    printf("  in-house unknown, Z3 decided : %u   (incompleteness -> runtime check)\n",
           inhouse_only_unknown);
    printf("  neither decided        : %u\n", both_unknown);
    printf("  SOUNDNESS bugs         : %u\n", soundness_bugs);
    printf("  REFUTATION bugs        : %u\n", refute_bugs);
    return (soundness_bugs || refute_bugs) ? 1 : 0;
}

#endif /* TUR_REFINE_Z3_ORACLE */
