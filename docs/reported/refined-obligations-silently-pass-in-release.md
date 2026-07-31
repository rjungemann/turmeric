# `refined` obligations silently pass on a Release build

**Severity: high.** The `refined` experiment's runtime-checked obligations do
not fire when `tur` is built `-DCMAKE_BUILD_TYPE=Release`. A refinement
violation that aborts correctly on a Debug build exits 0 on Release, with no
diagnostic. This is not JIT-related -- it reproduces on the plain `tur run`
path.

Found 2026-07-31 while running the mir-interp-tier I0 spike; the `refine-*`
fixtures showed up as failures under a Release JIT build in **both** engine
modes, which is what separated them from the interpreter divergences being
measured.

## Repro

```sh
# Debug build -- contract fires (SIGABRT)
./build/tur --enable=refined run tests/fixtures/refine-match-field-wrong/input.tur
echo $?      # 134

# Release build -- silently passes
./build-jitrel/tur --enable=refined run tests/fixtures/refine-match-field-wrong/input.tur
echo $?      # 0
```

`tests/fixtures/refine-match-field-wrong/expected.exit` is `nonzero`, so the
Debug result is the correct one.

## Scope

Nine fixtures fail this way on a Release build, and pass on Debug:

```
refine-let-shadow-not-split          expected nonzero exit, got 0
refine-match-arm-hyps-not-shared     expected nonzero exit, got 0
refine-match-field-wrong             expected nonzero exit, got 0
refine-match-impure-arm              expected nonzero exit, got 0
refine-memo-distinct-obligations     expected nonzero exit, got 0
refine-typeclass-not-congruent       expected nonzero exit, got 0
refine-typeclass-result-unproved     expected nonzero exit, got 0
refine-off-is-contracts-only         stdout mismatch
errors/refine-impure-predicate       diagnostic mismatch
```

Seven of the nine are `expected nonzero exit, got 0` -- the violation is not
merely reported differently, it is not detected at all.

## Root cause (partial)

`src/compiler/refine_discharge.c:11` states the design: an obligation the
solver cannot discharge statically is **moved to a runtime contract check**.
Those checks are the `stdlib/contract.tur` family, which lower through
`tur-contract-check`.

What is confirmed: the divergence is Debug-vs-Release, on the non-JIT path,
and it is the *detection* that disappears rather than the reporting. What is
not yet pinned down is which `NDEBUG` boundary removes the check -- whether
the contract lowering itself is compiled out of the generated program, or the
discharge stage stops emitting the fallback check when `tur` is built with
`NDEBUG`. `src/CMakeLists.txt:399` et al. add `-DNDEBUG` to Release, and the
codegen has NDEBUG-conditional regions (`emit_module.c:10624`, `:10646`,
`:10658`) -- but those specific ones are session-channel debug strings, not
contracts, so the responsible boundary is still unidentified.

## Why it matters

The fixture suite runs against a Debug build, so this class of failure is
invisible to normal development -- the suite is green precisely because it
never exercises the configuration end users get. Anyone running a Release
`tur` gets an experiment that reports success while checking nothing, which
is worse than the feature being off: `refine-off-is-contracts-only` exists to
assert that disabling refinement still leaves contracts working, and it also
fails here.

Note this interacts with the release-cut skills: the `refined` row in
`EXPERIMENTS[]` carries an `expires_at`, and a graduate/shelve decision taken
on Debug-only evidence would be made on numbers that do not describe the
shipped build.

## Fix directions

1. Identify the `NDEBUG` boundary that drops the check -- start at the
   contract lowering in the discharge stage rather than at the codegen
   NDEBUG regions, since the latter were ruled out above.
2. Whatever the mechanism, refinement obligations should not be
   build-type-dependent. If contracts are deliberately Debug-only, then
   discharge must refuse to *move* an obligation to a runtime check it knows
   will not exist in the target build, and report it statically instead.
3. Add a Release-build run of the `refine-*` fixtures so the configuration is
   covered. This is the gap that let it through.
