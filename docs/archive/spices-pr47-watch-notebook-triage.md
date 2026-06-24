# Triage: turmeric-spices PR #47 "Checks & Tests" follow-ups (watch, notebook)

**Date:** 2026-06-24. **Compiler:** main, v0.25.0 (HEAD `6244033`).

A turmeric-spices PR #47 report listed three "turmeric repo" follow-ups. This
note records the triage of all three against a from-source `main` build, with
minimal repros. **Conclusion: none of the three is a turmeric defect that
warrants a speculative codegen/runtime change.** Two are spice-side; one is a
narrow, real gap filed separately as an open report.

## 1. "Honor `#[used]` under whole-program / `tur test` builds"

Largely a **misdiagnosis**, with one narrow genuine gap.

- The reported `frame` link failures (`interop`/`group`/`reshape`) were
  **spice-side**: stale hand-spelled mangled names in inline-C bridges. The
  live mangler escapes `_`->`_un` and `-`->`_hy`, so e.g. `frame/sort/__so-take`
  is `frame__sort___un_unso_hytake`, never the hand-spelled
  `frame__sort____so_take`. Already resolved; see
  `docs/archive/cross-module-private-helper-dropped-at-link.md`. The fix is to
  reference siblings through the module system or the `__TUR_CNAME_<name>__`
  splice (verified working in both build modes).
- `#[used]` **is** honored in whole-program mode when the defining module is in
  the entry's Turmeric import closure (the normal case): the unexported,
  Turmeric-unreachable defn is retained and links as a same-TU `static`
  (verified by repro).
- The **only** real gap: a `#[used]` defn reached *only* via a raw mangled
  `extern` from a sibling that is **not** `(import)`ed was dropped on the
  single-file / whole-program path (`tur test`, `tur run <file>`, `tur build
  <file>`), because that path lacked the `file_has_used_attr` ->
  separate-compilation fallback that `tur build <project>` has. This is the
  legacy raw-extern pattern the C-integration guide already discourages, so
  guide-following spice code (which `import`s its modules) did not hit it.
  **FIXED 2026-06-24** (v0.25.0): `cmd_build` now scans the `-I` search dirs for
  `#[used]`-bearing modules and force-loads them into the whole-program TU so
  the extern resolves. See
  `docs/archive/used-attr-not-honored-in-single-file-whole-program.md` and the
  `build-file-used-attr-whole-program` regression test in
  `tests/run-build-project.sh`.

## 2. "Fix inline-C binding visibility codegen (watch)"

**Not a turmeric defect.** The watch tests reference `let`-bound locals from an
inline-C block by their source-derived name (e.g. `found_x` for `found-x`).
A `let`-bound local is emitted with the injective mangler plus a uniquifying id
suffix (`found_hyx_1246`) -- *not* the legacy `found_x` fold that the inline-C
hand-spelled -- so the reference is undeclared at the C stage. `tur check`
passes because no codegen runs.

This is the same class as item 1: relying on an **unstable local mangled name**,
which the C-integration guide explicitly warns against
(`docs/guides/c-integration-guide.md`: "Do not rely on identifier names that
look like Turmeric-mangled names ... these are unstable implementation
details"). The guide documents that **parameters** are available by source name;
`let`-bound locals are not, by design -- the id suffix is load-bearing
(distinct same-named bindings must not collide in C, and source names that are C
keywords like `double` must be disambiguated). Removing the suffix is unsafe.

Repro (`#{Unsafe}` fn, inline-C nested under a `let`):

```turmeric
(defn probe [ev : int batch : int] #{Unsafe} : int
  (let [found-x 10
        found-y 20]
    ```c
    return ev + batch + found_x + found_y;   ; => `found_x`/`found_y` undeclared
    ```))
```

The C compiler errors: `'found_x' undeclared`. Params `ev`/`batch` resolve fine
(parameters keep their source name; see `name_for_binding`,
`src/compiler/emit_core.c:1852-1886`, which returns `raw_name_for_binding` for
params but the id-suffixed mangle for other locals).

**Spice-side fix (verified):** reference the locals via the splice --
`__TUR_CNAME_found-x__` / `__TUR_CNAME_found-y__` -- which the emitter expands to
the actual emitted C name (`found_hyx_1246`). With that change the repro builds
and runs correctly. Alternatively, pass the values as function parameters
(documented to be available by source name). The report's claim that this is
"not fixable in spice source" is incorrect -- the `__TUR_CNAME_` splice resolves
`let`-bindings, not just sibling `defn`s.

## 3. "Investigate notebook/runtime ASan leaks"

**Not a turmeric defect -- by design.** The notebook suites trip
LeakSanitizer with frames through `turi_error` / `src/turi/value.c`. These are
the **tree-walking turi/eval interpreter's** deliberate process-lifetime
allocations. The interpreter never frees `TuriValue`s or their `strdup`'d
`as_error` strings (`src/turi/value.c:10-25`); confirmed there is no
value-free / `turi_free` path anywhere in `src/turi/`.

This is exactly the policy documented in CLAUDE.md and the ASan plan: the
interpreter "intentionally never frees its closures/registered natives
(process-lifetime)", so every in-repo harness that runs it defaults to
`ASAN_OPTIONS=detect_leaks=0` -- `tests/run-turi.sh:44`,
`tests/run-flags.sh:24`, `tests/run-build-project.sh:24`. The leak-checked path
is the **compiler/codegen** (`emit-c`/`build`), which is leak-clean and stays
checked.

Repro: running any program through the interpreter with leak detection on
surfaces the same pattern --

```sh
ASAN_OPTIONS=detect_leaks=1 tur eval --file any.tur
# LeakSanitizer: ... byte(s) leaked ... #1 turi_errorf src/turi/value.c:20 ...
```

A trivial program leaks tens of KB through the interpreter; this is the accepted
model, not a logic bug.

**Spice-side fix:** the notebook spice's test harness should default to
`ASAN_OPTIONS=detect_leaks=0` (or mark the fixtures `requires.no-leak-check`),
mirroring `run-turi.sh` / `run-flags.sh` / `run-build-project.sh`. No turmeric
runtime change is warranted: bounding interpreter allocations would require
adding value-drop discipline to a tree-walker that is intentionally never-free.
