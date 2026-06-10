# `tur run` aborts on Justfile `alias`, silently disabling the snapshot-drift CI guard

**Summary:** `tur run <recipe>` aborts parsing the whole `Justfile` the moment
it hits an `alias` directive, so the Phase 0.3 CI guard
`./build/tur run regen-snapshots -- --check` never runs the check -- it exits
non-zero for an unrelated parse reason. The snapshot-drift guard introduced by
fixture-churn-paydown-plan Phase 0 has effectively been a no-op (or a spurious
failure) since the `alias build-wasm := wasm` line was added.

**Severity:** Medium. Not a miscompile, but a defeated safety net: intentional
or accidental `expected.c` drift can land without the guard catching it, which
is the exact failure mode Phase 0.3 exists to prevent. Also an expressiveness
gap in the `tur run` Justfile parser.

## Minimal repro

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j --target tur
./build/tur run regen-snapshots -- --check
```

Observed:

```
tur run: unsupported Justfile feature at /home/user/turmeric/Justfile:234: alias
        Install `just` (https://just.systems) for aliases.
```

Expected: the `regen-snapshots` recipe runs and reports
`N snapshots are up to date.` (exit 0) or lists drift (exit 1). An unrelated
`alias` elsewhere in the file should not prevent an unrelated recipe from
running.

`Justfile:234` is:

```just
alias build-wasm := wasm
```

The CI job that depends on this is `.github/workflows/ci.yml:86`
(`check-snapshots` -> "Check for snapshot drift").

## Root cause

`tur run`'s embedded Justfile parser treats `alias` as an unsupported feature
and aborts parsing of the entire file rather than skipping/recording the alias
and continuing. Because parsing is whole-file up front, a single unsupported
directive anywhere makes *every* recipe unrunnable via `tur run`. (The upstream
`just` binary handles aliases, so `just regen-snapshots -- --check` works -- but
CI and the docs deliberately drive everything through `tur run` so `just` is not
required.)

## Proposed fix directions

1. **Support aliases in the `tur run` parser (preferred).** Parse
   `alias NAME := TARGET` into the recipe table as an additional invocation name
   for `TARGET`. This closes the expressiveness gap and matches `just`.
2. **Tolerate-and-skip unknown directives.** At minimum, downgrade an
   unsupported top-level directive from a hard abort to a skip-with-warning so
   one unsupported line cannot disable unrelated recipes. (Alias support is
   still better, but this prevents the whole-file blast radius.)
3. **Harden the CI guard independently.** Have `check-snapshots` invoke the
   drift check without going through `tur run`'s alias-sensitive parse path
   (e.g. a small standalone `tools/check-snapshots.sh` that runs the same
   emit-c/diff loop), so the guard is robust regardless of Justfile contents.

## Validation

- After (1) or (2): `./build/tur run regen-snapshots -- --check` exits 0 on a
  clean tree and 1 when an `expected.c` is intentionally edited.
- Confirm `./build/tur run build`, `test`, etc. still resolve with the alias
  present.
- Re-run the `check-snapshots` CI job on a PR that drifts a snapshot and confirm
  it now fails (today it fails for the wrong reason, masking real drift).

## Workaround used meanwhile

The Phase 1.1 sweep validated drift with an inline emit-c/diff loop over the 68
retained `expected.c` files (0 drift) instead of relying on `tur run`.
