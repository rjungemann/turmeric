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
