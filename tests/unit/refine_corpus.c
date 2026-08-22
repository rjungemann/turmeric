/* refine_corpus.c -- replay a labelled SMT-LIB2 corpus against the in-house
 * staged decision procedure, WITHOUT Z3.
 *
 * This is the durable half of the Z3 retirement criteria
 * (docs/archive/refinement-types-plan.md, "Z3 retirement criteria").  The
 * scaffold oracle is a live cross-check that only exists on a dev build with a
 * system Z3 linked; what survives its deletion is a corpus whose labels are
 * DATA, checked into the repo and replayed by a harness that has no solver
 * dependency of its own.
 *
 * ## How a satisfiability benchmark becomes an entailment query
 *
 * SMT-LIB `:status` is a claim about the SATISFIABILITY of the assertion set.
 * The in-house chain decides ENTAILMENT -- `hyps |- goal`, internally "is
 * `hyps AND NOT goal` unsatisfiable".  The two line up exactly by taking every
 * `(assert phi)` as a hypothesis and `false` as the goal:
 *
 *     hyps |- false   is VALID   iff   hyps is UNSAT
 *
 * which gives the check its whole shape:
 *
 *   | :status | RT_VALID              | anything else |
 *   |---------|-----------------------|---------------|
 *   | unsat   | correct (a proof)     | acceptable    |
 *   | sat     | SOUNDNESS FAILURE     | correct       |
 *
 * A `sat` benchmark answered VALID is the one-directional invariant broken:
 * the chain claimed a contradiction in a set of constraints that has a model.
 * That is the property this harness exists to defend, and it needs no oracle
 * to check -- only the label.
 *
 * `unknown` labels are recorded and never fail: they carry no claim.
 *
 * ## Where the reader lives
 *
 * The SMT-LIB2 reader this harness is built on now lives in
 * src/compiler/refine_smtlib.c beside the writer (SX8a), so `tur smt` and this
 * harness share one parser and one notion of the accepted fragment.  What is
 * left here is the DRIVER: fork-per-benchmark isolation, the label check, the
 * per-benchmark time budget, and the tally.
 *
 * A benchmark using anything outside the fragment is SKIPPED WHOLE by the
 * reader, never partially parsed.  Every skip is counted and its reason
 * printed, so a corpus that stops testing anything is visible rather than
 * green.
 *
 * See tests/corpus/smtlib/README.md for the corpus itself. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lsp/lsp_sym.h"
#include "compiler/refine_smtlib.h"
#include "compiler/refine_solver.h"
#include "compiler/refine_vc.h"
#include "runtime/arena.h"

/* Stub: tur_core's lsp.c references this; this harness never touches LSP, but
 * the symbol must resolve when tur_core is linked into a standalone
 * executable.  Matches the stub in tests/unit/refine_solver.c. */
int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out) {
    (void)source_path; (void)out; (void)cap;
    if (count_out) *count_out = 0;
    return 0;
}


/* ------------------------------------------------------------------------- *
 * Runner
 * ------------------------------------------------------------------------- */

typedef RefineDecision (*Stage)(RefineVC *, Arena *);

static RefineVerdict run_chain(RefineVC *vc, Arena *a) {
    static const Stage CHAIN[] = {
        refine_s0_decide, refine_s1_decide, refine_s2_decide, refine_s3_decide,
    };
    for (size_t i = 0; i < sizeof(CHAIN) / sizeof(CHAIN[0]); i++) {
        RefineDecision d = CHAIN[i](vc, a);
        if (d.verdict != RT_UNKNOWN) return d.verdict;
    }
    return RT_UNKNOWN;
}

static int g_soundness_failures = 0;
static int g_proved = 0, g_unproved = 0, g_skipped = 0, g_unlabelled = 0;
static int g_sat_ok = 0, g_total = 0, g_over_budget = 0;

/* Seconds any single benchmark may take.  The real SMT-LIB library is not
 * small -- the 2025 QF_LIA release contains a benchmark of over a million
 * lines -- and Fourier-Motzkin blows up combinatorially, so an unbounded run
 * over external data does not terminate in any useful time.  A ctest target
 * that can hang is worse than no target.
 *
 * Exceeding the budget is SAFE in both directions, which is what makes a
 * timeout an acceptable answer rather than a hole: a soundness failure
 * requires the chain to ANSWER `RT_VALID`, and a benchmark that never
 * finished never answered.  It is exactly `RT_UNKNOWN` arrived at by a
 * different route -- correct for a `sat` label, merely incomplete for `unsat`.
 *
 * Override with TUR_CORPUS_TIMEOUT (seconds; 0 disables the budget). */
#define CORPUS_DEFAULT_TIMEOUT_S 10
static int g_budget_s = CORPUS_DEFAULT_TIMEOUT_S;

/* How a child reports which counter the parent should bump.  The child prints
 * its own human-readable line -- it owns the skip reason and the label -- and
 * the exit code carries only the classification.
 *
 * The values are DELIBERATELY not small integers.  A sanitizer-detected
 * crash (ASan stack overflow, UBSan trap) exits the child with code 1, and
 * when OUT_UNLABELLED was 1 that crash tallied as "unlabelled" -- a pass --
 * quietly defeating the "a crash is loud" design for exactly the build the
 * suite runs.  Any exit status outside this range is now classified by the
 * parent as a crash. */
enum {
    OUT_SKIPPED = 40, OUT_UNLABELLED = 41, OUT_PROVED = 42,
    OUT_UNPROVED = 43, OUT_SAT_OK = 44, OUT_SOUNDNESS = 45, OUT_ERROR = 46,
};

static char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len_out = got;
    return buf;
}

/* Per-benchmark cap telemetry, one machine-readable line, under
 * TUR_CORPUS_CAPS=1.  Aggregation lives in the sweep script rather than here
 * because each benchmark is decided in a forked CHILD -- the parent tallies
 * through exit codes, which carry a classification and nothing else, so
 * summing counters in-process would mean adding shared memory to a harness
 * whose whole isolation story is that the child owns its own crash.
 *
 * Emitted for every benchmark, not only the capped ones: the peaks of the
 * benchmarks that did NOT cap out are what say how much headroom the corpus
 * actually leaves.  See docs/upcoming/solver-extension-plan.md (SX0(b)). */
static void report_caps(const char *path) {
    const char *e = getenv("TUR_CORPUS_CAPS");
    if (!e || e[0] != '1') return;
    const RefineCapStats *c = refine_caps();
    printf("  caps    %s cubes=%u:%u cube_lits=%u:%u expand_depth=%u:%u "
           "la_vars=%u:%u la_constr=%u:%u la_fm=%u euf_terms=%u:%u "
           "no_shared=%u:%u no_rounds=%u\n",
           path,
           c->cubes_hits, c->cubes_peak,
           c->cube_lits_hits, c->cube_lits_peak,
           c->expand_depth_hits, c->expand_depth_peak,
           c->la_vars_hits, c->la_vars_peak,
           c->la_constr_hits, c->la_constr_peak,
           c->la_fm_hits,
           c->euf_terms_hits, c->euf_terms_peak,
           c->no_shared_hits, c->no_shared_peak,
           c->no_rounds_hits);
}

/* Decide one benchmark and print its line.  Runs in a CHILD process so a
 * pathological input costs one benchmark rather than the whole run. */
static int decide_one(const char *path) {
    size_t len = 0;
    char *text = slurp(path, &len);
    if (!text) { printf("  ERROR   %s (unreadable)\n", path); return OUT_ERROR; }

    Arena arena;
    arena_init(&arena, 1 << 20);
    Arena *a = &arena;
    SmtlibQuery b;
    refine_smtlib_read(&b, text, len, a);

    int outcome;
    if (b.skipped) {
        printf("  skip    %s (%s)\n", path, b.skip_reason ? b.skip_reason : "?");
        outcome = OUT_SKIPPED;
    } else if (b.status == SMT_STATUS_NONE || b.status == SMT_STATUS_UNKNOWN) {
        printf("  unlab   %s (no :status claim)\n", path);
        outcome = OUT_UNLABELLED;
    } else {
        refine_caps_reset();
        RefineVerdict v = run_chain(b.vc, a);
        report_caps(path);
        if (b.status == SMT_STATUS_SAT) {
            /* The invariant: a satisfiable assertion set must never be proved
             * contradictory.  RT_UNKNOWN and RT_INVALID are both correct. */
            if (v == RT_VALID) {
                printf("  SOUND!  %s -- labelled sat, chain answered VALID "
                       "(claimed the constraints are contradictory)\n", path);
                outcome = OUT_SOUNDNESS;
            } else {
                printf("  ok      %s (sat, not proved -- correct)\n", path);
                outcome = OUT_SAT_OK;
            }
        } else if (v == RT_VALID) {
            printf("  ok      %s (unsat, proved)\n", path);
            outcome = OUT_PROVED;
        } else {
            printf("  weak    %s (unsat, not proved -- incomplete, not unsound)\n",
                   path);
            outcome = OUT_UNPROVED;
        }
    }
    arena_free(a);
    free(text);
    return outcome;
}

static void tally(int outcome) {
    switch (outcome) {
        case OUT_SKIPPED:    g_skipped++;            break;
        case OUT_UNLABELLED: g_unlabelled++;         break;
        case OUT_PROVED:     g_proved++;             break;
        case OUT_UNPROVED:   g_unproved++;           break;
        case OUT_SAT_OK:     g_sat_ok++;             break;
        case OUT_SOUNDNESS:  g_soundness_failures++; break;
        default:             g_soundness_failures++; break;   /* ERROR */
    }
}

static void run_one(const char *path) {
    g_total++;

    if (g_budget_s <= 0) {                 /* budget disabled: run in-process */
        tally(decide_one(path));
        fflush(stdout);
        return;
    }

    fflush(stdout);                        /* never duplicate buffered output */
    pid_t pid = fork();
    if (pid < 0) {                         /* cannot fork: fall back in-process */
        tally(decide_one(path));
        fflush(stdout);
        return;
    }
    if (pid == 0) {
        int outcome = decide_one(path);
        fflush(stdout);
        _exit(outcome);
    }

    /* Poll rather than alarm(): the parent must stay responsive and must not
     * take a signal in the middle of its own bookkeeping. */
    const long step_ns = 20L * 1000 * 1000;         /* 20ms */
    long waited_ns = 0;
    const long budget_ns = (long)g_budget_s * 1000L * 1000 * 1000;
    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) >= OUT_SKIPPED &&
                WEXITSTATUS(status) <= OUT_ERROR) {
                tally(WEXITSTATUS(status));
            } else if (WIFEXITED(status)) {
                /* An exit code the child never sends deliberately -- the
                 * sanitizer runtime aborting (ASan exits 1), a library
                 * calling exit(), anything unexpected.  A crash on this
                 * input, exactly like a signal death. */
                printf("  CRASH!  %s (child exited with unexpected code %d)\n",
                       path, WEXITSTATUS(status));
                g_soundness_failures++;
            } else {
                /* Killed by a signal it did not ask for -- a crash on this
                 * input.  That is a defect worth failing on, not a skip. */
                printf("  CRASH!  %s (child died on signal %d)\n",
                       path, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                g_soundness_failures++;
            }
            fflush(stdout);
            return;
        }
        if (r < 0 && errno != EINTR) {
            printf("  ERROR   %s (waitpid: %s)\n", path, strerror(errno));
            g_soundness_failures++;
            fflush(stdout);
            return;
        }
        if (waited_ns >= budget_ns) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            g_over_budget++;
            printf("  budget  %s (exceeded %ds -- counts as undecided)\n",
                   path, g_budget_s);
            fflush(stdout);
            return;
        }
        struct timespec ts = { 0, step_ns };
        nanosleep(&ts, NULL);
        waited_ns += step_ns;
    }
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Recurses, so the corpus can separate hand-written benchmarks from generated
 * ones without the runner needing to know the layout.  Sorted at every level so
 * the report is stable across filesystems. */
static void run_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        printf("refine_corpus: no corpus directory at %s\n", dir);
        return;
    }
    char *files[8192]; int nf = 0;
    char *subdirs[256];  int nd = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;          /* . .. and dotfiles */
        size_t l = strlen(e->d_name);
        char *full = (char *)malloc(strlen(dir) + l + 2);
        sprintf(full, "%s/%s", dir, e->d_name);
        if (l > 5 && strcmp(e->d_name + l - 5, ".smt2") == 0) {
            if (nf < 8192) files[nf++] = full; else free(full);
            continue;
        }
        DIR *probe = opendir(full);
        if (probe) {
            closedir(probe);
            if (nd < 256) subdirs[nd++] = full; else free(full);
        } else {
            free(full);                              /* README.md and friends */
        }
    }
    closedir(d);
    qsort(files, (size_t)nf, sizeof(char *), cmp_str);
    qsort(subdirs, (size_t)nd, sizeof(char *), cmp_str);
    for (int i = 0; i < nf; i++) { run_one(files[i]); free(files[i]); }
    for (int i = 0; i < nd; i++) { run_dir(subdirs[i]); free(subdirs[i]); }
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/corpus/smtlib";
    const char *budget = getenv("TUR_CORPUS_TIMEOUT");
    if (budget && *budget) g_budget_s = atoi(budget);
    printf("refine_corpus: replaying %s against the in-house chain (no Z3)\n", dir);
    if (g_budget_s > 0)
        printf("  per-benchmark budget: %ds (TUR_CORPUS_TIMEOUT to change, "
               "0 to disable)\n", g_budget_s);
    run_dir(dir);

    printf("\n  benchmarks              : %d\n", g_total);
    printf("  unsat, proved           : %d\n", g_proved);
    printf("  unsat, not proved       : %d  (incomplete, allowed)\n", g_unproved);
    printf("  sat, correctly not proved: %d\n", g_sat_ok);
    printf("  skipped (outside fragment): %d\n", g_skipped);
    printf("  over budget (%ds)        : %d  (undecided, allowed)\n",
           g_budget_s, g_over_budget);
    printf("  unlabelled              : %d\n", g_unlabelled);
    printf("  SOUNDNESS FAILURES      : %d\n", g_soundness_failures);

    if (g_soundness_failures) {
        printf("\nrefine_corpus: FAIL -- the chain proved a satisfiable "
               "benchmark contradictory\n");
        return 1;
    }
    /* A corpus that decides nothing is not a regression net.  Guard against
     * silently losing the corpus (an empty directory, a reader that skips
     * everything) rather than reporting green for work not done. */
    if (g_total == 0) {
        printf("\nrefine_corpus: FAIL -- no benchmarks found\n");
        return 1;
    }
    if (g_proved + g_sat_ok == 0) {
        printf("\nrefine_corpus: FAIL -- no benchmark was actually decided\n");
        return 1;
    }
    printf("\nrefine_corpus: PASS\n");
    return 0;
}
