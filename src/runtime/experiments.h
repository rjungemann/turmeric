#ifndef TUR_EXPERIMENTS_H
#define TUR_EXPERIMENTS_H
/* experiments.h -- the experimental-feature-flag registry.
 *
 * Successor to the retired `-X<name>` surface (drop-x-flags-plan).  A
 * genuinely-in-flight compiler feature opts in with `--enable=<name>` on the
 * CLI or `:experiments [...]` in build.tur; every such feature is declared
 * once in the EXPERIMENTS[] table in experiments.c with all of its metadata
 * filled in, including a hard `expires_at` the release-cut script enforces.
 *
 * See docs/upcoming/v1/experimental-flag-mechanism-plan.md and
 * docs/guides/experimental-flags-guide.md. */
#include <stdbool.h>
#include <stddef.h>

/* Where in its lifecycle an experiment sits.  Drives which warning code the
 * use-site emits (TUR-W0060 vs TUR-W0061). */
typedef enum ExperimentLifecycle {
    XF_LIFECYCLE_PROTOTYPE,  /* core algorithm/surface still in flux */
    XF_LIFECYCLE_BETA,       /* surface frozen, soaking before graduation */
} ExperimentLifecycle;

/* One row of the registry.  Every field is mandatory -- no flag may be added
 * without all of them populated (the release-cut enforcement keys on
 * expires_at, the docs site on the rest). */
typedef struct ExperimentDescriptor {
    const char          *name;        /* kebab-case, no leading '-' */
    const char          *summary;     /* one-line, shown in `tur experiments` */
    const char          *plan_path;   /* docs/upcoming/... */
    const char          *introduced;  /* version string, e.g. "0.25.0" */
    const char          *expires_at;  /* version string, e.g. "0.28.0" */
    ExperimentLifecycle  lifecycle;
    bool                *opt_global;   /* points at g_opt_<name> (the enable bit) */
} ExperimentDescriptor;

/* --- Table iteration (for `tur experiments` and release-cut enforcement) --- */
size_t                      experiment_count(void);
const ExperimentDescriptor *experiment_at(size_t i);
const ExperimentDescriptor *experiment_lookup(const char *name);

/* How an experiment came to be enabled, for the `tur experiments` source
 * column ("CLI wins on conflict"). */
typedef enum ExperimentSource {
    XF_SRC_NONE = 0,
    XF_SRC_MANIFEST,
    XF_SRC_CLI,
} ExperimentSource;

/* Turn an experiment on.  Returns false if `name` is not a known experiment
 * (the caller then emits TUR-E0310).  `src` records the origin; CLI overrides
 * a prior manifest enable but not vice-versa. */
bool experiment_enable(const char *name, ExperimentSource src);

/* True iff the named experiment is known and currently enabled. */
bool experiment_is_enabled(const char *name);

/* The source that enabled experiment index `i` (XF_SRC_NONE if disabled). */
ExperimentSource experiment_source_at(size_t i);

/* Emit the lifecycle warning (TUR-W0060 prototype / TUR-W0061 beta) for the
 * named experiment, at most once per compile.  No-op if the name is unknown
 * or the experiment is not enabled.  Each gated feature calls this from its
 * own elaboration entry point. */
void experiment_warn_if_used(const char *name);

/* Reset the once-per-compile warning dedup state.  Called at the start of
 * each compile (alongside diag_reset). */
void experiment_reset_warnings(void);

#endif /* TUR_EXPERIMENTS_H */
