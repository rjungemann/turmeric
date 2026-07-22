# Codegen: user defn named `pipe2` collides with libc `pipe2`, build fails

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
