# XF5 of the experimental-flag plan names an already-shipped feature

**Status:** RESOLVED. The plan has been archived
(`docs/archive/experimental-flag-mechanism-plan.md`) with a deferral note at
the top explicitly acknowledging that XF5's named consumer
(cross-parameter sized-type unification) is already default-on and that XF5
will not be executed against it. The mechanism (XF0--XF4, XF6) shipped with
an empty `EXPERIMENTS[]` registry (`src/runtime/experiments.c`), exactly as
the fix-directions section recommended; no fixtures changed.

**Summary:** `docs/upcoming/v1/experimental-flag-mechanism-plan.md` Phase XF5
("First real consumer") proposes migrating *cross-parameter sized-type
unification* onto `--enable=sized-cross-param` as the first occupant of the
experiment registry, calling it "the one open gap." That gap is closed --
the feature shipped and is default-on -- so XF5 cannot be executed as
written. **Severity: low** (documentation/plan defect; the rest of the plan,
XF0--XF4 and XF6, is sound and was implemented).

## Repro / evidence

- The plan's XF5 cites `project_sized_types_phase.md` for the open gap. That
  doc does not exist in the tree or on `origin/main` (`git show
  origin/main:docs/upcoming/v1/project_sized_types_phase.md` -> "does not
  exist"). It was an `.claude` auto-memory entry that predates the fix.
- The cross-parameter unification work is **resolved and archived**:
  `docs/archive/history/sized-types-cross-param-unification-plan.md` is marked
  `Status: RESOLVED (archived)` -- S1 (cross-parameter unification), S2
  (wrapper/projection propagation), and S3 (index-preserving helpers) all
  implemented; the elaborator emits `TUR-E0260` on cross-parameter mismatch.
- Fixtures already depend on the feature being on by default:
  `tests/fixtures/sized-cross-param-accept`, `sized-matrix-cross-param-accept`,
  `tests/fixtures/errors/sized-cross-param-reject`,
  `sized-matrix-cross-param-reject`, plus `tests/fixtures/sized-size-arith`.

## Root cause

XF5 was drafted against a stale "open gap" memory. Gating the feature now
behind `--enable=` would be a regression: it would flip default-on behavior
off and break the `sized-cross-param-*` / `sized-matrix-cross-param-*`
fixtures, directly contradicting the plan's own verification item ("this
mechanism does not gate any existing fixture").

## Fix directions

- The mechanism (XF0--XF4, XF6) was implemented with an **empty**
  `EXPERIMENTS[]` table, honoring the plan header's governing constraint
  ("Design only -- no flags ship until a concrete experimental feature
  requests one"). No fixtures change.
- When a genuinely in-flight feature does want a gate, it becomes the first
  real registry entry -- a natural candidate is **size-index arithmetic**
  (`+`/`*` on size indices, for `concat`-typed `SizedVec`), explicitly left
  out of scope by the cross-param plan and not yet built.
- XF5 in the plan should be re-pointed (or struck) accordingly; the "prove
  the mechanism end-to-end on real work" intent stands, just not against
  cross-parameter unification.
