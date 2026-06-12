# `run-turi.sh` compiles fixtures instead of interpreting them

> **RESOLVED (TI8):** `tests/run-turi.sh` now runs fixtures with
> `tur --interpret`. Reconciling the allowlist to true interpretation removed
> 31 false-green entries; the harness is green again at 122 passed. See
> [turi-harness-flip-reconciliation.md](turi-harness-flip-reconciliation.md)
> for the bucketed 31 + the full-denylist blast radius. `tests/run-flags.sh`'s
> three `tur run` assertions are the only remaining instance of this wiring.

**Summary:** The "interpreter parity" harness `tests/run-turi.sh` (and the
fixture-running portions of `tests/run-flags.sh`) invoke `tur run <file>`,
which **compiles and runs a native binary**, not the tree-walking `turi`
interpreter. As a result the entire turi-parity allowlist has never actually
exercised `src/turi/eval.c`; allowlisted fixtures "pass" via the C codegen
path. This silently invalidates the allowlist, the TI0 typeclass audit, and
the premise of `docs/upcoming/v1/turi-parity-post-v1-plan.md`.

**Severity:** High. It is a silent test-coverage hole: a feature can be
completely unimplemented in the interpreter (default arm: `eval: unhandled
expression kind N`) while its fixture sits on the turi allowlist showing
`PASS`. The CI ratchet the plan wants to build (TI8/`check_turi_parity.py`)
would be measuring nothing as long as the harness compiles.

## Root cause

`tur run <file.tur>` dispatches to `cmd_run` (`src/main.c:8517-8548`), which
routes any argument ending in `.tur`/`.tur.sweet` to the classic
compile-and-run path. `cmd_run` → `RUN_ENTRY` → `cmd_build`
(`src/main.c:2749-2763`): it emits C, compiles it, and execs the binary. There
is no interpreter branch in that path.

The interpreter is reached only through `tur --interpret <file>` (→ `cmd_eval`
→ `turi_eval_file`, `src/main.c:8574-8579`, `4906`) or `tur eval --file`.

`tests/run-turi.sh:289-310` runs each fixture with:

```sh
"$TUR" $fixture_flags run "$input" ...
```

i.e. `run`, the compile path. The harness was created already wired this way
(commit `599706b`, 2026-06-09 -- it has never used `--interpret`), yet its
header comment claims it "Runs ... through `tur run` (the tree-walk
interpreter)" and `CMakeLists.txt` adds leak-detection carve-outs
(`turi_fixture_tests`) on the belief that it exercises the
never-frees-its-closures interpreter.

## Minimal repro

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j

# letrec is on the turi allowlist and EX_LETREC had no interpreter case arm.
./build/tur run        tests/fixtures/letrec-mutual/input.tur   # => true / true  (compiled: OK)
ASAN_OPTIONS=detect_leaks=0 \
./build/tur --interpret tests/fixtures/letrec-mutual/input.tur  # => empty, rc=1 (interpreter: unhandled)
```

The harness reports `PASS letrec-mutual`; the interpreter cannot evaluate it.

## Observed vs. expected

- **Observed:** `run-turi.sh` summary is green; the allowlist appears healthy.
- **Expected:** the harness should run each fixture through `turi` and surface
  genuine interpreter gaps as `FAIL`.

Running the **current allowlist** (129 fixtures) through the real interpreter
(`tur --interpret`, stdout+exit compared the same way the harness does) yields
**31 failures** that the compile path hides, including:

```
call-cc-star clone-primitives clone-list clone-option clone-pair
continuation-callcc continuation-escape continuation-escape-fn
dynvar-convey dynvar-convey-isolation effect-capture-k ptc4-basic
result-basic typed/grid-basic typed/list-basic typed/list-macro
typed/map-basic typed/map-collision typed/map-eq typed/option-basic
typed/pair-basic typed/result-basic typed/set-basic typed/slice-basic
typed/vec-basic typed/zipper-basic weak-dangling arrow-instance-apply
arrow-instance-stdlib-basic hkt-stdlib-result-ok-biased instance-head-hole-pair
```

Note four of these (`arrow-instance-apply`, `arrow-instance-stdlib-basic`,
`hkt-stdlib-result-ok-biased`, `instance-head-hole-pair`) are exactly the
fixtures the **TI0 audit** "verified ... against `./build/tur run`" and added
to the allowlist. Because `tur run` compiles, that verification did not touch
the interpreter and the four are in fact broken under it. The TI0 audit's own
"12 fixtures were run against `./build/tur run`" methodology is unsound for the
same reason.

## Proposed fix

1. Point the harness at the interpreter:

   ```sh
   "$TUR" $fixture_flags --interpret "$input" ...
   ```

   (or `eval --file`). Keep `ASAN_OPTIONS=detect_leaks=0`, which the harness
   already exports -- the interpreter's process-lifetime closures otherwise
   trip LeakSanitizer.

2. The flip turns ~31 currently-allowlisted fixtures red immediately, and more
   once the allowlist→denylist switch (TI8) runs every fixture. This is exactly
   the triage TI8 budgets a day for. Each newly-red fixture must be either:
   - fixed in `src/turi/eval.c` (the real parity work, TI1-TI6), or
   - tagged `requires.tur-only` / `requires.compiled` with a one-line reason
     (genuine carve-outs: inline-C-driven fixtures, WASM async, etc.).

   Do **not** flip the harness without doing this triage in the same change, or
   CI goes red.

3. Fix the same `run` → `--interpret` wiring in the fixture-running parts of
   `tests/run-flags.sh` (e.g. `:345`, `:355`, `:408`), which has the identical
   problem.

## Validation

After the wiring fix, a clean `run-turi.sh` should show interpreter-specific
`FAIL`s for any unimplemented `EX_*` kind, and zero `FAIL`s once the gaps are
closed or carved out. Spot-check with a fixture whose feature is known to be
interpreter-only-broken (e.g. `typed/list-basic`, which errors with
`make-struct: 'Cons' is not a defined struct type` under `--interpret`).

## Status

Filed while executing TI1 of `turi-parity-post-v1-plan.md`. TI1's interpreter
work (EX_LETREC, EX_SET_FIELD) was implemented and verified directly with
`tur --interpret` rather than through the harness, precisely because the
harness does not interpret. The harness wiring fix is intentionally **not**
bundled here: it is a large, cascading change (the 31-fixture triage above)
that belongs with TI8, and landing it blind would turn CI red. This report
exists so the gap is not forgotten.
