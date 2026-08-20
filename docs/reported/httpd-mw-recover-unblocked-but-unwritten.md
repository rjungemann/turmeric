# mw-recover (panic -> 500 middleware) is unwritten and its stated blocker is gone

**Severity: low** (feature gap, blocker resolved). Found in the 2026-08-20
docs audit.

## Repro

`grep -n "defn mw-recover" stdlib/httpd.tur` -> nothing.
`tests/fixtures/panic-catch-unwind-captures` proves a capturing catch-unwind
thunk works, which was the old blocker.

## Root cause

Never written; the old EX_CATCH_UNWIND env-propagation defect that blocked it
is fixed (docs/archive/history/catch-unwind-drops-captures-segv.md).

## Fix direction

`(defn mw-recover [^fat next : int] : ptr<void> ...)` wrapping
`(httpd-call next c)` in `catch-unwind`, responding 500 on Err; add a fixture.
The same PR could take `mw-timeout`, which still needs a `with-deadline`
combinator on the async path.

## Guides to update when fixed

- docs/guides/httpd-middleware-guide.md ("Not yet shipped" section)
