# Functions inside a `defmodule` are never effect-row checked

**Severity: medium (silent loss of a checked guarantee).** `effect_check_pass`
walks only the program's top-level `EX_FN_DEF` items. A `defn` inside a
`(defmodule ...)` body lives in `EX_DEFMODULE`'s `mod->body` and is never
visited, so its declared `#fx{...}` row is never resolved, never inferred and
never checked: `#fx{}` on a module member is a promise nothing reads. Filed
2026-10-02, found while measuring effect-row-honesty-plan W4 against
turmeric-spices.

## Minimal repro

```turmeric
(defmodule m
  (export loud)
  (defn loud [] #fx{} : int (do (println "x") 0)))
(defn main [] : int 0)
```

```sh
./build/tur check repro.tur ; echo rc=$?
```

Observed: no diagnostic, `rc=0`. Expected: `TUR-E0009` -- `loud` declares
`#fx{}` and prints, which is `#fx{IO}` since W4. The same `defn` at top level
is rejected. An undeclared name in a member's row (`#fx{Typo}`) is not
reported either (TUR-E0026 runs during the same resolution), and neither is a
`(perform ...)` of an effect the member's row omits.

## Root cause

`src/passes/effect_check.c`, `effect_check_pass`: every step -- Step 0's row
resolution, the fixed point, Step 3's validation, the closure / call-site /
unreachable-handler sweeps -- iterates `program->as.program.items` and skips
anything that is not `EX_FN_DEF`. Module members are inside `EX_DEFMODULE`
items.

The CPS coloring had the same blind spot and fixed it by descending into
`mod->body` (`src/passes/cps.c`, the `CPS_ADD_FN_NODE` loop and its comment:
"A module member never appears as a top-level EX_FN_DEF"). The effect pass
never got the equivalent change.

## Blast radius of fixing it

This is why it is filed rather than fixed alongside W4: checking module
members turns every annotated member that prints, or that calls a member
whose row grew, into a compile error. Known instances in turmeric-spices,
where test programs are themselves modules -- each a `main` declared
`#fx{Unsafe}` that calls `println`:

- `spices/sqlite/tests/linear_handles_test.tur` (`main`, verified)
- `spices/tls/tests/conn_linear_test.tur` (`main`)
- `tidal_test.tur` (two sites)

Those were found by a static scan, not by running a fixed compiler; the
real count needs the fix and a sweep. Inside this repo, the stdlib loads
modules too (`effect-row-cross-private` shows `wrapper` and `run-internal`
absent from `--dump-effects` today).

## Fix directions

1. **Descend into `EX_DEFMODULE` bodies in every step of
   `effect_check_pass`**, the way `cps.c` does -- probably by collecting the
   function items once (top-level plus module members) into a list each step
   iterates. Keep the module-context bookkeeping (`s_current_analysis_module`,
   private-effect filtering) correct for members. Then sweep this repo and
   turmeric-spices and annotate what breaks (`#fx{Unsafe IO}` on those
   `main`s).
2. **Land it behind a warning first** if the spices sweep is large: report
   member violations as a warning for a release, then promote. Weaker, but
   it is a migration, unlike W1's undeclared-name error, which broke nothing.

`--dump-effects` should list module members too once they are inferred, so
the gap is visible from the outside.
