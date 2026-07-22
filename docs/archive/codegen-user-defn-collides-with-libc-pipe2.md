# Codegen: user defn named `pipe2` collides with libc `pipe2`, build fails

> **RESOLVED (2026-07-22).** Took the report's "cheapest scoped fix": a curated
> libc/POSIX symbol denylist (`tur_name_collides_libc`, `src/compiler/mangle.c`)
> that forces mangling ONLY on collision, so churn is minimal. A bare
> (non-module-prefixed) global `defn`/`def` whose spelling is a libc symbol now
> emits with a `tur_u_` guard prefix (`read` -> `tur_u_read`) instead of a bare
> `static int64_t read(...)` that redeclares the system-header prototype. The
> guard is applied at the two name-generation chokepoints that must stay in
> lockstep -- `raw_name_for_binding` (emitter) and `elab_mangle_binding_name`
> (elaborator, used for inline-C `__TUR_CNAME_` splices) -- so definition, every
> call site, and inline-C references all resolve to the same C name.
>
> Scoped to avoid the global-rename footgun the report warns about:
> module-qualified globals already carry a distinguishing prefix
> (`geom__read` never matches the bare denylist) and extern-c bindings keep the
> libc spelling on purpose to name the real symbol -- both are untouched. Only
> bare top-level libc-named globals are remapped; a sweep found just four
> pre-existing fixtures in that shape, none carrying an `expected.c` snapshot, so
> the change is snapshot-churn-free.
>
> Verified on Linux, where glibc declares `read`/`write`/`close`/`pipe`/`time`/
> `link`/`index`/... in `<unistd.h>` etc. by default (so the collision reproduces
> here, not only on the macOS SDK that added `pipe2`): all of those now build and
> run; `hrt-stdlib-cont` (the report's `pipe2` fixture) and the three other
> affected fixtures pass; a new regression fixture
> `tests/fixtures/libc-name-collision-mangle` defines `read`/`write`/`time` and
> asserts they build and run; full `bash tests/run.sh` green (2269 passed).
> Original finding below.

**Severity:** Medium -- any user function whose name matches a libc symbol
now visible in the SDK headers fails to compile. Newly triggered because
recent macOS SDKs declare `pipe2` in `<unistd.h>`.

## Symptom

`tests/fixtures/hrt-stdlib-cont` -> `tur build failed`:

```
error: static declaration of 'pipe2' follows non-static declaration
 3728 | static int64_t pipe2(tur_poly_fn_t, tur_poly_fn_t, int64_t);
/.../sys/unistd.h:219:9: note: previous declaration is here
  219 | int pipe2(int [2], int);
...
error: too many arguments to function call, expected 2, have 3
```

## Repro

`tests/fixtures/hrt-stdlib-cont/input.tur`:

```turmeric
; pipe2 applies f then g in sequence
(defn pipe2 [f (forall [a] (-> a a)) g (forall [a] (-> a a)) x : int] : int
  ...)
```

A `defn pipe2` emits `static int64_t pipe2(...)` into the same TU that
transitively includes `<unistd.h>`, whose non-static `int pipe2(int[2], int)`
now exists on this SDK. C rejects the redeclaration and mis-resolves the call.

## Root cause

User-level names are lowered to their bare identifier in the generated C
(`pipe2` -> `pipe2`) with no reserved/libc-collision avoidance. When a system
header in the same TU declares the same identifier with a different signature,
the two collide. `pipe2` was recently added to the macOS SDK `<unistd.h>`, so
a fixture that has compiled for a long time started failing purely from the
toolchain side -- but the underlying codegen-hygiene gap is real and
platform-independent (any `defn read`, `defn open`, `defn pipe2`, ... is a
latent landmine).

## Fix directions

- Give user-defined top-level function names a reserved-collision-safe
  emission (a `tur_u_` / `__tur_user_` prefix, or a curated libc-name denylist
  that forces mangling only on collision to minimize churn).
- Cheapest scoped fix to unblock the suite: mangle only when the lowered name
  matches a known libc/POSIX symbol set.
- If a broad rename is chosen, regenerate fixture snapshots in the same PR
  (codegen change). Beware the global-rename footgun in
  [[feedback_no_global_rename]] -- scope by symbol table, not text.
