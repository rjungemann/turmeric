/* experiments.c -- the experimental-feature-flag registry.
 *
 * Successor to the retired `-X<name>` surface.  See experiments.h and
 * docs/upcoming/v1/experimental-flag-mechanism-plan.md.
 *
 * The EXPERIMENTS[] table below is the single source of truth: `tur
 * experiments`, `tur --help`, the docs site, and the release-cut script all
 * read it, and nothing restates the list.  It is intentionally EMPTY -- no
 * feature ships behind this mechanism until a concrete experiment requests a
 * gate.  When one does, add a row with all seven fields populated and point
 * `opt_global` at a `g_opt_<name>` bool the feature's elaboration reads.
 *
 * Shape of a future entry (do not uncomment -- illustrative only):
 *
 *   static const ExperimentDescriptor EXPERIMENTS[] = {
 *     { "fancy-rows",
 *       "extensible row-typed records",
 *       "docs/upcoming/v1/fancy-rows-plan.md",
 *       "0.25.0",                  // introduced
 *       "0.28.0",                  // expires_at (hard contract; release-cut enforced)
 *       XF_LIFECYCLE_PROTOTYPE,
 *       &g_opt_fancy_rows },
 *   };
 */
#include "experiments.h"

#include <stdio.h>
#include <string.h>

/* The registry.  Empty by design (see file header). */
static const ExperimentDescriptor EXPERIMENTS[] = {
    /* populated as features land */
    { 0 }, /* sentinel so the array is never zero-length (C forbids that);
            * experiment_count() subtracts it off. */
};

/* Number of real entries (excluding the trailing { 0 } sentinel). */
size_t experiment_count(void) {
    return (sizeof(EXPERIMENTS) / sizeof(EXPERIMENTS[0])) - 1;
}

const ExperimentDescriptor *experiment_at(size_t i) {
    if (i >= experiment_count()) return NULL;
    return &EXPERIMENTS[i];
}

const ExperimentDescriptor *experiment_lookup(const char *name) {
    if (!name) return NULL;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return &EXPERIMENTS[i];
    }
    return NULL;
}

/* Per-index enable source + once-per-compile warning dedup.  Sized to a fixed
 * cap that comfortably exceeds any sane table size (the mechanism is designed
 * so no more than ~2 flags live here at once). */
#define XF_MAX 64
static ExperimentSource g_src[XF_MAX];   /* XF_SRC_NONE = disabled */
static bool             g_warned[XF_MAX]; /* TUR-W006x emitted this compile */

static long experiment_index(const char *name) {
    if (!name) return -1;
    size_t n = experiment_count();
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EXPERIMENTS[i].name, name) == 0) return (long)i;
    }
    return -1;
}

bool experiment_enable(const char *name, ExperimentSource src) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return false;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->opt_global) *d->opt_global = true;
    /* CLI wins on conflict: a CLI enable overrides a prior manifest enable,
     * but a manifest enable does not downgrade an existing CLI enable. */
    if (src == XF_SRC_CLI || g_src[idx] == XF_SRC_NONE) g_src[idx] = src;
    return true;
}

bool experiment_is_enabled(const char *name) {
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return false;
    return g_src[idx] != XF_SRC_NONE;
}

ExperimentSource experiment_source_at(size_t i) {
    if (i >= experiment_count() || i >= XF_MAX) return XF_SRC_NONE;
    return g_src[i];
}

/* --allow-experimental: suppress TUR-W006x.  Intended for the Turmeric
 * project's own CI matrix; spice users should not set this.  Defined in
 * globals.c, declared in globals.h. */
extern bool g_allow_experimental;

void experiment_warn_if_used(const char *name) {
    if (g_allow_experimental) return;
    long idx = experiment_index(name);
    if (idx < 0 || idx >= XF_MAX) return;
    if (g_src[idx] == XF_SRC_NONE) return;   /* not enabled -> nothing to warn */
    if (g_warned[idx]) return;               /* once per compile */
    g_warned[idx] = true;
    const ExperimentDescriptor *d = &EXPERIMENTS[idx];
    if (d->lifecycle == XF_LIFECYCLE_BETA) {
        fprintf(stderr,
                "warning [TUR-W0061]: experimental feature '%s' (beta) -- "
                "graduates in %s; see %s\n",
                d->name, d->expires_at, d->plan_path);
    } else {
        fprintf(stderr,
                "warning [TUR-W0060]: experimental feature '%s' (prototype) -- "
                "breaking changes likely; see %s\n",
                d->name, d->plan_path);
    }
}

void experiment_reset_warnings(void) {
    memset(g_warned, 0, sizeof(g_warned));
}
