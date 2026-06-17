## sleep-ms (and the rest of `stdlib/time.tur`) is not in the auto-load list

**Status:** Reported (2026-06-17). Per-fixture stub workaround landed in
`tur-tourist` (v0.2.2-pending) and `tur-httpd` (v0.2.1-pending) to make
those spices' fixtures compile; root fix is still open.

**Severity:** Documentation / build-ergonomics defect. Every fixture
across `tur-httpd` and `tur-tourist` that used `(sleep-ms ...)` has been
failing CI since the test suites were introduced (May 2026); the fix
that landed inline in the fixtures masks it but does not address the
broader issue that any new spice author who writes `(sleep-ms ...)` will
hit the same wall.

## Summary

`stdlib/time.tur` exports `sleep-ms` / `get-time-ms` / `Real-Time` plus
the `Real-Time-free` lifecycle helper. None of those names are visible
to user code by default: the compiler's stdlib auto-load list at
`src/main.c:646-727` includes `macros.tur`, `safe.tur`, `contract.tur`,
`hamt.tur`, the typeclass shims, `map.tur` / `vec.tur` / `slice.tur` /
`option.tur` / `result.tur` / `pair.tur` / `tuple.tur` / `list.tur` /
`grid.tur` / `zipper.tur` / `set.tur` / `mutmap.tur` -- but **not**
`time.tur`, `thread.tur`, or the other Phase T19 concurrency files (an
explicit comment in `src/main.c` says T19 stdlib is omitted "to avoid
polluting every program's generated C and invalidating codegen
snapshots").

`time.tur` is sitting in the same omitted bucket. There is no
mechanical or theoretical reason it has to be -- `sleep-ms` is a small
inline-C function with no typeclass deps and no auto-loaded
dependencies of its own. It is omitted by historical accident.

## Observed

Every fixture in `tur-httpd` and `tur-tourist` whose `main` calls
`(sleep-ms N)` (to give the worker thread time to bind the listener
before the client does an HTTP roundtrip) fails compilation:

```
tests/fixtures/hello-world/hello-world.tur:37:10: error: unknown function
  or operator 'sleep-ms'
```

CI run `27589656910` (and every CI run since the affected fixtures were
introduced) is red on these. Two fixtures that do not exhibit the
failure (`url-map`, `nested-routing`) had local `(defn sleep-ms ...)`
inline-C stubs added at authoring time -- exactly the workaround used
to unblock the others.

## Expected

`(sleep-ms 50)` should resolve to `stdlib/time.tur`'s definition in
every program without ceremony, the same way `(some ...)` or `(println
...)` does. Failing that, the failure mode should be a better
diagnostic than "unknown function or operator": "did you mean to
`(load "stdlib/time.tur")`?"

## Root cause

`time.tur` is **not** in `stdlib_files[]` in `src/main.c:646-727`. The
list comment for the explicitly-omitted Phase T19 files reads:

```
/* Phase T19-C/D stdlib files (mutex, rwlock, condvar, sync, thread,
 * chan, atomic) are NOT auto-loaded here to avoid polluting every
 * program's generated C and invalidating codegen snapshots. */
```

Time is in the same bucket without the same justification: it has zero
typeclass deps and its functions (`sleep-ms`, `get-time-ms`,
`Real-Time`, `Real-Time-free`) compile to tiny inline-C bodies. The
auto-loaded `macros.tur` etc. all carry far more weight.

## Workaround landed alongside this report

`spices/tourist/tests/fixtures/{hello-world,middleware,path-capture,
post-echo,query-param,static-files,template}/*.tur` and
`spices/httpd/tests/httpd/{echo,concurrent,headers,post_body}_test.tur`
now each define their own:

```turmeric
(defn sleep-ms [ms : int] #{Unsafe} : void
  ```c
  #include <unistd.h>
  if (ms > 0) usleep((useconds_t)ms * 1000u);
  ```)
```

Call sites wrap with `(unsafe (sleep-ms N))`. This matches the
pre-existing stubs in `url-map` / `nested-routing`. No spice has to
import anything new; the fixture suite stops failing on the
`sleep-ms` unknown-symbol error.

## Proposed fix directions

1. **Add `"time.tur"` to `stdlib_files[]`** in `src/main.c:727`. If
   the codegen-snapshot churn is real, regenerate snapshots in the
   same PR (`tests/fixtures/*/expected.c`) per the CLAUDE.md rule.
   Check that the four omitted T19 files would *also* be auto-loaded
   if the same logic applies -- they probably should not, but the
   reasoning should be explicit.

2. **Or: ship a `time` module under the new spice-import scheme**
   so users write `(import time :refer [sleep-ms])` and the symbol
   table has a real referent. Keeps stdlib codegen clean. Slightly
   higher activation energy for users.

3. **Or: improve the "unknown function" diagnostic** to recognize
   names that exist in non-auto-loaded stdlib files and emit
   "consider `(load "stdlib/time.tur")`". Cheapest fix; addresses
   the discoverability problem without forcing a decision on
   auto-loading.

Option 1 is cleanest. The per-fixture stub is a stopgap while the
decision is made -- it should be removed once the upstream answer
lands.

## Validation

- After the fix lands, removing the local `sleep-ms` stub from
  `tests/fixtures/hello-world/hello-world.tur` (and dropping
  `(unsafe ...)` around the call) should not regress `tur test`.
- The four `httpd` integration fixtures and the seven `tourist`
  fixtures should all compile without the stub.
- CI for `tur-tourist` and `tur-httpd` should turn green on the
  `sleep-ms` axis (other open issues -- audit-S3 `ok-val` failures,
  `tur-httpd`'s `thread-spawn-fn` use in `concurrent_test`, the
  Option-lowering compiler bug noted in
  `docs/reported/option-lowering-mid-migration.md` -- are separate).
