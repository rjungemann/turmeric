# `tur_eval_import` fails: duplicate runtime symbols when autolinking libturi

## Summary

The ctest target `tur_eval_import` (`tests/turi/eval-import.sh`, building
`tests/fixtures/eval-import/input.tur`) fails at the **link** step with
`multiple definition of 'tur_hamt_*'` errors. The fixture's generated C
object and the auto-linked `libturi.a` both define the HAMT runtime
symbols, so the final link is rejected.

**Severity:** Medium -- a hard build/link failure (not a miscompile), but
it is **pre-existing and environment-dependent** (reproduces on a clean
tree with the current toolchain; it is not caused by interpreter logic).
It blocks one ctest target; the rest of the suite (`bash tests/run.sh`)
is unaffected.

## Repro

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j --config Debug
ctest --test-dir build -R tur_eval_import --output-on-failure
```

Observed (abridged):

```
hamt.c:(.text+0xce30): multiple definition of `tur_hamt_merge_with';
    libturi.a(hamt.c.o):src/runtime/hamt.c:1278: first defined here
... (tur_hamt_show, tur_hamt_dump, tur_hamt_transient, ... all duplicated)
collect2: error: ld returned 1 exit status
tur: cc invocation failed (status 256)
```

Confirmed pre-existing: `git stash` of unrelated work + rebuild + rerun
reproduces the identical failure, so it is independent of the
`EX_CONS_LIST` interpreter change landed in the same session.

## Observed vs. expected

- **Observed:** link fails with duplicate `tur_hamt_*` (and likely other
  runtime) symbols.
- **Expected:** the fixture links once against the runtime and runs,
  printing the `expected.stdout`.

## Root-cause analysis

The fixture uses `(import turi/eval)`, which triggers the stdlib fallback
search (`stdlib/turi/eval.tur`) and the `__tur_autolink__` mechanism that
injects `-lturi` into the generated C's link line (see
`tests/turi/eval-import.sh` header comment and `cmd_build`'s autolink
path).

The symbols are defined **twice** at link time:

1. Inside the object compiled from the fixture's generated C (the
   `/tmp/cc*.o` translation unit -- it pulls in the runtime HAMT
   definitions directly), and
2. Inside `libturi.a(hamt.c.o)`, dragged in by the injected `-lturi`.

Modern `ld` defaults to `-fno-common` / rejects multiple strong
definitions, so the duplicate `tur_hamt_*` definitions are a hard error
rather than being silently merged. Older toolchains (or `-fcommon`)
would have tolerated this, which is why it presents as
environment-dependent.

Pointers:
- `tests/turi/eval-import.sh:43` -- the `tur build ... -lturi` invocation.
- `src/runtime/hamt.c:1278` (`tur_hamt_merge_with`) and the other flagged
  lines -- the canonical (library) definitions.
- `CMakeLists.txt:296` -- `tur_eval_import` `TUR_CC_FLAGS` (`-L.../src` so
  the autolinked `libturi.a` is found).

## Proposed fix directions

1. **Don't double-compile the runtime into the generated C** when the
   autolink already pulls `libturi.a`: the generated C for an
   `(import turi/eval)` program should *declare* (extern) the runtime
   symbols and let the library provide the single definition, instead of
   emitting/compiling the runtime translation units inline.
2. Alternatively, make the autolink path and the inline-runtime path
   mutually exclusive (if the runtime is emitted inline, do **not**
   inject `-lturi`, and vice versa).
3. Stop-gap only: link with `-Wl,-z,muldefs` (silently picks the first
   definition) -- masks the real duplication, not recommended as the
   permanent fix.

Direction (1) is the clean fix and matches how every other compiled
program consumes the runtime (one definition, in the library).

## Validation

- `ctest --test-dir build -R tur_eval_import` links and passes.
- `bash tests/run.sh` stays green (this target is a dedicated ctest, not
  part of the fixture sweep).
