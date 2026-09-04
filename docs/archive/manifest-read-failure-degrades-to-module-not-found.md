# A broken `build.tur` is treated as "no manifest", degrading into a cascade of `module not found`

**Severity:** medium. Nothing is miscompiled, but a one-token manifest typo
presents as N unrelated import failures with no working pointer back to the
cause, and `tur check` reports it at `error:` severity while exiting **0**. In
the field this hid an entire spice's test suite being unrunnable for four
weeks, and the failures read as product regressions.

## Summary

`pkg_manifest_read` returns `false` for two very different situations:

1. **there is no manifest here** -- normal, and the right response is to carry
   on with whatever resolution the caller already had; and
2. **there is a manifest and it is broken** -- which is fatal, because the
   caller is about to silently drop everything the manifest was going to
   provide.

Callers cannot distinguish them, so they all take the benign branch. The spice
root's `src/` never joins the module search path, and every intra-spice import
then fails with `module not found` -- naming the import, not the manifest.

## Reproduce

Minimal spice (`:spices` spelled `#fx{...}`, an effect-row literal, where the
reader wants a map -- `TUR-E0620`):

```
mspice/build.tur         (defpackage demo :name "demo" :version "0.1.0"
                            :spices #fx{ "test" #map{:url "..." :optional true} }
                            :exports #map{ "demo/lib" ["answer"] })
mspice/src/demo/lib.tur  (defmodule demo/lib (export answer) (defn answer [] : int 42))
mspice/tests/use_test.tur(defmodule use_it (export)
                          (import demo/lib :refer [answer])
                          (defn main [] : int (println (answer)) 0))
```

```
$ tur test tests
build.tur:4:14: error: TUR-E0620: :spices expects a map ... got an effect-row literal
tests/use_test.tur:2:1: error: module 'demo/lib' not found
  searched:
    tests/demo/lib.tur    (importing file's directory)
    .../stdlib/demo/lib.tur    (stdlib)
  hint: this looks like an intra-spice import.
        try `tur check -I src <file>` from the spice root,
        or build the whole spice with `tur build src/`
1 tests, 0 passed, 1 failed          # exit 1
```

Change `#fx{` to `#map{` in both places and the same command is
`1 tests, 1 passed, 0 failed`, exit 0. Nothing else changes.

Two things to notice in that output:

- **`src/` is absent from `searched:`.** The manifest is what adds it, and the
  manifest was discarded, so the one path that would have worked is the one
  path not tried -- and not mentioned.
- **The `hint:` sends you the wrong way.** It proposes `-I src`, which does
  make the import resolve, so a reader who follows it concludes the import was
  mis-specified and never revisits the manifest. `-I src` is a workaround for
  "not in a spice"; here it papers over a broken manifest.

At scale the E0620 scrolls off: in `turmeric-spices/spices/ecs` this reported
`70 tests, 4 passed, 66 failed`, with the single manifest error above 66
`module not found` blocks.

## `tur check` prints `error:` and exits 0

Independent of the cascade, and easy to miss:

```
$ tur check tests/use_test.tur ; echo "exit=$?"
build.tur:4:14: error: TUR-E0620: ...
exit=0
```

Verified three ways (consumer at spice root, consumer in `tests/`, and `tur
check` on a file in `src/` itself): E0620 is emitted at `error:` severity every
time and the exit code is 0 every time. Any CI step that shells out to `tur
check` and trusts `$?` sees a clean manifest. `tur test` does exit 1, but only
because the *tests* failed, not because the manifest did.

`pkg_manifest_read` computes `bool ok = !diag_had_error()`
(`src/compiler/pkg.c:737`), so the read itself does report failure; the error
state is evidently no longer set by the time `tur check` picks its exit code. I
did not pin down where it is cleared -- worth finding, because it decides
whether the fix belongs in the manifest layer or the driver.

## Root cause

- `reject_fx_row` emits TUR-E0620 (`src/compiler/pkg.c:186`), reached from
  `expect_map` (`:197`) via `parse_spices` (`:219`). The guard is correct and
  the message is good; the problem is entirely downstream of it.
- `pkg_manifest_read` collapses "absent" and "malformed" into one `false`
  (`src/compiler/pkg.c:737`).
- Callers then take the benign branch. `collect_spice_aux_c` does
  `if (!pkg_manifest_read(mp, &m)) return;` (`src/main.c:3236`); the same
  `!pkg_manifest_read(...) -> continue/return` shape appears at `src/main.c:3212`,
  `:3271`, and `:3306`, and there are ~10 call sites in total.
- Contributing: `pkg_manifest_read` (`src/compiler/pkg.c:537`) discards the
  `bool` from its slot parsers -- `parse_spices` (`:657`), `parse_cmake_deps`
  (`:659`), `parse_exports` (`:661`) all return `bool` and none is checked --
  so parsing continues past a failed slot and the resulting `PkgManifest` is
  silently partial. Only the trailing `diag_had_error()` sweep catches it,
  which is why the failure is all-or-nothing and carries no indication of
  *which* slot broke.

## Fix directions

1. **Separate the two outcomes.** Give `pkg_manifest_read` a tri-state (or an
   out-param): `ABSENT` / `OK` / `MALFORMED`. `ABSENT` keeps today's quiet
   fallback; `MALFORMED` is fatal at every call site. This is the real fix and
   it is mechanical -- the call sites already branch, they just branch on too
   little information.
2. **Make a malformed manifest fail the command**, including `tur check`. A
   diagnostic printed at `error:` severity should never coexist with exit 0.
3. **Make the cascade self-diagnosing** if 1 is deferred: when resolution is
   about to report `module not found` and a manifest was found-but-rejected at
   the discovered spice root, say so and suppress the `-I src` hint. A note on
   the `searched:` list ("`src/` not added: build.tur failed to read") turns
   66 misleading errors into one true one.
4. Check the slot-parser return values so a partial `PkgManifest` is never
   handed back.

## Field note

The spelling that triggered this (`:spices #fx{...}`) was itself a stalled
migration: turmeric `fe67bd9fd` (2026-07-05) split `#fx{...}` from `#{...}` and
added TUR-E0620, and the companion spices-side change converted `:exports` but
missed `:spices` in the same files. So **41 manifests** were unreadable on every
`tur` from v0.27.0 onward, and nobody noticed for four weeks, because the
symptom never mentioned the manifest. Fixed spices-side in
`turmeric-spices` `e091505`; this report is about why it was invisible, which
is the part that will happen again.

That also means "suite green" claims recorded for those spices between
2026-07-05 and 2026-08-01 were measured against a compiler older than the
shipping one, and should not be trusted without a re-run.

## Resolution (2026-08-13)

All four fix directions landed, plus a memory leak the repro surfaced.

**1. The two outcomes are separated.** `PkgManifestStatus`
(`ABSENT` / `OK` / `MALFORMED`) with `pkg_manifest_read_status()`; the old
`pkg_manifest_read()` stays as a bool wrapper. The boundary is the `fopen`:
**before** it, "no manifest here" is `ABSENT` and stays quiet, which is the
normal result of every walk-up probe. **After** it the file exists, so every
subsequent failure is `MALFORMED`.

The report's direction 1 proposed making `MALFORMED` fatal at each of the ~28
call sites. That is not what landed, and the difference matters: rather than
edit 28 branches, a malformed manifest is recorded in a **sticky verdict** that
survives `diag_reset()`, and `pkg_manifest_reassert()` re-emits it at each
compile entry point. The command fails wherever the manifest was consumed,
which is the actual goal, with a fraction of the blast radius.

**2. `error:` no longer coexists with exit 0** -- and the report's open question
("the error state is evidently no longer set by the time `tur check` picks its
exit code. I did not pin down where it is cleared") has an answer that was
already written down 30 lines below the code the report was reading.
`pkg.c`'s `:tur-version` block documents exactly this, for exactly this reason:

> The manifest is read once, before compilation; every compile entry point then
> calls `diag_reset()` [...] That reset is correct, and it also wiped this
> check's error -- so a floor violation printed an "error" and then exited 0,
> which is not an error at all. **(The pre-existing TUR-E0620 manifest error has
> the same shape.)**

So the mechanism was known and annotated; only the fix had not been generalised
from `:tur-version` to the manifest as a whole. `pkg_manifest_reassert()` is
modelled directly on `pkg_tur_version_reassert()` and is called beside it at
all three sites in `main.c`.

**3. The cascade is gone rather than annotated** -- and direction 3 was
**deliberately not implemented**. It was explicitly conditional ("if 1 is
deferred"), and 1 was not deferred. Because TUR-E0624 fails the compile *before*
elaboration, the `module not found` errors never happen at all: verified across
`tur check`, `tur test`, `tur emit-c`, `tur run`, and `tur build`. A prototype
that added a `note:` to the `searched:` list and suppressed the `-I src` hint
was written, confirmed unreachable, and removed; `elab_module.c` carries a
comment saying why, so the next reader does not re-add it. Preventing the
cascade beats explaining it.

**4. Slot-parser return values are checked.** `parse_spices` /
`parse_cmake_deps` / `parse_exports` / `parse_str_vec` had their `bool` results
discarded. They are now checked, and the first failing slot name rides along in
the sticky verdict, so the error names it. Parsing still continues past a bad
slot on purpose -- the user sees every slot's diagnostic in one pass instead of
one per edit-compile cycle.

### A memory leak, found by reproducing the report

Running the repro under the Debug build's LeakSanitizer failed the process with
36 bytes leaked in 4 allocations from `parse_exports` (`pkg.c:483`) and `ss_dup`
(`pkg.c:50`). Cause: the slot parsers run to completion past a broken slot, so
`out` holds whatever *did* parse, and every caller's `if (!read) continue;`
branch dropped it without `pkg_manifest_free`. `pkg_manifest_read` now frees and
zeroes `*out` before returning false, so the failure branch owns nothing. This
also settles the report's "silently partial `PkgManifest`" concern by
construction -- there is no longer a partial manifest to hand back.

One artifact worth knowing: **the leak masked the bug.** Under the Debug build
the repro exits **1**, because LeakSanitizer fails the process -- not because
the manifest error was honoured. The report's exit-0 claim is correct and
reproduces on Release, or with `ASAN_OPTIONS=detect_leaks=0`. Anyone re-checking
this on a Debug `tur` and reading the exit code alone will conclude, wrongly,
that there is nothing to fix.

### Verification

`tests/spice-resolver-tests.sh` gains seven cases (73 passed, 0 failed):
`tur check` on the report's exact `:spices #fx{...}` tree must exit non-zero;
stderr must carry `TUR-E0624` and name `:spices`; stderr must **not** carry
`not found` or the `-I src` hint; the same tree with `#fx{` corrected to
`#map{` must still check clean; and an **absent** manifest must remain silent
and exit 0 -- that last one guards the half of the split most at risk from
this fix.

`tests/run.sh`: 2590 passed, 0 failed. The manifest-adjacent ctest targets
(`tur_spice_resolver_tests`, `tur_spice_c_sources_tests`, `tur_flags_tests`,
`tur_build_project`, `tur_sweet_manifest`, `tur_install_tests`, and all eight
`tur_repl*`) pass.

### Filed separately

`tur build src/` -- the invocation the `module not found` hint recommends --
prints `tur: no .tur files found in 'src/'` and **exits 0** on a project whose
sources are nested one level (`src/demo/lib.tur`). Unrelated to the manifest
(it reproduces with a perfectly valid one) but found while verifying this fix.
See `docs/archive/tur-build-nested-src-dir-finds-no-files.md`.
