# schan-recv still delivers via a caller-provided cell although its blocker is fixed

**Severity: low** (cleanup/expressiveness). Found in the 2026-08-20 docs
audit.

## Repro

stdlib/schan.tur:149 --
`(defn schan-recv [T R] [c : (SChan (SRecv T R)) cell : ptr<void>] : (SChan R))`.

## Root cause

Workaround for the generic-struct-opaque-element miscompile, both variants of
which are FIXED as of 2026-06-05
(docs/archive/history/generic-struct-opaque-element-miscompile.md; pinned by
tests/fixtures/generic-relay-aggregate-result).

## Fix direction

Migrate to returning `(Pair T (SChan R))`, retire
`schan-cell-new/get/free`, update the `schan-roundtrip`/`schan-worker-pool`
fixtures.

## Guides to update when fixed

- docs/guides/session-types-guide.md
