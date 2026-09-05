# `tur fetch` has one exit code for "an optional dep failed" and "a required dep failed"

**Severity: low** (ergonomics/CI diagnosability, not correctness). Split out
2026-09-04 from
[spices-ci-fetch-failure-downgraded-to-warning](../archive/spices-ci-fetch-failure-downgraded-to-warning.md)
(now archived) when only that report's item (1) -- capturing and re-printing
`tur fetch`'s output in the CI warning annotation -- was fixed. This is items
(2)/(3) from that report: the durable fix, still open.

## Problem

`turmeric-spices`'s CI treats a nonzero `tur fetch --update` exit as
non-fatal, because a spice's `:optional` `:spices` entries are *expected* to
fail sometimes and the job should stay green. But `tur fetch` returns the
same nonzero code whether the failure was an optional dep (fine, ignore it)
or a required `:cmake-deps` native build (a real problem that will blow up
two steps later as an unrelated link error). The workflow has to guess, so it
always guesses "ignore," and required-dep breakage goes undiagnosed until it
resurfaces downstream.

## Fix direction

1. **Distinguish the two cases in `tur fetch`'s exit code** (e.g. `0` = clean,
   `1` = only optional deps failed, `2` = a required dep failed). This is the
   real fix and lives in `turmeric`'s fetch/pkg machinery, not in CI YAML.
2. Once that exists, `turmeric-spices/.github/workflows/ci.yml`'s fetch step
   can `exit 1` on a required-dep failure and warn-and-continue on an
   optional-only one, so Linux stays green and macOS required-dep breakage
   actually fails the job instead of surfacing later as a link error.

Cheaper interim option scoped entirely to the workflow: after `tur fetch`,
check that every non-`:optional` `:cmake-deps` entry produced its expected
build artifact, and fail naming the dep if not. Not attempted here because
`:cmake-deps` entries don't consistently mark themselves `:optional` even
when the C code gates on their availability (e.g. `ws-client`'s `mbedtls` dep
is functionally optional -- `__has_include` gated -- but carries no
`:optional` field), so a blanket "every non-optional :cmake-deps must produce
an artifact" check would incorrectly fail spices relying on that gating. See
[cmake-deps-cannot-express-framework](../archive/cmake-deps-cannot-express-framework.md)
and
[cmake-deps-link-name-not-overridable](../archive/cmake-deps-link-name-not-overridable.md)
for the related manifest-expressiveness gaps.
