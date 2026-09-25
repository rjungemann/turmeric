# Every program fails to link at `-O0`: `tur_set_contract_handler` unresolved

> **RESOLVED 2026-09-25.** `runtime/contract_handler.c` is in
> `TURT_RUNTIME_SOURCES` (`src/CMakeLists.txt`), so `libturt_runtime.a`
> defines both symbols; the repro builds and prints `hi` at `-O0`.  The
> release and Windows archives ship the same CMake target, so they pick it up
> with no layout change.  As with every member of that archive, a program that
> never references the pair never extracts it.


**Severity:** low-medium. `-O2` (the default) links, so the suites never see
it; a developer building with `TUR_CC_FLAGS="-O0 ..."` to debug emitted C
cannot link any program, Turmeric or Scheme.

## Repro

```sh
$ printf '(defn main [] : int (println "hi") 0)\n' > p.tur
$ TUR_CC_FLAGS="-O0 -std=c99 -L$PWD/build/src" tur build p.tur -o p
... undefined reference to `tur_set_contract_handler'
... undefined reference to `tur_get_contract_handler'
```

The link line already carries `-lturt_runtime` (`TUR_SHOW_CC=1`).

## Root cause (lead)

`src/runtime/contract_handler.c` defines both functions, and it is in
`tur_core` (src/CMakeLists.txt, the compiler's own sources) but not in
`TURT_RUNTIME_SOURCES`, the archive a program links. At `-O2` the emitted
static helpers that call them are dropped as unused, so nothing references
the symbols; at `-O0` the helpers stay, and so do the references.

Found verifying r7rs-lang-plan T5 at `-O0`. R6's "tail calls at `-O0`"
claim predates it or used other flags.

## Fix directions

Add `runtime/contract_handler.c` to `TURT_RUNTIME_SOURCES`, and check the
release-archive layout (docs/archive/release-archive-cannot-compile.md) and
the Windows archive, which have hit this file before.
