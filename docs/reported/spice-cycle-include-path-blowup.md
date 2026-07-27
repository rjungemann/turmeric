# Cyclic `:spices` deps blow up the `-I` search path

**Severity:** medium -- a manifest cycle produces an unreadable `cc` error
instead of a diagnostic, and the include path grows without bound.

## Summary

Two spices that declare each other as `:path` deps make the include-path
resolver walk the cycle unboundedly, emitting one `-I <a>/../<b>` segment per
lap. The result is a multi-kilobyte `-I` argument that `cc` rejects with
`File name too long` on an unrelated system header.

## Repro

`tests/fixtures/transitive-cmake-deps-cycle/` already encodes it -- `spice-a`
depends on `spice-b` and `spice-b` on `spice-a`. The harness case
`transitive-cmake-deps-cycle-builds` in `tests/run-transitive-cmake-deps.sh`
is red:

```
$ bash tests/run-transitive-cmake-deps.sh
FAIL transitive-cmake-deps-cycle-builds -- tur build exit=2:
  ...spice-a/../spice-b/../spice-a/../spice-b/[... ~60 laps ...]/sys/select.h:
  File name too long
   10 | #include <sys/select.h>
```

Note the failure surfaces on `#include <sys/select.h>` -- a red herring; the
header is fine, the search path is not.

## Root cause

The transitive `:spices` walk that unions dep `src/` dirs into the include
path has no visited-set, so a cycle recurses until some other limit trips.
Entry points are the transitive resolution in `src/main.c:4891` and the
manifest walk described at `src/compiler/pkg.h:300`. The sibling case is
already handled -- `transitive-cmake-deps-conflict-detected` passes -- so the
conflict detector has the bookkeeping this path is missing.

## Fix directions

Thread a visited set (by canonicalized manifest path) through the transitive
`:spices` walk and stop on re-entry. Decide whether a cycle should be a hard
error (`tur.lock` cannot express one) or silently deduped; a diagnostic naming
both spices is friendlier than either current behavior. Canonicalizing each
`-I` before appending would also cap the damage independently.

## Status

Pre-existing, not introduced by the TUR-E0620 manifest-slot audit -- verified
by rebuilding at `67ce68f6c` and reproducing identically.
