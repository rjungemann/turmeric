---
title: m3-separate-compilation fails to link -- undefined `some___spec__bool_Option__opaque`
category: Codegen -- separate compilation / monomorphized constructor spec emission
severity: Low-Medium. Auxiliary `tests/run-flags.sh` only; the default
  `tests/run.sh` gate is green. The separate-compilation flag fixture fails at
  the link step with an undefined reference to a monomorphized `some` spec
  symbol, so `-Xseparate-compilation` (or the m3 path it exercises) does not
  emit/define every `some__spec__...` it references across translation units.
status: OPEN (pre-existing; reproduces on a clean checkout of the branch)
---

## Repro

```sh
cmake --build build -j --config Debug
TUR_BIN=./build/tur TUR=./build/tur ASAN_OPTIONS=detect_leaks=0 \
  bash tests/run-flags.sh
```

Observed (clean tree, no local changes):

```
FAIL m3-separate-compilation -- build failed (rc=2): /usr/bin/ld: /tmp/ccXXXXXX.o: in function `some___spec__bool_Option__opaque':
tur: cc invocation failed (status 256)
flags summary: 76 passed, 1 failed
```

## Notes

- Confirmed pre-existing: stashing all local work and rebuilding reproduces the
  identical linker error, so it is unrelated to the interpreter-side fixes
  landed alongside this report.
- The undefined symbol is a monomorphized `some` constructor spec for
  `(Option opaque)` with a `bool`-flavored mangling
  (`some___spec__bool_Option__opaque`). Under separate compilation the
  *referencing* TU emits an extern use but no TU emits the definition (or it is
  emitted `static`/with a divergent mangling in the defining TU).

## Fix directions

1. Find where `some__spec__...` constructor specs are emitted
   (`src/compiler/emit_module.c` / `emit_fns.c`) and confirm that under
   `-Xseparate-compilation` the spec is emitted with external linkage in the TU
   that owns it, and that the referencing TU's extern mangling matches byte for
   byte.
2. Check whether the `bool_` element-mangle component on an `(Option opaque)`
   spec is consistent between the definition site and the call site (a mangling
   divergence would produce exactly this "defined here, referenced there as a
   different name" link error).
