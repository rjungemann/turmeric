---
title: stdlib codegen emits undeclared `m1`/`x`/`keyeq`/`val_cmp_NNN` for trivial programs
category: Reported
severity: high
description: Non-trivial Turmeric programs fail to compile via `tur build`. The generated C includes references to identifiers `m1`, `x`, `container`, `fn_right`, `keyeq`, and `val_cmp_NNN` that are never declared, in stdlib autoloaded blocks (Map/Eq/MapKey instances). Triggered by ordinary code: even adding a second `let` binding to a 12-line program activates the broken codegen path.
---

# stdlib poly-codegen emits undeclared identifiers

## Summary

`tur build` on freestanding programs produces C source that references
undeclared identifiers from inside stdlib autoloaded code:

```
_tmp_probe4_tur.c:3065: call to undeclared function 'm1';
_tmp_probe4_tur.c:3069: use of undeclared identifier 'val_cmp_558'
...
```

Severity: **high.** The bug fires for nearly any non-trivial freestanding
input (anything beyond ~15 lines that uses `let` with more than ~3
bindings), so it blocks the ordinary `tur build <file>` workflow. The
in-tree fixture suite (`bash tests/run.sh`) still passes, so something
about the fixture-build path avoids the trigger -- the issue is in the
standalone-build path.

## Observed vs. expected

### Expected

A small freestanding `defn main` program should build via
`./build/tur build <file>`.

### Observed

```sh
cat > /tmp/probe4.tur <<'EOF'
(defn invoke-float [^fat f : (fn [float] float)] :float
  (f 7.1))

(defn print-f [x :float] :int
  ```c
  printf("%.3f\n", x);
  return 0;
  ```)

(defn main [] :int
  (let [c (fn [x :float] :float (* x 2.0))
        r (invoke-float c)
        a 1
        b 2]
    (print-f r)
    0))
EOF

./build/tur build /tmp/probe4.tur -o /tmp/probe4
# error: call to undeclared function 'm1'; ISO C99 and later do not support implicit function declarations
# ...
```

Removing the `a 1 b 2` let bindings produces a working binary. The
minimum reproducer is *adding inert bindings*, which can't be what
actually activates the bad codegen path -- it's almost certainly some
indirect trigger (compactor heuristic, dictionary materialization,
unused-binding visit, etc.) that flips the elaborator into a state
where stdlib instance methods get codegen'd with the wrong identifier
substitution.

## Codegen context

The bad block looks like:

```c
static dict_MapKey_float dict_MapKey_float_singleton = {
    .mk_box = __inst_MapKey_mk_box_float,
    .mk_cmp = __inst_MapKey_mk_cmp_float,
    .mk_owned_qu = __inst_MapKey_mk_owned_qu_float,
};

static int64_t __poly_562(void * __poly_env_563, int64_t __poly_x0_565) {
        return m1(__poly_x0_565);     // <-- m1 never declared
}

static int64_t __poly_588(void * __poly_env_589, int64_t __poly_x0_591) {
        return x(__poly_x0_591);      // <-- x never declared
}

static bool __inst_Eq_eq_qu_Map(int64_t x, int64_t y) {
    ...
    return map_eq_dynamic((tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_588 }, y, ...);
}
```

The `m1`, `x`, `container`, `fn_right` look like **parameter names**
from the original Turmeric source (`m1` is plausibly a typeclass-method
arg name, `x`/`container` look like struct accessor names). The
codegen appears to be substituting *the parameter name* into the body
verbatim, instead of the materialized expression for that parameter
(e.g. `__poly_x0_565`, `arg->field`).

Likely culprits to inspect:

1. **Typeclass method codegen** for `Eq`/`MapKey` instances over
   parameterized maps -- specifically the `eq_qu` and `mk_*` paths
   (`stdlib/map.tur`, `stdlib/equal.tur`, `stdlib/hamt.tur`).
2. **Polymorphic wrapper emission** (`__poly_NNN`). The wrapper takes
   `(__poly_env, __poly_x0_NNN)` but emits a call using the *original*
   identifier from the source instead of `__poly_x0_NNN`.
3. **Dictionary record materialization** -- something earlier registers
   the instance but the closure-over-environment substitution is
   skipped at codegen.

## Why `tests/run.sh` doesn't catch it

The fixture suite uses fixture-style inputs (often `tests/fixtures/<name>/input.tur` plus an `expected.c` snapshot). The
suite is largely small, focused, and the `expected.c` snapshot already
locks in working codegen for each one. Freestanding `tur build <file>`
on arbitrary new programs goes through the same codegen but trips a
combination of stdlib instances the fixtures don't.

A quick way to expand coverage: add a fixture that mirrors `probe4`
above -- 4-binding `let`, one `^fat` typed-fn invocation, one inline-C
helper -- and watch it FAIL with `expected.c` mismatch (no current
expected snapshot will match the broken output).

## Minimal reproducers

The original 12-line `float_closure_probe.tur` (one `let` binding plus
a non-trivial inline-C) builds and runs cleanly. The 16-line `probe4`
(same code plus two integer bindings) fails. Any of the following also
fail:

- An `alloc-state` inline-C helper that calls `calloc`.
- A `(defn ...)` that takes an `:int` state pointer and a `:float`
  argument and threads them through inline-C.

The float-closure-return ABI itself works -- `float_closure_probe`
prints `14.200` from a fat-dispatched closure returning `:float`, so
the underlying calling convention is sound. The bug is in stdlib
poly-instance codegen, not in fat dispatch.

## Proposed fix direction

1. Reproduce locally:
   ```sh
   rm -rf /tmp/tur-build
   ./build/tur build /tmp/probe4.tur -o /tmp/probe4   # fails
   ```
2. Inspect the generated C around the `__poly_NNN` function bodies
   that reference `m1`/`x`/etc. Map each undeclared identifier back to
   the Turmeric source that produced it (likely a typeclass method or
   struct accessor).
3. Fix the codegen to emit the polymorphic-wrapper parameter
   substitution: replace the Turmeric-source identifier with the
   `__poly_x0_NNN` (or appropriate `env->field`) at the codegen step
   for typeclass-method wrappers.
4. Add a freestanding-build fixture covering 4+ `let` bindings and a
   `^fat` typed-fn parameter so this never regresses again.

## Validation of a fix

- `./build/tur build /tmp/probe4.tur -o /tmp/probe4` succeeds.
- `bash tests/run.sh` still green.
- New fixture covering the freestanding case is added and passes.

## Related

Discovered while implementing Phase 0c of the
`tur-signal` spice's `:float` sample migration: every attempt to build
a runtime smoke test for the migrated DSP filters surfaced this
codegen bug, regardless of whether the source code referenced any
stdlib map/eq machinery directly.
